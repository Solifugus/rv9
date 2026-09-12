/*
 * RV-9 I/O manager.
 *
 * Owns path tables and dispatch. Knows nothing about hardware and nothing
 * about line discipline -- it finds the device, checks the mode, and hands
 * off to the file manager. See rv9/io.h for the layering.
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"

#include <string.h>

#include "esp_log.h"

/*
 * Modules are told about failures as negated versions of these, and the
 * numbers are declared separately in rv9/module.h because a module must
 * not include this file. Two lists of the same numbers drift; these keep
 * them from drifting quietly.
 */
_Static_assert((int)RV9_IO_ERR_NOTFOUND    == RV9_IOE_NOTFOUND,    "ABI drift");
_Static_assert((int)RV9_IO_ERR_BADPATH     == RV9_IOE_BADPATH,     "ABI drift");
_Static_assert((int)RV9_IO_ERR_NOPATHS     == RV9_IOE_NOPATHS,     "ABI drift");
_Static_assert((int)RV9_IO_ERR_NOMEM       == RV9_IOE_NOMEM,       "ABI drift");
_Static_assert((int)RV9_IO_ERR_MODE        == RV9_IOE_MODE,        "ABI drift");
_Static_assert((int)RV9_IO_ERR_UNSUPPORTED == RV9_IOE_UNSUPPORTED, "ABI drift");
_Static_assert((int)RV9_IO_ERR_WOULDBLOCK  == RV9_IOE_WOULDBLOCK,  "ABI drift");
_Static_assert((int)RV9_IO_ERR_IO          == RV9_IOE_IO,          "ABI drift");
_Static_assert((int)RV9_IO_ERR_INVAL       == RV9_IOE_INVAL,       "ABI drift");
_Static_assert((int)RV9_IO_ERR_EXISTS      == RV9_IOE_EXISTS,      "ABI drift");
_Static_assert((int)RV9_IO_ERR_TIMEOUT     == RV9_IOE_TIMEOUT,     "ABI drift");

static const char *TAG = "rv9-io";

/* Room to grow. These were sized to what existed at the time, and the
   moment a ninth driver appeared its registration failed -- silently,
   because the caller ignored the error. */
#define MAX_FILEMGRS 8
#define MAX_DRIVERS  16

static const rv9_filemgr_t *s_filemgrs[MAX_FILEMGRS];
static const rv9_driver_t  *s_drivers[MAX_DRIVERS];
static rv9_dev_t           *s_devs;
static rv9_lock_t          s_lock;

/*
 * Per-process path tables. Kept here rather than in the process descriptor
 * so that rv9_proc need not know rv9_io exists; the link is made with hooks.
 */
typedef struct proc_paths {
    rv9_pid_t           pid;
    rv9_path_t         *paths[RV9_MAX_PATHS];
    struct proc_paths  *next;
} proc_paths_t;

static proc_paths_t *s_tables;

/* Paths a process with no parent starts with. */
static char s_sys_in[16];
static char s_sys_out[16];

const char *rv9_io_strerror(rv9_io_err_t err)
{
    switch (err) {
    case RV9_IO_OK:              return "ok";
    case RV9_IO_ERR_NOTFOUND:    return "no such device";
    case RV9_IO_ERR_BADPATH:     return "bad path number";
    case RV9_IO_ERR_NOPATHS:     return "path table full";
    case RV9_IO_ERR_NOMEM:       return "out of memory";
    case RV9_IO_ERR_MODE:        return "wrong mode";
    case RV9_IO_ERR_UNSUPPORTED: return "unsupported";
    case RV9_IO_ERR_WOULDBLOCK:  return "would block";
    case RV9_IO_ERR_IO:          return "device error";
    case RV9_IO_ERR_INVAL:       return "invalid argument";
    case RV9_IO_ERR_EXISTS:      return "already exists";
    case RV9_IO_ERR_TIMEOUT:     return "timed out";
    default:                     return "unknown error";
    }
}

/* ---- registries ---- */

rv9_io_err_t rv9_io_register_filemgr(const rv9_filemgr_t *fm)
{
    if (fm == NULL || fm->name == NULL) return RV9_IO_ERR_INVAL;

    for (int i = 0; i < MAX_FILEMGRS; i++) {
        if (s_filemgrs[i] == NULL) {
            s_filemgrs[i] = fm;
            ESP_LOGI(TAG, "file manager '%s' registered", fm->name);
            return RV9_IO_OK;
        }
        if (strcmp(s_filemgrs[i]->name, fm->name) == 0) return RV9_IO_ERR_EXISTS;
    }

    ESP_LOGE(TAG, "no room for file manager '%s': %d already registered",
             fm->name, MAX_FILEMGRS);
    return RV9_IO_ERR_NOMEM;
}

rv9_io_err_t rv9_io_register_driver(const rv9_driver_t *drv)
{
    if (drv == NULL || drv->name == NULL) return RV9_IO_ERR_INVAL;

    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (s_drivers[i] == NULL) {
            s_drivers[i] = drv;
            ESP_LOGI(TAG, "driver '%s' registered", drv->name);
            return RV9_IO_OK;
        }
        if (strcmp(s_drivers[i]->name, drv->name) == 0) return RV9_IO_ERR_EXISTS;
    }

    ESP_LOGE(TAG, "no room for driver '%s': %d already registered",
             drv->name, MAX_DRIVERS);
    return RV9_IO_ERR_NOMEM;
}

static const rv9_filemgr_t *find_filemgr(const char *name)
{
    for (int i = 0; i < MAX_FILEMGRS && s_filemgrs[i]; i++) {
        if (strcmp(s_filemgrs[i]->name, name) == 0) return s_filemgrs[i];
    }
    return NULL;
}

static const rv9_driver_t *find_driver(const char *name)
{
    for (int i = 0; i < MAX_DRIVERS && s_drivers[i]; i++) {
        if (strcmp(s_drivers[i]->name, name) == 0) return s_drivers[i];
    }
    return NULL;
}

static rv9_dev_t *find_dev(const char *name)
{
    for (rv9_dev_t *d = s_devs; d; d = d->next) {
        if (strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

/* ---- devices ---- */

rv9_io_err_t rv9_io_attach(const rv9_devdesc_t *desc)
{
    if (desc == NULL) return RV9_IO_ERR_INVAL;

    /* The descriptor comes off media, so treat its strings as untrusted. */
    char name[16], fmname[16], drvname[16];
    memcpy(name, desc->name, 16);       name[15] = '\0';
    memcpy(fmname, desc->filemgr, 16);  fmname[15] = '\0';
    memcpy(drvname, desc->driver, 16);  drvname[15] = '\0';

    if (find_dev(name)) return RV9_IO_ERR_EXISTS;

    const rv9_filemgr_t *fm = find_filemgr(fmname);
    if (fm == NULL) {
        ESP_LOGE(TAG, "%s: no file manager '%s'", name, fmname);
        return RV9_IO_ERR_NOTFOUND;
    }

    const rv9_driver_t *drv = find_driver(drvname);
    if (drv == NULL) {
        ESP_LOGE(TAG, "%s: no driver '%s'", name, drvname);
        return RV9_IO_ERR_NOTFOUND;
    }

    rv9_dev_t *dev = rv9_calloc(1, sizeof(*dev));
    if (dev == NULL) return RV9_IO_ERR_NOMEM;

    strncpy(dev->name, name, sizeof(dev->name) - 1);
    dev->fmgr = fm;
    dev->drv  = drv;
    memcpy(dev->opt, desc->opt, sizeof(dev->opt));

    if (drv->init) {
        rv9_io_err_t err = drv->init(dev);
        if (err != RV9_IO_OK) {
            ESP_LOGE(TAG, "%s: driver '%s' init failed: %s",
                     name, drvname, rv9_io_strerror(err));
            rv9_free(dev);
            return err;
        }
    }
    dev->initialised = true;

    /* Give the file manager a chance to mount before anyone can open it. */
    if (fm->mount) {
        rv9_io_err_t err = fm->mount(dev);
        if (err != RV9_IO_OK) {
            ESP_LOGE(TAG, "%s: file manager '%s' could not mount: %s",
                     name, fmname, rv9_io_strerror(err));
            rv9_free(dev);
            return err;
        }
    }

    dev->next = s_devs;
    s_devs = dev;

    ESP_LOGI(TAG, "%-8s %s over %s", dev->name, fm->name, drv->name);
    return RV9_IO_OK;
}

int rv9_io_attach_from_modules(void)
{
    int attached = 0;

    for (const rv9_mod_entry_t *e = rv9_mod_dir_next(NULL);
         e != NULL;
         e = rv9_mod_dir_next(e)) {

        if (e->type != RV9_MOD_DESCRIPTOR) continue;

        rv9_mod_entry_t *linked = NULL;
        if (rv9_mod_link(e->name, &linked) != RV9_MOD_OK) continue;

        const rv9_mod_header_t *h = (const rv9_mod_header_t *)linked->image;
        const uint8_t *body = (const uint8_t *)linked->image + h->entry_offset;

        /* entry_offset points at the body for a data module; the descriptor
           must fit in what remains. */
        if (h->module_len - h->entry_offset >= sizeof(rv9_devdesc_t)) {
            rv9_devdesc_t desc;
            memcpy(&desc, body, sizeof(desc));
            if (rv9_io_attach(&desc) == RV9_IO_OK) attached++;
        } else {
            ESP_LOGW(TAG, "descriptor module '%s' is too short", e->name);
        }

        /* The descriptor has been copied into the device; the module image
           itself is no longer needed. */
        rv9_mod_unlink(linked);
    }

    return attached;
}

const rv9_dev_t *rv9_io_dev_next(const rv9_dev_t *prev)
{
    return prev ? prev->next : s_devs;
}

/* ---- per-process path tables ---- */

static proc_paths_t *table_for(rv9_pid_t pid, bool create)
{
    for (proc_paths_t *t = s_tables; t; t = t->next) {
        if (t->pid == pid) return t;
    }
    if (!create) return NULL;

    proc_paths_t *t = rv9_calloc(1, sizeof(*t));
    if (t == NULL) return NULL;
    t->pid = pid;
    t->next = s_tables;
    s_tables = t;
    return t;
}

/*
 * The path table of whoever is calling.
 *
 * Found once per task and then remembered in task-local storage. Every
 * read and write used to ask the process manager for the caller's pid --
 * taking the process lock and scanning the process list -- and then walk
 * the table list with no lock at all, which was both slower than necessary
 * and a race against anyone creating a table.
 *
 * The cached pointer is safe to keep: a table belongs to a process, is
 * created before that process can run and destroyed after it has stopped,
 * so a task can never observe its own table being freed.
 *
 * This is what makes I/O bounded for real-time work. A control loop moving
 * a servo does a dereference and an index, and never contends with the
 * shell for a lock. Opening and closing a path are *not* bounded -- they
 * allocate, and closing a file on a volume writes to flash -- so a control
 * loop opens what it needs before its first period, not inside the loop.
 */
static RV9_RT_CODE proc_paths_t *my_table(void)
{
    proc_paths_t *t = (proc_paths_t *)rv9_task_local_get();
    if (t != NULL) return t;

    rv9_lock_acquire(s_lock);
    t = table_for(rv9_proc_current_pid(), false);
    rv9_lock_release(s_lock);

    if (t != NULL) rv9_task_local_set(t);
    return t;
}

static RV9_RT_CODE rv9_path_t *path_for_caller(int num)
{
    if (num < 0 || num >= RV9_MAX_PATHS) return NULL;
    proc_paths_t *t = my_table();
    return t ? t->paths[num] : NULL;
}

/* ---- open / close ---- */

/*
 * Split "/r0/notes" into the device "/r0" and the remainder "notes".
 * A name with no second slash is the device itself, and the remainder is
 * empty -- which is how a block file manager knows it is being asked for
 * its directory rather than a file.
 */
static void split_path(const char *full, char *dev_out, size_t dev_len,
                       const char **rest_out)
{
    const char *slash = NULL;
    if (full[0] == '/') slash = strchr(full + 1, '/');

    if (slash == NULL) {
        strncpy(dev_out, full, dev_len - 1);
        dev_out[dev_len - 1] = '\0';
        *rest_out = "";
        return;
    }

    size_t n = (size_t)(slash - full);
    if (n >= dev_len) n = dev_len - 1;
    memcpy(dev_out, full, n);
    dev_out[n] = '\0';
    *rest_out = slash + 1;
}

/*
 * Opening is split in two because a file manager's open may block for a
 * long time -- NFM's listener sits in accept() until someone connects. The
 * descriptor is built and the slot reserved under the lock; the file
 * manager runs outside it.
 *
 * Holding the global I/O lock across a blocking open freezes every other
 * process's I/O, including the shell that is waiting to see what happens.
 */
static rv9_path_t *path_new(rv9_dev_t *dev, uint32_t mode)
{
    rv9_path_t *p = rv9_calloc(1, sizeof(*p));
    if (p == NULL) return NULL;

    p->dev  = dev;
    p->mode = mode;
    p->pos  = 0;
    p->refs = 1;
    return p;
}

/* Must be called with the lock NOT held. */
static rv9_io_err_t path_open(rv9_path_t *p, const char *rest)
{
    if (p->dev->fmgr->open == NULL) return RV9_IO_OK;
    return p->dev->fmgr->open(p, rest);
}

/*
 * Tell the driver when its device starts and stops being used.
 *
 * Both are called with the lock released, because a driver that wants to
 * know may also want to take its time -- `ssh` spends the first open
 * waiting for somebody to connect. Holding the I/O lock across that would
 * stop every other process from opening anything.
 */
static rv9_io_err_t dev_first_open(rv9_dev_t *dev, uint32_t mode)
{
    if (dev->drv->open == NULL) return RV9_IO_OK;
    return dev->drv->open(dev, mode);
}

/*
 * May this device be opened again while somebody already has it?
 *
 * For an ordinary device, yes: every process opens /term and they share
 * it. For one whose driver does work on first open, no -- that work is a
 * session, and a session belongs to whoever established it. A second
 * opener cannot have one of its own (there is one device) and must not
 * silently join somebody else's.
 *
 * Getting this wrong was visible rather than subtle: a second sshd found
 * the count already raised by the first, skipped the handshake it thought
 * had happened, and handed its shell a device with no session behind it.
 * The shell read, failed, exited, and the daemon did it again as fast as
 * it could.
 *
 * Sharing still works the way it always did -- dup2 and fork raise the
 * reference count and never come through here.
 */
static bool dev_open_refused(const rv9_dev_t *dev)
{
    return dev->drv->open != NULL && dev->open_count > 0;
}

static void dev_last_close(rv9_dev_t *dev)
{
    if (dev->drv->close) dev->drv->close(dev);
}

static rv9_path_t *path_alloc(rv9_dev_t *dev, const char *rest, uint32_t mode,
                              rv9_io_err_t *err)
{
    if (dev_open_refused(dev)) { *err = RV9_IO_ERR_EXISTS; return NULL; }

    rv9_path_t *p = path_new(dev, mode);
    if (p == NULL) { *err = RV9_IO_ERR_NOMEM; return NULL; }

    bool first = (dev->open_count == 0);

    rv9_io_err_t e = first ? dev_first_open(dev, mode) : RV9_IO_OK;
    if (e == RV9_IO_OK) e = path_open(p, rest);

    if (e != RV9_IO_OK) {
        if (first) dev_last_close(dev);
        rv9_free(p);
        *err = e;
        return NULL;
    }

    dev->open_count++;
    *err = RV9_IO_OK;
    return p;
}

/*
 * Drop a reference, and say whether that left the device idle.
 *
 * The driver is *not* told here, because this runs with the I/O lock held
 * and telling it may mean closing a path of its own -- which takes the
 * same lock. The caller releases the lock and then calls dev_last_close
 * with what came back.
 */
static void path_release(rv9_path_t *p, rv9_dev_t **idle)
{
    if (idle) *idle = NULL;
    if (p == NULL) return;
    if (--p->refs > 0) return;

    rv9_dev_t *dev = p->dev;

    if (dev->fmgr->close) dev->fmgr->close(p);
    if (dev->open_count) dev->open_count--;
    if (dev->open_count == 0 && idle) *idle = dev;

    rv9_free(p);
}

int rv9_io_open(const char *name, uint32_t mode)
{
    if (name == NULL || (mode & RV9_MODE_RW) == 0) return -RV9_IO_ERR_INVAL;

    char devname[16];
    const char *rest = "";
    split_path(name, devname, sizeof(devname), &rest);

    rv9_lock_acquire(s_lock);

    rv9_dev_t *dev = find_dev(devname);
    if (dev == NULL) {
        rv9_lock_release(s_lock);
        return -RV9_IO_ERR_NOTFOUND;
    }
    if (dev_open_refused(dev)) {
        rv9_lock_release(s_lock);
        return -RV9_IO_ERR_EXISTS;
    }

    rv9_pid_t pid = rv9_proc_current_pid();
    proc_paths_t *t = table_for(pid, true);
    if (t == NULL) {
        rv9_lock_release(s_lock);
        return -RV9_IO_ERR_NOMEM;
    }

    /* Warm the cache here, where blocking is allowed. A real-time process
       opens its pin before it declares a period, so by the time a deadline
       exists the lookup below is a load and nothing else. */
    rv9_task_local_set(t);

    int num = -1;
    for (int i = 0; i < RV9_MAX_PATHS; i++) {
        if (t->paths[i] == NULL) { num = i; break; }
    }
    if (num < 0) {
        rv9_lock_release(s_lock);
        return -RV9_IO_ERR_NOPATHS;
    }

    rv9_path_t *p = path_new(dev, mode);
    if (p == NULL) {
        rv9_lock_release(s_lock);
        return -RV9_IO_ERR_NOMEM;
    }

    /* Reserve the slot so a concurrent open in this process cannot take it,
       then let go of the lock: what follows may block for a long time. */
    t->paths[num] = p;
    bool first = (dev->open_count == 0);
    dev->open_count++;
    rv9_lock_release(s_lock);

    rv9_io_err_t err = first ? dev_first_open(dev, mode) : RV9_IO_OK;
    if (err == RV9_IO_OK) err = path_open(p, rest);

    if (err != RV9_IO_OK) {
        if (first) dev_last_close(dev);
        rv9_lock_acquire(s_lock);
        t->paths[num] = NULL;
        if (dev->open_count) dev->open_count--;
        rv9_lock_release(s_lock);
        rv9_free(p);
        return -err;
    }

    return num;
}

rv9_io_err_t rv9_io_close(int num)
{
    rv9_lock_acquire(s_lock);

    /* table_for, not my_table: the cache filler takes this same lock, and it
       does not nest. */
    proc_paths_t *t = table_for(rv9_proc_current_pid(), false);
    if (t == NULL || num < 0 || num >= RV9_MAX_PATHS || t->paths[num] == NULL) {
        rv9_lock_release(s_lock);
        return RV9_IO_ERR_BADPATH;
    }

    rv9_dev_t *idle = NULL;
    path_release(t->paths[num], &idle);
    t->paths[num] = NULL;

    rv9_lock_release(s_lock);

    if (idle) dev_last_close(idle);
    return RV9_IO_OK;
}

/* ---- transfers ---- */

/*
 * Reads and writes are RV9_RT_CODE: a control loop runs them with a
 * deadline pending, so they must still be there when the flash cache is
 * not. What they call is not automatically resident -- SCF ends up in the
 * USB driver and RBF in the flash driver, neither of which can run then --
 * so this guarantee reaches as far as PIO and its peripheral drivers,
 * which is where control loops actually go. See RV9_RT_CODE in rv9/kal.h.
 */
RV9_RT_CODE rv9_io_err_t rv9_io_read(int num, void *buf, size_t len, size_t *done)
{
    if (buf == NULL) return RV9_IO_ERR_INVAL;
    if (done) *done = 0;

    rv9_path_t *p = path_for_caller(num);
    if (p == NULL) return RV9_IO_ERR_BADPATH;
    if (!(p->mode & RV9_MODE_READ)) return RV9_IO_ERR_MODE;
    if (p->dev->fmgr->read == NULL) return RV9_IO_ERR_UNSUPPORTED;

    size_t moved = 0;
    rv9_io_err_t err = p->dev->fmgr->read(p, buf, len, &moved);
    if (done) *done = moved;
    return err;
}

RV9_RT_CODE rv9_io_err_t rv9_io_write(int num, const void *buf, size_t len,
                                      size_t *done)
{
    if (buf == NULL) return RV9_IO_ERR_INVAL;
    if (done) *done = 0;

    rv9_path_t *p = path_for_caller(num);
    if (p == NULL) return RV9_IO_ERR_BADPATH;
    if (!(p->mode & RV9_MODE_WRITE)) return RV9_IO_ERR_MODE;
    if (p->dev->fmgr->write == NULL) return RV9_IO_ERR_UNSUPPORTED;

    size_t moved = 0;
    rv9_io_err_t err = p->dev->fmgr->write(p, buf, len, &moved);
    if (done) *done = moved;
    return err;
}

/*
 * Point path `to` at whatever `from` refers to, sharing one path
 * descriptor. This is how redirection works: the shell aims its own stdout
 * elsewhere, forks -- the child inherits the redirected path -- then puts
 * its stdout back.
 */
rv9_io_err_t rv9_io_dup2(int from, int to)
{
    if (from == to) return RV9_IO_OK;
    if (to < 0 || to >= RV9_MAX_PATHS) return RV9_IO_ERR_BADPATH;

    rv9_lock_acquire(s_lock);

    proc_paths_t *t = table_for(rv9_proc_current_pid(), true);
    if (t != NULL) rv9_task_local_set(t);
    if (t == NULL || from < 0 || from >= RV9_MAX_PATHS ||
        t->paths[from] == NULL) {
        rv9_lock_release(s_lock);
        return RV9_IO_ERR_BADPATH;
    }

    rv9_dev_t *idle = NULL;
    if (t->paths[to] != NULL) path_release(t->paths[to], &idle);

    t->paths[from]->refs++;
    t->paths[to] = t->paths[from];

    rv9_lock_release(s_lock);

    if (idle) dev_last_close(idle);
    return RV9_IO_OK;
}

rv9_io_err_t rv9_io_remove(const char *name)
{
    if (name == NULL) return RV9_IO_ERR_INVAL;

    char devname[16];
    const char *rest = "";
    split_path(name, devname, sizeof(devname), &rest);

    rv9_lock_acquire(s_lock);
    rv9_dev_t *dev = find_dev(devname);
    rv9_lock_release(s_lock);

    if (dev == NULL) return RV9_IO_ERR_NOTFOUND;
    if (dev->fmgr->remove == NULL) return RV9_IO_ERR_UNSUPPORTED;
    if (rest[0] == '\0') return RV9_IO_ERR_INVAL;

    return dev->fmgr->remove(dev, rest);
}

rv9_io_err_t rv9_io_puts(int num, const char *s)
{
    if (s == NULL) return RV9_IO_ERR_INVAL;
    return rv9_io_write(num, s, strlen(s), NULL);
}

rv9_io_err_t rv9_io_seek(int num, int64_t offset, int whence)
{
    rv9_path_t *p = path_for_caller(num);
    if (p == NULL) return RV9_IO_ERR_BADPATH;
    if (p->dev->fmgr->seek == NULL) return RV9_IO_ERR_UNSUPPORTED;
    return p->dev->fmgr->seek(p, offset, whence);
}

rv9_io_err_t rv9_io_getstat(int num, uint32_t code, void *arg)
{
    rv9_path_t *p = path_for_caller(num);
    if (p == NULL) return RV9_IO_ERR_BADPATH;
    if (p->dev->fmgr->getstat == NULL) return RV9_IO_ERR_UNSUPPORTED;
    return p->dev->fmgr->getstat(p, code, arg);
}

rv9_io_err_t rv9_io_setstat(int num, uint32_t code, void *arg)
{
    rv9_path_t *p = path_for_caller(num);
    if (p == NULL) return RV9_IO_ERR_BADPATH;
    if (p->dev->fmgr->setstat == NULL) return RV9_IO_ERR_UNSUPPORTED;
    return p->dev->fmgr->setstat(p, code, arg);
}

/* ---- detached paths: a path held by a driver rather than a process ---- */

rv9_io_err_t rv9_io_open_detached(const char *name, uint32_t mode,
                                  rv9_path_t **out)
{
    if (name == NULL || out == NULL || (mode & RV9_MODE_RW) == 0) {
        return RV9_IO_ERR_INVAL;
    }

    char devname[16];
    const char *rest = "";
    split_path(name, devname, sizeof(devname), &rest);

    rv9_lock_acquire(s_lock);

    rv9_dev_t *dev = find_dev(devname);
    if (dev == NULL) {
        rv9_lock_release(s_lock);
        return RV9_IO_ERR_NOTFOUND;
    }
    if (dev_open_refused(dev)) {
        rv9_lock_release(s_lock);
        return RV9_IO_ERR_EXISTS;
    }

    rv9_path_t *p = path_new(dev, mode);
    if (p == NULL) {
        rv9_lock_release(s_lock);
        return RV9_IO_ERR_NOMEM;
    }

    bool first = (dev->open_count == 0);
    dev->open_count++;
    rv9_lock_release(s_lock);

    /* Same two-stage shape as rv9_io_open, and for the same reason: what
       follows may sit waiting for a connection. */
    rv9_io_err_t err = first ? dev_first_open(dev, mode) : RV9_IO_OK;
    if (err == RV9_IO_OK) err = path_open(p, rest);

    if (err != RV9_IO_OK) {
        if (first) dev_last_close(dev);
        rv9_lock_acquire(s_lock);
        if (dev->open_count) dev->open_count--;
        rv9_lock_release(s_lock);
        rv9_free(p);
        return err;
    }

    *out = p;
    return RV9_IO_OK;
}

rv9_io_err_t rv9_io_read_path(rv9_path_t *p, void *buf, size_t len, size_t *done)
{
    if (done) *done = 0;
    if (p == NULL || buf == NULL) return RV9_IO_ERR_INVAL;
    if (!(p->mode & RV9_MODE_READ)) return RV9_IO_ERR_MODE;
    if (p->dev->fmgr->read == NULL) return RV9_IO_ERR_UNSUPPORTED;

    size_t moved = 0;
    rv9_io_err_t err = p->dev->fmgr->read(p, buf, len, &moved);
    if (done) *done = moved;
    return err;
}

rv9_io_err_t rv9_io_write_path(rv9_path_t *p, const void *buf, size_t len,
                               size_t *done)
{
    if (done) *done = 0;
    if (p == NULL || buf == NULL) return RV9_IO_ERR_INVAL;
    if (!(p->mode & RV9_MODE_WRITE)) return RV9_IO_ERR_MODE;
    if (p->dev->fmgr->write == NULL) return RV9_IO_ERR_UNSUPPORTED;

    size_t moved = 0;
    rv9_io_err_t err = p->dev->fmgr->write(p, buf, len, &moved);
    if (done) *done = moved;
    return err;
}

rv9_io_err_t rv9_io_setstat_path(rv9_path_t *p, uint32_t code, void *arg)
{
    if (p == NULL) return RV9_IO_ERR_INVAL;
    if (p->dev->fmgr->setstat == NULL) return RV9_IO_ERR_UNSUPPORTED;
    return p->dev->fmgr->setstat(p, code, arg);
}

void rv9_io_close_path(rv9_path_t *p)
{
    if (p == NULL) return;

    rv9_dev_t *idle = NULL;
    rv9_lock_acquire(s_lock);
    path_release(p, &idle);
    rv9_lock_release(s_lock);

    if (idle) dev_last_close(idle);
}

/* ---- process lifecycle ---- */

/*
 * A child shares its parent's standard paths rather than reopening them.
 * That is what makes redirection work in a shell: the parent opens the
 * destination, forks, and the child writes to it without knowing.
 */
static void io_on_fork(rv9_pid_t parent, rv9_pid_t child)
{
    rv9_lock_acquire(s_lock);

    proc_paths_t *ct = table_for(child, true);
    if (ct == NULL) {
        rv9_lock_release(s_lock);
        return;
    }

    proc_paths_t *pt = (parent != RV9_PID_NONE) ? table_for(parent, false) : NULL;

    if (pt != NULL) {
        for (int i = 0; i < 3 && i < RV9_MAX_PATHS; i++) {
            if (pt->paths[i] == NULL) continue;
            pt->paths[i]->refs++;
            ct->paths[i] = pt->paths[i];
        }
        rv9_lock_release(s_lock);
        return;
    }

    /* No parent: give the child the system standard paths. */
    rv9_dev_t *in  = s_sys_in[0]  ? find_dev(s_sys_in)  : NULL;
    rv9_dev_t *out = s_sys_out[0] ? find_dev(s_sys_out) : NULL;
    rv9_io_err_t err;

    if (in)  ct->paths[RV9_STDIN]  = path_alloc(in,  "", RV9_MODE_READ,  &err);
    if (out) {
        ct->paths[RV9_STDOUT] = path_alloc(out, "", RV9_MODE_WRITE, &err);
        if (ct->paths[RV9_STDOUT]) {
            ct->paths[RV9_STDOUT]->refs++;
            ct->paths[RV9_STDERR] = ct->paths[RV9_STDOUT];
        }
    }

    rv9_lock_release(s_lock);
}

static void io_on_exit(rv9_pid_t pid)
{
    /* Devices this process was the last user of. Told after the lock goes,
       because a driver being told may close a path of its own. */
    rv9_dev_t *idle[RV9_MAX_PATHS];
    int nidle = 0;

    rv9_lock_acquire(s_lock);

    proc_paths_t **pp = &s_tables;
    while (*pp && (*pp)->pid != pid) pp = &(*pp)->next;

    if (*pp) {
        proc_paths_t *t = *pp;
        for (int i = 0; i < RV9_MAX_PATHS; i++) {
            rv9_dev_t *d = NULL;
            path_release(t->paths[i], &d);
            if (d) idle[nidle++] = d;
            t->paths[i] = NULL;
        }
        *pp = t->next;

        /* The exiting process is the caller here, so clear its cached
           pointer before the table goes: a thread slot that gets reused
           must not inherit a pointer to freed memory. */
        if (rv9_task_local_get() == t) rv9_task_local_set(NULL);

        rv9_free(t);
    }

    rv9_lock_release(s_lock);

    for (int i = 0; i < nidle; i++) dev_last_close(idle[i]);
}

rv9_io_err_t rv9_io_set_system_std(const char *in, const char *out)
{
    if (in)  { strncpy(s_sys_in,  in,  sizeof(s_sys_in) - 1); }
    if (out) { strncpy(s_sys_out, out, sizeof(s_sys_out) - 1); }
    return RV9_IO_OK;
}

/*
 * Adapter so modules can do I/O. rv9_module must not depend on rv9_io, so
 * the I/O manager hands itself over as function pointers instead.
 * Errors come back negative; successes are byte counts or path numbers.
 */
static int io_open_op(const char *name, uint32_t mode)
{
    return rv9_io_open(name, mode);
}

static int io_close_op(int path)
{
    rv9_io_err_t err = rv9_io_close(path);
    return (err == RV9_IO_OK) ? 0 : -(int)err;
}

static RV9_RT_CODE int io_read_op(int path, void *buf, uint32_t len)
{
    size_t done = 0;
    rv9_io_err_t err = rv9_io_read(path, buf, len, &done);
    return (err == RV9_IO_OK) ? (int)done : -(int)err;
}

static RV9_RT_CODE int io_write_op(int path, const void *buf, uint32_t len)
{
    size_t done = 0;
    rv9_io_err_t err = rv9_io_write(path, buf, len, &done);
    return (err == RV9_IO_OK) ? (int)done : -(int)err;
}

static int io_seek_op(int path, int32_t offset, int whence)
{
    rv9_io_err_t err = rv9_io_seek(path, offset, whence);
    return (err == RV9_IO_OK) ? 0 : -(int)err;
}

static int io_getstat_op(int path, uint32_t code, void *arg)
{
    rv9_io_err_t err = rv9_io_getstat(path, code, arg);
    return (err == RV9_IO_OK) ? 0 : -(int)err;
}

static int io_setstat_op(int path, uint32_t code, void *arg)
{
    rv9_io_err_t err = rv9_io_setstat(path, code, arg);
    return (err == RV9_IO_OK) ? 0 : -(int)err;
}

static int io_remove_op(const char *name)
{
    rv9_io_err_t err = rv9_io_remove(name);
    return (err == RV9_IO_OK) ? 0 : -(int)err;
}

static int io_dup2_op(int from, int to)
{
    rv9_io_err_t err = rv9_io_dup2(from, to);
    return (err == RV9_IO_OK) ? 0 : -(int)err;
}

static const rv9_mod_io_ops_t s_mod_io_ops = {
    .open  = io_open_op,
    .close = io_close_op,
    .read  = io_read_op,
    .write = io_write_op,
    .dup2   = io_dup2_op,
    .remove = io_remove_op,
    .seek    = io_seek_op,
    .getstat = io_getstat_op,
    .setstat = io_setstat_op,
};

rv9_io_err_t rv9_io_init(void)
{
    if (s_lock != NULL) return RV9_IO_OK;
    if (rv9_lock_create(&s_lock) != RV9_OK) return RV9_IO_ERR_NOMEM;

    rv9_proc_set_hooks(io_on_fork, io_on_exit);
    rv9_mod_set_io_ops(&s_mod_io_ops);
    ESP_LOGI(TAG, "I/O manager up");
    return RV9_IO_OK;
}
