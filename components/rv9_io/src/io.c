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
_Static_assert((int)RV9_IO_ERR_BUSY        == RV9_IOE_BUSY,        "ABI drift");

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
    case RV9_IO_ERR_BUSY:        return "owned by another process";
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
/*
 * Ownership is settled here, before the file manager or the driver hears
 * about the open at all. That order is deliberate: a refusal must cost
 * nothing, and by the time a driver has configured an LEDC channel it has
 * already begun driving the pin the second opener was going to be told it
 * could not have.
 */
static rv9_path_t *path_new(rv9_dev_t *dev, const char *rest, uint32_t mode,
                            rv9_pid_t owner, rv9_io_err_t *err)
{
    char resource[RV9_CLAIM_NAME_MAX];
    rv9_claim_resource(resource, sizeof(resource), dev->name, rest);

    struct rv9_claim *claim = NULL;
    rv9_io_err_t e = rv9_claim_take(resource, owner,
                                    (mode & RV9_MODE_EXCL) != 0, &claim);
    if (e != RV9_IO_OK) { *err = e; return NULL; }

    rv9_path_t *p = rv9_calloc(1, sizeof(*p));
    if (p == NULL) {
        rv9_claim_drop(claim);
        *err = RV9_IO_ERR_NOMEM;
        return NULL;
    }

    p->dev   = dev;
    p->mode  = mode;
    p->pos   = 0;
    p->refs  = 1;
    p->claim = claim;

    *err = RV9_IO_OK;
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

/* Let go of a path that never became one, or has finished being one. */
static void path_free(rv9_path_t *p)
{
    if (p == NULL) return;
    rv9_claim_drop(p->claim);
    rv9_free(p);
}

static rv9_path_t *path_alloc(rv9_dev_t *dev, const char *rest, uint32_t mode,
                              rv9_pid_t owner, rv9_io_err_t *err)
{
    if (dev_open_refused(dev)) { *err = RV9_IO_ERR_EXISTS; return NULL; }

    rv9_path_t *p = path_new(dev, rest, mode, owner, err);
    if (p == NULL) return NULL;

    bool first = (dev->open_count == 0);

    rv9_io_err_t e = first ? dev_first_open(dev, mode) : RV9_IO_OK;
    if (e == RV9_IO_OK) e = path_open(p, rest);

    if (e != RV9_IO_OK) {
        if (first) dev_last_close(dev);
        path_free(p);
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

    path_free(p);
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

    rv9_io_err_t cerr = RV9_IO_OK;
    rv9_path_t *p = path_new(dev, rest, mode, pid, &cerr);
    if (p == NULL) {
        rv9_lock_release(s_lock);
        return -(int)cerr;
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
        path_free(p);
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

    /*
     * A detached path is the system's, not any process's. Owning it under
     * whichever pid happened to call would be wrong twice: the hold has to
     * outlive that process, and no process exit should be able to take a
     * driver's connection away from it.
     */
    rv9_io_err_t cerr = RV9_IO_OK;
    rv9_path_t *p = path_new(dev, rest, mode, RV9_PID_NONE, &cerr);
    if (p == NULL) {
        rv9_lock_release(s_lock);
        return cerr;
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
        path_free(p);
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

    if (in)  ct->paths[RV9_STDIN]  = path_alloc(in,  "", RV9_MODE_READ,
                                                child, &err);
    if (out) {
        ct->paths[RV9_STDOUT] = path_alloc(out, "", RV9_MODE_WRITE,
                                           child, &err);
        if (ct->paths[RV9_STDOUT]) {
            ct->paths[RV9_STDOUT]->refs++;
            ct->paths[RV9_STDERR] = ct->paths[RV9_STDOUT];
        }
    }

    rv9_lock_release(s_lock);
}

/*
 * Leave this process's actuators where it said they should be left.
 *
 * Run after everything of its own is closed and released, and through a
 * fresh detached open, because the alternative is worse than it looks: to
 * write through the dying process's own path we would have to keep that
 * path alive, and the process this is for is frequently one that ran off
 * its stack. Nothing here touches anything the dead program owned except
 * the device itself.
 *
 * A device that does not hold its state past release (see rv9_driver_t
 * .retains) is left where its driver leaves it -- /pwm0 stops driving,
 * which is its own declared safe state. The value is still written, both
 * because it is what was promised and because the device may be reopened
 * before anything else touches it.
 */
#define MAX_FAILSAFES 8

static void apply_failsafes(const rv9_claim_fs_t *fs, int n)
{
    for (int i = 0; i < n; i++) {
        rv9_path_t *p = NULL;
        rv9_io_err_t err = rv9_io_open_detached(fs[i].name, RV9_MODE_WRITE, &p);
        if (err != RV9_IO_OK) {
            ESP_LOGE(TAG, "failsafe: cannot open %s to park it: %s",
                     fs[i].name, rv9_io_strerror(err));
            continue;
        }

        size_t done = 0;
        uint32_t v = fs[i].value;
        err = rv9_io_write_path(p, &v, sizeof(v), &done);
        rv9_io_close_path(p);

        if (err != RV9_IO_OK) {
            ESP_LOGE(TAG, "failsafe: cannot park %s at %lu: %s", fs[i].name,
                     (unsigned long)v, rv9_io_strerror(err));
        } else {
            ESP_LOGW(TAG, "failsafe: %s left at %lu", fs[i].name,
                     (unsigned long)v);
        }
    }
}

/*
 * Tell every file manager that cares that a process has ended.
 *
 * Devices are only ever added, at the head, so the list can be walked
 * without the I/O lock once its head is read -- and must be, because a
 * manager being told takes locks of its own.
 */
static void notify_ended(rv9_pid_t pid, int fault)
{
    rv9_lock_acquire(s_lock);
    rv9_dev_t *list = s_devs;
    rv9_lock_release(s_lock);

    for (rv9_dev_t *d = list; d != NULL; d = d->next) {
        if (d->fmgr != NULL && d->fmgr->ended != NULL) {
            d->fmgr->ended(d, pid, fault);
        }
    }
}

/* From the process manager, once the table says how a process ended. */
static void io_on_ended(rv9_pid_t pid, int fault)
{
    if (fault != RV9_FAULT_NONE) notify_ended(pid, fault);
}

static void io_on_exit(rv9_pid_t pid)
{
    /* Devices this process was the last user of. Told after the lock goes,
       because a driver being told may close a path of its own. */
    rv9_dev_t *idle[RV9_MAX_PATHS];
    int nidle = 0;

    /* Taken before anything is released, because releasing is what frees
       the records these live on. */
    rv9_claim_fs_t fs[MAX_FAILSAFES];
    int nfs = rv9_claim_failsafes(pid, fs, MAX_FAILSAFES);
    if (nfs > MAX_FAILSAFES) nfs = MAX_FAILSAFES;

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

    /*
     * Whatever it reserved at fork, whether or not it ever opened it, and
     * whether it returned or was stopped by the scheduler. This is the
     * clause that matters on a machine that moves: a program does not get
     * to keep the motor by dying.
     *
     * Outside the lock because nothing here needs it, and the claim table
     * has one of its own.
     */
    rv9_claim_release_pid(pid);

    for (int i = 0; i < nidle; i++) dev_last_close(idle[i]);

    /*
     * Last, and on every path out of a process rather than only the bad
     * ones.
     *
     * The distinction between orderly stop and failure belongs to the
     * program: R9's `on stop` runs while the module is still healthy, so
     * it has already happened by the time RV-9 sees an exit at all. What
     * is left here is the same question either way -- this device had an
     * owner, it no longer does, and the owner said where to leave it.
     * Applying it only on faults would mean the safety path is the one
     * that almost never runs.
     */
    if (nfs > 0) apply_failsafes(fs, nfs);

    /* And what it reserved inside file managers -- a declared publication.
       On every path out, including a fork refused halfway through
       admission, which is also how a partial reservation is undone. */
    notify_ended(pid, RV9_FAULT_NONE);
}

/*
 * What a program declared it needs, checked and claimed before it starts.
 *
 * Called by the process manager at fork -- through a hook, because rv9_proc
 * must not know rv9_io exists. Two tags, and they fail differently on
 * purpose:
 *
 *   device      it needs this, shared. Checked for existence only. A
 *               program that names a device this machine does not have is
 *               not going to work on it, and finding that out at fork is
 *               better than finding out at the first open, three seconds
 *               into a startup sequence.
 *   exclusive   it needs this alone. Claimed for the process's whole life.
 *
 * Both are refusals before anything is allocated. A partial claim is undone
 * by the caller: it releases the pid on any failure, which drops whatever
 * was taken before the one that failed.
 *
 * Answers in the process manager's vocabulary rather than its own, because
 * this is a fork failing and not an open failing, and the person reading
 * the message is being told why their program did not start.
 */
/* Does the device behind this path hold its state once released? */
static bool device_retains(const char *resource)
{
    char devname[16];
    const char *rest = "";
    split_path(resource, devname, sizeof(devname), &rest);

    rv9_lock_acquire(s_lock);
    rv9_dev_t *dev = find_dev(devname);
    bool retains = (dev != NULL && dev->drv->retains);
    rv9_lock_release(s_lock);

    return retains;
}

/*
 * What this program promises to leave its actuators at.
 *
 * Read after the exclusives, and only after, because a failsafe is
 * recorded against an ownership record that must already exist. A program
 * that names a device it did not claim is contradicting its own manifest
 * -- it has promised to park something it never asked to own -- so the
 * refusal is CONTRACT rather than BUSY: the fix is a build.conf line, not
 * stopping something else.
 */
static int claim_failsafes(rv9_pid_t pid, const void *image,
                           const char *progname)
{
    const void *v = NULL;
    uint16_t len = 0;

    while ((v = rv9_mod_manifest_find(image, RV9_MTAG_FAILSAFE, v,
                                      &len)) != NULL) {
        if (len < RV9_FAILSAFE_MIN_LEN) {
            ESP_LOGE(TAG, "admit '%s': a failsafe entry is malformed",
                     progname);
            return RV9_PROC_ERR_CONTRACT;
        }

        uint32_t value;
        memcpy(&value, v, sizeof(value));       /* may be unaligned in flash */

        char res[RV9_CLAIM_NAME_MAX];
        size_t n = (size_t)len - sizeof(value);
        if (n > sizeof(res) - 1) n = sizeof(res) - 1;
        memcpy(res, (const uint8_t *)v + sizeof(value), n);
        res[n] = '\0';

        if (rv9_claim_failsafe(res, pid, value) != RV9_IO_OK) {
            ESP_LOGE(TAG, "admit '%s': it promises to leave %s at %lu, but "
                          "never claimed it", progname, res,
                     (unsigned long)value);
            return RV9_PROC_ERR_CONTRACT;
        }

        /*
         * Say so when the promise cannot be kept past the last close.
         *
         * Not a refusal: the value still holds while the program is alive
         * and during the window before the device is released, and the
         * driver's own release behaviour is a safe state in its own
         * right. But a program whose author believes the servo will stay
         * where it was parked should find that out from the log rather
         * than from the servo.
         */
        if (!device_retains(res)) {
            ESP_LOGW(TAG, "admit '%s': %s does not hold its state when "
                          "released; its failsafe lasts only until then",
                     progname, res);
        }
    }

    return RV9_PROC_OK;
}

static int io_claim_for_fork(rv9_pid_t pid, const void *image,
                             const char *progname)
{
    static const struct { uint16_t tag; bool exclusive; } want[] = {
        { RV9_MTAG_DEVICE,    false },
        { RV9_MTAG_EXCLUSIVE, true  },
    };

    for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        const void *v = NULL;
        uint16_t len = 0;

        while ((v = rv9_mod_manifest_find(image, want[k].tag, v, &len)) != NULL) {
            /* Manifest strings carry their length and are not terminated. */
            char res[RV9_CLAIM_NAME_MAX];
            size_t n = (len < sizeof(res) - 1) ? len : sizeof(res) - 1;
            memcpy(res, v, n);
            res[n] = '\0';
            if (n == 0) continue;

            char devname[16];
            const char *rest = "";
            split_path(res, devname, sizeof(devname), &rest);

            rv9_lock_acquire(s_lock);
            bool exists = (find_dev(devname) != NULL);
            rv9_lock_release(s_lock);

            if (!exists) {
                ESP_LOGE(TAG, "admit '%s': it needs %s, which this machine "
                              "does not have", progname, res);
                return RV9_PROC_ERR_NODEV;
            }

            if (!want[k].exclusive) continue;

            rv9_io_err_t err = rv9_claim_reserve(res, pid, true);
            if (err == RV9_IO_ERR_BUSY) {
                rv9_pid_t owner = RV9_PID_NONE;
                if (rv9_claim_owner(res, &owner, NULL)) {
                    ESP_LOGE(TAG, "admit '%s': it needs %s alone, and pid %u "
                                  "has it", progname, res, (unsigned)owner);
                }
                return RV9_PROC_ERR_BUSY;
            }
            if (err != RV9_IO_OK) return RV9_PROC_ERR_NOMEM;
        }
    }

    /*
     * Publications it says it makes, and those it says it reads.
     *
     * After the devices, so that a publication on a device the machine
     * lacks is refused as the device it is; before the failsafes, which
     * are about actuators and have nothing to do with these.
     */
    const void *v = NULL;
    uint16_t len = 0;

    while ((v = rv9_mod_manifest_find(image, RV9_MTAG_PUBLISHES, v, &len))
           != NULL) {
        char res[RV9_CLAIM_NAME_MAX];
        size_t n = (len < sizeof(res) - 1) ? len : sizeof(res) - 1;
        memcpy(res, v, n);
        res[n] = '\0';
        if (n == 0) continue;

        char devname[16];
        const char *rest = "";
        split_path(res, devname, sizeof(devname), &rest);

        rv9_lock_acquire(s_lock);
        rv9_dev_t *dev = find_dev(devname);
        rv9_lock_release(s_lock);

        if (dev == NULL) {
            ESP_LOGE(TAG, "admit '%s': it publishes %s, on a device this "
                          "machine does not have", progname, res);
            return RV9_PROC_ERR_NODEV;
        }
        if (dev->fmgr->reserve_writer == NULL) {
            ESP_LOGE(TAG, "admit '%s': %s is not something that can be "
                          "published", progname, res);
            return RV9_PROC_ERR_CONTRACT;
        }

        rv9_io_err_t err = dev->fmgr->reserve_writer(dev, rest, pid);
        if (err == RV9_IO_ERR_BUSY)  return RV9_PROC_ERR_BUSY;
        if (err == RV9_IO_ERR_NOMEM) return RV9_PROC_ERR_NOMEM;
        if (err != RV9_IO_OK) {
            ESP_LOGE(TAG, "admit '%s': cannot publish %s: %s", progname, res,
                     rv9_io_strerror(err));
            return RV9_PROC_ERR_CONTRACT;
        }
    }

    v = NULL;
    while ((v = rv9_mod_manifest_find(image, RV9_MTAG_WATCHES, v, &len))
           != NULL) {
        char res[RV9_CLAIM_NAME_MAX];
        size_t n = (len < sizeof(res) - 1) ? len : sizeof(res) - 1;
        memcpy(res, v, n);
        res[n] = '\0';
        if (n == 0) continue;

        char devname[16];
        const char *rest = "";
        split_path(res, devname, sizeof(devname), &rest);

        rv9_lock_acquire(s_lock);
        rv9_dev_t *dev = find_dev(devname);
        rv9_lock_release(s_lock);

        if (dev == NULL) {
            ESP_LOGE(TAG, "admit '%s': it watches %s, on a device this "
                          "machine does not have", progname, res);
            return RV9_PROC_ERR_NODEV;
        }
        if (dev->fmgr->provided == NULL) {
            ESP_LOGE(TAG, "admit '%s': %s is not something that can be "
                          "watched", progname, res);
            return RV9_PROC_ERR_CONTRACT;
        }

        /*
         * Provided now, or provided by something that could be started.
         * The second is the check that matters: a supervisor started before
         * its control loop is ordinary, and one naming a control loop that
         * does not exist on this machine will wait forever.
         */
        if (dev->fmgr->provided(dev, rest)) continue;
        if (rv9_mod_any_declares(RV9_MTAG_PUBLISHES, res)) continue;

        ESP_LOGE(TAG, "admit '%s': it watches %s, and nothing on this "
                      "machine publishes it", progname, res);
        return RV9_PROC_ERR_NOPUB;
    }

    return claim_failsafes(pid, image, progname);
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

/* Records, or a count when the buffer is empty -- the same shape as
   procs and stacks, so a module can size its buffer before filling it. */
static int io_claims_op(void *buf, uint32_t len)
{
    if (buf == NULL || len == 0) return rv9_claim_list(NULL, 0);
    return rv9_claim_list((rv9_sys_claim_t *)buf,
                          (int)(len / sizeof(rv9_sys_claim_t)));
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
    .claims  = io_claims_op,
};

rv9_io_err_t rv9_io_init(void)
{
    if (s_lock != NULL) return RV9_IO_OK;
    if (rv9_lock_create(&s_lock) != RV9_OK) return RV9_IO_ERR_NOMEM;
    if (rv9_claim_init() != RV9_IO_OK) return RV9_IO_ERR_NOMEM;

    rv9_proc_set_hooks(io_on_fork, io_on_exit);
    rv9_proc_set_claim_hook(io_claim_for_fork);
    rv9_proc_set_ended_hook(io_on_ended);
    rv9_mod_set_io_ops(&s_mod_io_ops);
    ESP_LOGI(TAG, "I/O manager up");
    return RV9_IO_OK;
}
