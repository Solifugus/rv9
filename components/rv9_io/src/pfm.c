/*
 * PFM -- the publication file manager.
 *
 * A fourth discipline, after SCF's character streams, RBF's blocks and
 * PIO's addressable units. It exists because one process computes a value
 * and another has to see it, and RV-9 had no answer to that: paths and
 * signals, and neither carries an observation.
 *
 *     /pub0/MOTOR_CONTROL      one publication cell
 *
 * A cell is a named, fixed-size piece of memory holding the most recent
 * publication and nothing else. Writing to it publishes; reading it takes
 * a snapshot. There is no queue, and that is the point: a watcher wants
 * *the current value*, not every value that has ever been, and a queue
 * must either grow without bound or throw things away -- both wrong
 * answers to "what is the speed now".
 *
 * ---- why a device ----
 *
 * The obvious answer is shared memory, and it dies at the next phase: PMP
 * isolation exists precisely to stop one process handing another a
 * pointer. A path survives it, because a read and a write go through the
 * I/O manager, which sits above the seam. And a path arrives with naming,
 * ownership, `owns`, lifetime tied to the process, and an already
 * RT-resident transfer path -- none of which a new mechanism would have.
 *
 * ---- coherence ----
 *
 * Publication is one write of one struct, so a reader never sees half of
 * one value. Seeing half of a *set* -- the new speed with the old current
 * -- is the harder problem, and the one R9 §18 is actually about. It is
 * solved with a seqlock:
 *
 *   the writer bumps a counter to odd, copies, bumps it to even
 *   a reader takes the counter, copies, takes it again, and retries
 *   if the two differ or the first was odd
 *
 * The asymmetry falls the right way round. The writer never waits, never
 * allocates, and never takes a lock -- so a 1 kHz control loop publishing
 * into a cell pays two stores and a memcpy, with no bound it can miss. The
 * cost of contention lands entirely on the observer, which is the process
 * that can afford it. That is the whole reason for choosing this over a
 * mutex, and it is what makes the RT half of R9 §18's contract keepable.
 *
 * ---- what a cell is not ----
 *
 * It is not a lock, a queue, a mailbox or an RPC. R9 §19's inputs are this
 * same object with the ownership reversed -- the supervisor publishes,
 * the control loop observes -- and need nothing added here.
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "rv9-pfm";

/*
 * How many observers may be blocked in RV9_PUB_GS_WAIT at once, across the
 * whole device.
 *
 * Small and fixed, like everything else here. Waiting costs a semaphore,
 * and a semaphore taken from the heap while a control loop is running is
 * exactly the kind of thing this file manager exists to avoid -- so they
 * are all created at mount and handed out. Reading a cell needs no slot at
 * all; only blocking on one does.
 */
#define PFM_MAX_WAITERS 4

/*
 * How many times a reader retries before giving up.
 *
 * On one core a reader that is preempted mid-snapshot needs exactly one
 * retry: the writer runs to completion before the reader is scheduled
 * again. More than that means publications are arriving faster than a
 * snapshot can be taken, which is a real condition and is counted rather
 * than waited out -- an observer that spins forever inside the I/O manager
 * is worse than one told it cannot keep up.
 */
#define PFM_MAX_TRIES 8

/*
 * A cell, in the store the driver supplied.
 *
 * seq, len, stamp_us and the payload are the seqlock's territory and are
 * touched by the writer with no lock held. name, cap, writer and readers
 * are bookkeeping, written only at open and close under the device lock.
 * The two never overlap.
 */
typedef struct {
    char     name[RV9_PUB_MAX_NAME];   /* "" while the cell is unused */
    uint32_t cap;                      /* bytes of value it can hold */
    uint32_t held;                     /* somebody has it open for writing */
    uint32_t writer;                   /* their pid; 0 if the system's own */
    uint32_t readers;
    uint32_t torn;                     /* snapshots abandoned, since boot */

    uint32_t seq;                      /* odd while writing; 0 = never */
    uint32_t len;
    uint64_t stamp_us;
    /* cap bytes of value follow, and then padding to the stride */
} pub_cell_t;

typedef struct {
    rv9_sem_t   sem;
    pub_cell_t *cell;      /* the cell being waited on, NULL when free */
    uint32_t    armed;     /* 1 while a wake is wanted */
} pub_waiter_t;

typedef struct {
    uint8_t     *base;
    uint32_t     stride;   /* bytes from one cell to the next */
    uint32_t     cap;      /* bytes of value per cell */
    uint32_t     count;
    rv9_lock_t   lock;
    pub_waiter_t waiter[PFM_MAX_WAITERS];
} pub_dev_t;

typedef struct {
    pub_cell_t *cell;      /* NULL on a path to the device itself */
    bool        is_writer;
    int         slot;      /* waiter slot, -1 until it first blocks */
    uint32_t    dirpos;    /* how far a directory read has got */
} pub_path_t;

static pub_cell_t *cell_at(pub_dev_t *d, uint32_t i)
{
    return (pub_cell_t *)(d->base + (size_t)i * d->stride);
}

static uint8_t *cell_value(pub_cell_t *c)
{
    return (uint8_t *)c + sizeof(pub_cell_t);
}

/* ---- mount ---- */

static rv9_io_err_t pfm_mount(rv9_dev_t *dev)
{
    if (dev->drv->arena == NULL) {
        ESP_LOGE(TAG, "%s: driver '%s' has no store to keep cells in",
                 dev->name, dev->drv->name);
        return RV9_IO_ERR_UNSUPPORTED;
    }

    void *base = NULL;
    uint32_t size = 0;
    rv9_io_err_t err = dev->drv->arena(dev, &base, &size);
    if (err != RV9_IO_OK) return err;
    if (base == NULL || size == 0) return RV9_IO_ERR_IO;

    /* opt[0] is the driver's too. Reading it here rather than being told
       keeps one number in one place in the descriptor. */
    uint32_t cap = dev->opt[0] ? dev->opt[0] : 64;
    cap = (cap + 7u) & ~7u;

    uint32_t stride = (uint32_t)((sizeof(pub_cell_t) + cap + 7u) & ~7u);
    uint32_t count = size / stride;
    if (count == 0) {
        ESP_LOGE(TAG, "%s: %lu bytes will not hold one cell of %lu",
                 dev->name, (unsigned long)size, (unsigned long)cap);
        return RV9_IO_ERR_NOMEM;
    }

    pub_dev_t *d = rv9_calloc(1, sizeof(*d));
    if (d == NULL) return RV9_IO_ERR_NOMEM;

    d->base   = (uint8_t *)base;
    d->stride = stride;
    d->cap    = cap;
    d->count  = count;

    if (rv9_lock_create(&d->lock) != RV9_OK) {
        rv9_free(d);
        return RV9_IO_ERR_NOMEM;
    }

    /*
     * Every semaphore an observer could ever need, made now. A wait must
     * not allocate: the process doing it is the reactive supervisor, and
     * the moment it most wants to block is the moment memory is tightest.
     */
    for (int i = 0; i < PFM_MAX_WAITERS; i++) {
        if (rv9_sem_create(1, 0, &d->waiter[i].sem) != RV9_OK) {
            for (int j = 0; j < i; j++) rv9_sem_destroy(d->waiter[j].sem);
            rv9_lock_destroy(d->lock);
            rv9_free(d);
            return RV9_IO_ERR_NOMEM;
        }
    }

    memset(base, 0, size);
    for (uint32_t i = 0; i < count; i++) cell_at(d, i)->cap = cap;

    dev->fmgr_state = d;
    ESP_LOGI(TAG, "%s: %lu cells of %lu bytes", dev->name,
             (unsigned long)count, (unsigned long)cap);
    return RV9_IO_OK;
}

/* ---- finding and making cells ---- */

/* Called with the lock held. */
static pub_cell_t *find_cell(pub_dev_t *d, const char *name)
{
    for (uint32_t i = 0; i < d->count; i++) {
        pub_cell_t *c = cell_at(d, i);
        if (c->name[0] && strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

static pub_cell_t *make_cell(pub_dev_t *d, const char *name)
{
    for (uint32_t i = 0; i < d->count; i++) {
        pub_cell_t *c = cell_at(d, i);
        if (c->name[0]) continue;

        strncpy(c->name, name, sizeof(c->name) - 1);
        c->cap = d->cap;
        return c;
    }
    return NULL;
}

/*
 * A cell name is one path component and nothing clever.
 *
 * It becomes a name a watcher in another process has to type, so it is
 * held to what a program can be expected to write down: no slashes, no
 * spaces, and short enough to print in a table.
 */
static bool name_ok(const char *rest)
{
    if (rest == NULL || rest[0] == '\0') return false;

    size_t n = 0;
    for (; rest[n]; n++) {
        char ch = rest[n];
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' ||
                  ch == '-';
        if (!ok) return false;
    }
    return n < RV9_PUB_MAX_NAME;
}

static rv9_io_err_t pfm_open(rv9_path_t *path, const char *rest)
{
    rv9_dev_t *dev = path->dev;
    pub_dev_t *d = (pub_dev_t *)dev->fmgr_state;
    if (d == NULL) return RV9_IO_ERR_IO;

    pub_path_t *st = rv9_calloc(1, sizeof(*st));
    if (st == NULL) return RV9_IO_ERR_NOMEM;
    st->slot = -1;

    /* The device itself is its directory, the same as a volume. */
    if (rest == NULL || rest[0] == '\0') {
        if (path->mode & RV9_MODE_WRITE) {
            rv9_free(st);
            ESP_LOGW(TAG, "%s: the device is a directory; write to a cell",
                     dev->name);
            return RV9_IO_ERR_MODE;
        }
        path->fm_state = st;
        return RV9_IO_OK;
    }

    if (!name_ok(rest)) {
        rv9_free(st);
        ESP_LOGW(TAG, "%s: '%s' is not a publication name", dev->name, rest);
        return RV9_IO_ERR_INVAL;
    }

    rv9_lock_acquire(d->lock);

    pub_cell_t *c = find_cell(d, rest);

    /*
     * A publisher declares the cell; an observer does not.
     *
     * Opening a name nobody publishes is a mistake worth reporting rather
     * than a cell that will never change: a watcher naming a component
     * that is not running should be told so at open, not left reading
     * "never published" forever.
     */
    if (c == NULL) {
        if (!(path->mode & RV9_MODE_WRITE)) {
            rv9_lock_release(d->lock);
            rv9_free(st);
            return RV9_IO_ERR_NOTFOUND;
        }
        c = make_cell(d, rest);
        if (c == NULL) {
            rv9_lock_release(d->lock);
            rv9_free(st);
            ESP_LOGE(TAG, "%s: all %lu cells are taken; %s cannot be added",
                     dev->name, (unsigned long)d->count, rest);
            return RV9_IO_ERR_NOMEM;
        }
    }

    /*
     * One writer, always.
     *
     * Two processes publishing one value is not a race to be won -- it is
     * two answers to a question with one reader. The claim table cannot
     * express this: its exclusivity shuts out readers too, and one writer
     * with many readers is the entire shape of a publication. So the rule
     * lives here, where the discipline is.
     */
    if (path->mode & RV9_MODE_WRITE) {
        if (c->held) {
            rv9_lock_release(d->lock);
            rv9_free(st);
            ESP_LOGW(TAG, "%s/%s is already published by pid %lu; pid %u "
                          "may not publish it too (0 is the system)",
                     dev->name, rest, (unsigned long)c->writer,
                     (unsigned)rv9_proc_current_pid());
            return RV9_IO_ERR_BUSY;
        }
        c->held   = 1;
        c->writer = rv9_proc_current_pid();
        st->is_writer = true;
    }
    if (path->mode & RV9_MODE_READ) c->readers++;

    rv9_lock_release(d->lock);

    st->cell = c;
    strncpy(path->name, rest, sizeof(path->name) - 1);
    path->fm_state = st;
    return RV9_IO_OK;
}

static rv9_io_err_t pfm_close(rv9_path_t *path)
{
    pub_path_t *st = (pub_path_t *)path->fm_state;
    if (st == NULL) return RV9_IO_OK;

    pub_dev_t *d = (pub_dev_t *)path->dev->fmgr_state;

    if (d != NULL && st->cell != NULL) {
        rv9_lock_acquire(d->lock);

        if (st->is_writer) { st->cell->held = 0; st->cell->writer = 0; }
        if ((path->mode & RV9_MODE_READ) && st->cell->readers) {
            st->cell->readers--;
        }
        if (st->slot >= 0) {
            d->waiter[st->slot].armed = 0;
            d->waiter[st->slot].cell  = NULL;
        }

        rv9_lock_release(d->lock);
    }

    /*
     * The cell stays. A control loop that has stopped leaves behind the
     * last value it published and the moment it observed it, which is
     * precisely what an operator or a supervisor arriving afterwards needs
     * to read. Cells are a fixed resource claimed for the life of the
     * system, not for the life of a process.
     */
    rv9_free(st);
    path->fm_state = NULL;
    return RV9_IO_OK;
}

/* ---- publishing ---- */

/*
 * Wake anyone blocked on this cell.
 *
 * Deliberately after the publication is complete and outside any lock. The
 * `armed` flag means at most one wake per wait, so a loop publishing at
 * 1 kHz into a cell whose observer works at 50 Hz makes fifty calls a
 * second and reads a word the other nine hundred and fifty times.
 *
 * Not RV9_RT_CODE, and it must not be: giving a semaphore reaches into the
 * host kernel. The publication above it *is* resident, so a control loop
 * with the flash cache off still publishes -- it simply does not wake
 * anybody until the cache is back. That is the right way round.
 */
static void wake_watchers(pub_dev_t *d, pub_cell_t *c)
{
    for (int i = 0; i < PFM_MAX_WAITERS; i++) {
        pub_waiter_t *w = &d->waiter[i];
        if (__atomic_load_n(&w->cell, __ATOMIC_ACQUIRE) != c) continue;
        if (__atomic_exchange_n(&w->armed, 0, __ATOMIC_ACQ_REL) == 0) continue;

        /* The semaphore is created at mount and never destroyed, so this
           cannot be racing a teardown -- only a waiter that has just given
           up, which finds its slot re-armed and loops. */
        rv9_sem_give(w->sem);
    }
}

/*
 * Resident: a control loop publishes with a deadline pending, so this must
 * still be here when the flash cache is not. Everything it touches is the
 * store the driver allocated and this function's own code.
 */
static RV9_RT_CODE rv9_io_err_t pfm_write(rv9_path_t *path, const void *buf,
                                          size_t len, size_t *done)
{
    pub_path_t *st = (pub_path_t *)path->fm_state;
    if (st == NULL || st->cell == NULL) return RV9_IO_ERR_MODE;
    if (!st->is_writer) return RV9_IO_ERR_MODE;
    if (len < sizeof(rv9_pub_t)) return RV9_IO_ERR_INVAL;

    pub_cell_t *c = st->cell;

    rv9_pub_t head;
    memcpy(&head, buf, sizeof(head));

    /* len says how many bytes of value follow; zero means "all of them",
       so a publisher with a fixed struct need not repeat its size. */
    uint32_t avail = (uint32_t)(len - sizeof(rv9_pub_t));
    uint32_t vlen  = head.len ? head.len : avail;
    if (vlen > avail || vlen > c->cap) return RV9_IO_ERR_INVAL;

    uint64_t stamp = head.stamp_us ? head.stamp_us : rv9_time_us();

    /*
     * Odd, copy, even. The two increments are the publication: until the
     * second one lands, every observer keeps seeing the value before this.
     */
    uint32_t seq = c->seq;
    __atomic_store_n(&c->seq, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    memcpy(cell_value(c), (const uint8_t *)buf + sizeof(rv9_pub_t), vlen);
    c->len      = vlen;
    c->stamp_us = stamp;

    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&c->seq, seq + 2, __ATOMIC_RELAXED);

    if (done) *done = sizeof(rv9_pub_t) + vlen;

    pub_dev_t *d = (pub_dev_t *)path->dev->fmgr_state;
    if (d) wake_watchers(d, c);

    return RV9_IO_OK;
}

/* ---- observing ---- */

static rv9_io_err_t read_directory(pub_dev_t *d, pub_path_t *st, void *buf,
                                   size_t len, size_t *done)
{
    rv9_dirent_t *out = (rv9_dirent_t *)buf;
    size_t room = len / sizeof(rv9_dirent_t);
    size_t n = 0;

    rv9_lock_acquire(d->lock);

    while (n < room && st->dirpos < d->count) {
        pub_cell_t *c = cell_at(d, st->dirpos++);
        if (c->name[0] == '\0') continue;

        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].name, c->name, sizeof(out[n].name) - 1);
        out[n].size = c->len;
        n++;
    }

    rv9_lock_release(d->lock);

    if (done) *done = n * sizeof(rv9_dirent_t);
    return RV9_IO_OK;
}

/*
 * A snapshot, or nothing.
 *
 * Resident for the same reason the write is: a real-time process observing
 * another one's publication is an ordinary arrangement, and it observes
 * with a deadline pending.
 *
 * The value is truncated to the caller's buffer if it will not fit, and
 * the head always carries the true published length -- so a reader that
 * asked for too little can tell, and one that asked for enough never has
 * to. What is never truncated is coherence: the bytes handed back are all
 * from one publication.
 *
 * The directory branch is not resident and does not need to be -- nothing
 * reads a directory with a deadline pending, still less with the flash
 * cache off. The snapshot branch below it touches only the store and this
 * function.
 */
static RV9_RT_CODE rv9_io_err_t pfm_read(rv9_path_t *path, void *buf,
                                         size_t len, size_t *done)
{
    pub_path_t *st = (pub_path_t *)path->fm_state;
    if (st == NULL) return RV9_IO_ERR_IO;

    pub_dev_t *d = (pub_dev_t *)path->dev->fmgr_state;
    if (d == NULL) return RV9_IO_ERR_IO;

    if (st->cell == NULL) return read_directory(d, st, buf, len, done);
    if (len < sizeof(rv9_pub_t)) return RV9_IO_ERR_INVAL;

    pub_cell_t *c = st->cell;
    size_t room = len - sizeof(rv9_pub_t);

    for (int tries = 0; tries < PFM_MAX_TRIES; tries++) {
        uint32_t before = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
        if (before & 1u) continue;              /* mid-publication */

        rv9_pub_t head;
        head.seq      = before / 2;
        head.len      = c->len;
        head.stamp_us = c->stamp_us;

        size_t take = (head.len < room) ? head.len : room;
        memcpy((uint8_t *)buf + sizeof(head), cell_value(c), take);

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&c->seq, __ATOMIC_RELAXED) != before) continue;

        memcpy(buf, &head, sizeof(head));
        if (done) *done = sizeof(head) + take;
        return RV9_IO_OK;
    }

    /*
     * Counted rather than retried forever. An observer that cannot take a
     * snapshot between publications is being outrun, and that is a fact
     * about the system worth having in `pubs` -- not a reason to sit in
     * the I/O manager with a deadline running.
     */
    c->torn++;
    return RV9_IO_ERR_WOULDBLOCK;
}

/* ---- waiting ---- */

/* Called with the lock held. */
static int claim_slot(pub_dev_t *d, pub_cell_t *c)
{
    for (int i = 0; i < PFM_MAX_WAITERS; i++) {
        if (d->waiter[i].cell != NULL) continue;

        /* It may have been given while its last owner was giving up. */
        rv9_sem_take(d->waiter[i].sem, 0);
        d->waiter[i].armed = 0;
        __atomic_store_n(&d->waiter[i].cell, c, __ATOMIC_RELEASE);
        return i;
    }
    return -1;
}

static rv9_io_err_t pfm_wait(rv9_path_t *path, rv9_pub_wait_t *w)
{
    pub_path_t *st = (pub_path_t *)path->fm_state;
    if (st == NULL || st->cell == NULL) return RV9_IO_ERR_MODE;

    pub_dev_t *d = (pub_dev_t *)path->dev->fmgr_state;
    if (d == NULL) return RV9_IO_ERR_IO;

    pub_cell_t *c = st->cell;

    /* Already moved on, or the caller only wanted to look. */
    uint32_t now = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) / 2;
    if (now != w->seq) { w->seq = now; return RV9_IO_OK; }
    if (w->timeout_ms == 0) return RV9_IO_ERR_TIMEOUT;

    if (st->slot < 0) {
        rv9_lock_acquire(d->lock);
        st->slot = claim_slot(d, c);
        rv9_lock_release(d->lock);

        if (st->slot < 0) {
            ESP_LOGW(TAG, "%s/%s: all %d waiters are taken", path->dev->name,
                     c->name, PFM_MAX_WAITERS);
            return RV9_IO_ERR_NOPATHS;
        }
    }

    uint64_t deadline = (w->timeout_ms == RV9_WAIT_FOREVER)
                            ? 0 : rv9_time_ms() + w->timeout_ms;

    for (;;) {
        /*
         * Arm, then look again, then sleep. In that order: a publication
         * landing between the look and the sleep must find the flag set,
         * or the wakeup is lost and the observer sleeps through the change
         * it was waiting for.
         */
        __atomic_store_n(&d->waiter[st->slot].armed, 1, __ATOMIC_RELEASE);

        now = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) / 2;
        if (now != w->seq) {
            __atomic_store_n(&d->waiter[st->slot].armed, 0, __ATOMIC_RELEASE);
            w->seq = now;
            return RV9_IO_OK;
        }

        uint32_t slice = 0;
        if (w->timeout_ms == RV9_WAIT_FOREVER) {
            slice = RV9_WAIT_FOREVER;
        } else {
            uint64_t nowms = rv9_time_ms();
            if (nowms >= deadline) break;
            slice = (uint32_t)(deadline - nowms);
        }

        rv9_sem_take(d->waiter[st->slot].sem, slice);
        /* Whether that was the publication, a stale give or the timeout is
           settled by looking at the sequence, not by the return. */
    }

    __atomic_store_n(&d->waiter[st->slot].armed, 0, __ATOMIC_RELEASE);

    now = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) / 2;
    if (now != w->seq) { w->seq = now; return RV9_IO_OK; }
    return RV9_IO_ERR_TIMEOUT;
}

static rv9_io_err_t pfm_getstat(rv9_path_t *path, uint32_t code, void *arg)
{
    pub_path_t *st = (pub_path_t *)path->fm_state;
    if (st == NULL || arg == NULL) return RV9_IO_ERR_INVAL;

    pub_dev_t *d = (pub_dev_t *)path->dev->fmgr_state;
    if (d == NULL) return RV9_IO_ERR_IO;

    switch (code) {
    case RV9_PUB_GS_WAIT:
        return pfm_wait(path, (rv9_pub_wait_t *)arg);

    case RV9_PUB_GS_INFO: {
        if (st->cell == NULL) return RV9_IO_ERR_MODE;
        pub_cell_t *c = st->cell;
        rv9_pub_info_t *out = (rv9_pub_info_t *)arg;

        memset(out, 0, sizeof(*out));
        rv9_lock_acquire(d->lock);
        strncpy(out->name, c->name, sizeof(out->name) - 1);
        out->cap     = c->cap;
        out->held    = (uint8_t)(c->held ? 1 : 0);
        out->writer  = (uint16_t)c->writer;
        out->readers = (uint16_t)c->readers;
        out->torn    = c->torn;
        rv9_lock_release(d->lock);

        /* Outside the lock, and read the way an observer reads it: the
           publisher does not take the lock, so neither may this. */
        uint32_t seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
        out->seq      = seq / 2;
        out->len      = c->len;
        out->stamp_us = c->stamp_us;
        return RV9_IO_OK;
    }

    case RV9_GS_SIZE:
        *(uint32_t *)arg = st->cell ? st->cell->cap : d->count;
        return RV9_IO_OK;

    default:
        return RV9_IO_ERR_UNSUPPORTED;
    }
}

/*
 * Give a cell back.
 *
 * Cells are not freed when their publisher exits, deliberately -- the last
 * thing a stopped component said should outlive it. But "not automatically"
 * is not "never": a component that will not run again leaves a cell nobody
 * will ever publish into, and an operator should be able to clear it
 * rather than reboot. `del /pub0/NAME` does it.
 *
 * Only when nobody has it open. Removing a cell somebody is publishing
 * into would leave that path writing into a name that has been reused.
 */
static rv9_io_err_t pfm_remove(rv9_dev_t *dev, const char *name)
{
    pub_dev_t *d = (pub_dev_t *)dev->fmgr_state;
    if (d == NULL) return RV9_IO_ERR_IO;

    rv9_lock_acquire(d->lock);

    pub_cell_t *c = find_cell(d, name);
    rv9_io_err_t err = RV9_IO_OK;

    if (c == NULL) {
        err = RV9_IO_ERR_NOTFOUND;
    } else if (c->held || c->readers) {
        err = RV9_IO_ERR_BUSY;
    } else {
        memset(c, 0, d->stride);
        c->cap = d->cap;
    }

    rv9_lock_release(d->lock);
    return err;
}

/* A publication has no position: there is one value, and it is the current
   one. Rewinding a directory read is the one thing seeking could mean. */
static rv9_io_err_t pfm_seek(rv9_path_t *path, int64_t offset, int whence)
{
    pub_path_t *st = (pub_path_t *)path->fm_state;
    if (st == NULL || st->cell != NULL) return RV9_IO_ERR_UNSUPPORTED;
    if (whence != RV9_SEEK_SET || offset != 0) return RV9_IO_ERR_UNSUPPORTED;

    st->dirpos = 0;
    return RV9_IO_OK;
}

static const rv9_filemgr_t pfm = {
    .name    = "pfm",
    .mount   = pfm_mount,
    .open    = pfm_open,
    .close   = pfm_close,
    .read    = pfm_read,
    .write   = pfm_write,
    .seek    = pfm_seek,
    .getstat = pfm_getstat,
    .remove  = pfm_remove,
};

rv9_io_err_t rv9_pfm_register(void)
{
    return rv9_io_register_filemgr(&pfm);
}
