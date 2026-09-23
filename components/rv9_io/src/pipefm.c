/*
 * PIPEFM -- a pipe is a path.
 *
 * WHY THIS IS A FILE MANAGER AND NOT A SHELL FEATURE
 *
 * RV-9 had redirection from phase 4 and no pipes, which meant tools could
 * send their output somewhere but never to each other. "Small tools" with
 * nothing to compose them is just a lot of small programs.
 *
 * The temptation is to build pipes into the shell, where the `|` is. That
 * would work and would be worth nothing anywhere else. A pipe is a thing
 * you write into at one end and read out of at the other, which is the
 * definition of a device, and RV-9 already knows what to do with those.
 * So it is a file manager beside RBF, NFM, PFM and PIO, and the whole of
 * the shell's job becomes two opens and a fork.
 *
 * Everything that already reads standard input and writes standard output
 * composes from the moment this exists, without one line changing in any
 * of them. That is the entire argument for having built the I/O model this
 * way, finally being cashed in.
 *
 * NAMED, AND THEREFORE USEFUL WITHOUT A SHELL
 *
 * `/pipe/x` names a pipe; opening it makes it if it is absent. Two opens
 * find the same pipe, which is what makes an end-of-file possible: a
 * reader can be told that the last writer has gone, which a single shared
 * path could never express.
 *
 * The shell will use made-up names for `|`. But a name is not a cost paid
 * for the shell's benefit -- on a robot, one process publishing into
 * /pipe/telemetry and another consuming it is the same mechanism with
 * nobody typing anything.
 *
 * WAITING
 *
 * A read on an empty pipe sleeps until there is something or until the
 * writers have gone; a write to a full one sleeps until there is room.
 * Sleeping is done through the scheduler in small slices rather than on a
 * semaphore, which costs a millisecond of latency at the margin and is
 * correct under both kernels -- the lesson NFM learned in §9e, that a
 * driver has no business knowing how many host tasks its callers share.
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "rv9-pipe";

#define PIPE_NAME_MAX 16

/* How long a blocked reader or writer sleeps before looking again. Short
   enough that a pipeline does not feel it, long enough that a blocked
   thread is not a busy one. */
#define PIPE_SLICE_MS 2

/*
 * One pipe, in the driver's arena.
 *
 * Kept as a plain ring with counts rather than head/tail comparisons,
 * because "is it empty" and "is it full" are then the same question asked
 * of one number, and the wraparound has one place to be wrong.
 */
typedef struct {
    char     name[PIPE_NAME_MAX];
    bool     in_use;

    uint32_t head;          /* next byte to read */
    uint32_t tail;          /* next byte to write */
    uint32_t count;         /* bytes present */
    uint32_t capacity;

    uint32_t readers;
    uint32_t writers;

    /* Whether a writer has ever attached to this pipe.
       Without it a reader that opens first is told end-of-file
       immediately -- there are no writers, after all -- and a pipeline
       whose reader wins the race reads nothing at all. A pipe with no
       writer *yet* is not a pipe that has finished. */
    bool     seen_writer;

    /*
     * Whether the "nobody is reading" warning has already been given for
     * this pipe.
     *
     * The write returns an error every time, correctly. What it must not do
     * is *say so* every time: a writer that ignores the error -- which is
     * every tool here, since m_say discarded the return -- calls write once
     * per line of its output, and one `mdir` into an abandoned pipe was a
     * hundred identical warnings. Soak run14's serial log came to 5.8 MB
     * against the previous run's 400 KB, almost all of it this.
     *
     * Once per pipe is information. Once per write is noise that hides it.
     */
    bool     warned_no_reader;

    uint8_t *data;
} pipe_t;

typedef struct {
    pipe_t   *pipes;
    uint32_t  count;
    uint32_t  capacity;     /* bytes per pipe */
    rv9_lock_t lock;
} pipe_dev_t;

/* What a path knows: which pipe, and which end it is. */
typedef struct {
    pipe_t *pipe;
    bool    reading;
    bool    writing;
} pipe_path_t;

/* ------------------------------------------------------------------ */

static rv9_io_err_t pipe_mount(rv9_dev_t *dev)
{
    void    *base = NULL;
    uint32_t size = 0;

    if (dev->drv == NULL || dev->drv->arena == NULL) {
        ESP_LOGE(TAG, "%s: its driver has no store to lend", dev->name);
        return RV9_IO_ERR_UNSUPPORTED;
    }
    rv9_io_err_t e = dev->drv->arena(dev, &base, &size);
    if (e != RV9_IO_OK || base == NULL) return RV9_IO_ERR_IO;

    uint32_t bytes = dev->opt[0] ? dev->opt[0] : 256;
    uint32_t count = dev->opt[1] ? dev->opt[1] : 4;

    /* Lay the bookkeeping out first and the buffers after it, so a pipe's
       bytes are contiguous and a wrap is one memcpy plus one more. */
    uint32_t need = count * (uint32_t)sizeof(pipe_t) + count * bytes;
    if (need > size) {
        ESP_LOGE(TAG, "%s: %lu pipes of %lu needs %lu, store is %lu",
                 dev->name, (unsigned long)count, (unsigned long)bytes,
                 (unsigned long)need, (unsigned long)size);
        return RV9_IO_ERR_NOMEM;
    }

    pipe_dev_t *d = rv9_calloc(1, sizeof(*d));
    if (d == NULL) return RV9_IO_ERR_NOMEM;

    if (rv9_lock_create(&d->lock) != RV9_OK) {
        rv9_free(d);
        return RV9_IO_ERR_NOMEM;
    }

    d->pipes    = (pipe_t *)base;
    d->count    = count;
    d->capacity = bytes;

    uint8_t *buffers = (uint8_t *)base + count * sizeof(pipe_t);
    for (uint32_t i = 0; i < count; i++) {
        memset(&d->pipes[i], 0, sizeof(pipe_t));
        d->pipes[i].data     = buffers + i * bytes;
        d->pipes[i].capacity = bytes;
    }

    dev->fmgr_state = d;
    ESP_LOGI(TAG, "%s: %lu pipes of %lu bytes", dev->name,
             (unsigned long)count, (unsigned long)bytes);
    return RV9_IO_OK;
}

/* Caller holds the lock. */
static pipe_t *find(pipe_dev_t *d, const char *name)
{
    for (uint32_t i = 0; i < d->count; i++) {
        if (d->pipes[i].in_use && strcmp(d->pipes[i].name, name) == 0) {
            return &d->pipes[i];
        }
    }
    return NULL;
}

/* Caller holds the lock. */
static pipe_t *make(pipe_dev_t *d, const char *name)
{
    for (uint32_t i = 0; i < d->count; i++) {
        pipe_t *p = &d->pipes[i];
        if (p->in_use) continue;

        memset(p->name, 0, sizeof(p->name));
        strncpy(p->name, name, sizeof(p->name) - 1);
        p->in_use      = true;
        p->head        = p->tail = p->count = 0;
        p->readers     = p->writers = 0;
        p->seen_writer = false;
        return p;
    }
    return NULL;
}

static rv9_io_err_t pipe_open(rv9_path_t *path, const char *rest)
{
    pipe_dev_t *d = (pipe_dev_t *)path->dev->fmgr_state;
    if (d == NULL) return RV9_IO_ERR_IO;

    if (rest == NULL || rest[0] == '\0') {
        /* The device itself is not a pipe. Naming one is what lets two
           opens find each other, which is what makes an end-of-file
           possible at all. */
        ESP_LOGW(TAG, "%s: a pipe needs a name, as in %s/x",
                 path->dev->name, path->dev->name);
        return RV9_IO_ERR_INVAL;
    }
    if (strlen(rest) >= PIPE_NAME_MAX) return RV9_IO_ERR_INVAL;

    pipe_path_t *st = rv9_calloc(1, sizeof(*st));
    if (st == NULL) return RV9_IO_ERR_NOMEM;

    rv9_lock_acquire(d->lock);

    pipe_t *p = find(d, rest);
    if (p == NULL) {
        p = make(d, rest);
        if (p == NULL) {
            rv9_lock_release(d->lock);
            rv9_free(st);
            ESP_LOGW(TAG, "%s: all %lu pipes are in use", path->dev->name,
                     (unsigned long)d->count);
            return RV9_IO_ERR_NOPATHS;
        }
    }

    st->pipe    = p;
    st->reading = (path->mode & RV9_MODE_READ)  != 0;
    st->writing = (path->mode & RV9_MODE_WRITE) != 0;

    /* A path opened for neither is a path that would hold the pipe open
       and never use it, and on close would decrement nothing. */
    if (!st->reading && !st->writing) {
        rv9_lock_release(d->lock);
        rv9_free(st);
        return RV9_IO_ERR_MODE;
    }

    if (st->reading) p->readers++;
    if (st->writing) { p->writers++; p->seen_writer = true; }

    rv9_lock_release(d->lock);

    path->fm_state = st;
    return RV9_IO_OK;
}

static rv9_io_err_t pipe_close(rv9_path_t *path)
{
    pipe_path_t *st = (pipe_path_t *)path->fm_state;
    if (st == NULL) return RV9_IO_OK;

    pipe_dev_t *d = (pipe_dev_t *)path->dev->fmgr_state;
    if (d != NULL) {
        rv9_lock_acquire(d->lock);

        pipe_t *p = st->pipe;
        if (st->reading && p->readers > 0) p->readers--;
        if (st->writing && p->writers > 0) p->writers--;

        /*
         * Nobody left at either end: the name goes, and so does whatever
         * was still in it. See pipemem's `retains`: half a sentence left
         * in a pipe is not a record, and the next command to use the name
         * would read it as the start of its own.
         */
        if (p->readers == 0 && p->writers == 0) {
            p->in_use           = false;
            p->seen_writer      = false;
            p->warned_no_reader = false;
            p->head        = p->tail = p->count = 0;
        }

        rv9_lock_release(d->lock);
    }

    path->fm_state = NULL;
    rv9_free(st);
    return RV9_IO_OK;
}

static rv9_io_err_t pipe_read(rv9_path_t *path, void *buf, size_t len,
                              size_t *done)
{
    pipe_path_t *st = (pipe_path_t *)path->fm_state;
    pipe_dev_t  *d  = (pipe_dev_t *)path->dev->fmgr_state;
    if (st == NULL || d == NULL || buf == NULL) return RV9_IO_ERR_INVAL;
    if (!st->reading) return RV9_IO_ERR_MODE;

    if (done) *done = 0;
    if (len == 0) return RV9_IO_OK;

    pipe_t *p = st->pipe;

    for (;;) {
        rv9_lock_acquire(d->lock);

        if (p->count > 0) {
            size_t n = (p->count < len) ? p->count : len;

            /* Up to the end of the buffer, then from the start again. */
            size_t first = p->capacity - p->head;
            if (first > n) first = n;
            memcpy(buf, p->data + p->head, first);
            if (n > first) memcpy((uint8_t *)buf + first, p->data, n - first);

            p->head   = (uint32_t)((p->head + n) % p->capacity);
            p->count -= (uint32_t)n;

            rv9_lock_release(d->lock);
            if (done) *done = n;
            return RV9_IO_OK;
        }

        /*
         * Empty. The end of the file is when the last writer has *gone*,
         * which is not the same as there being none yet -- a reader that
         * opened first would otherwise be told the pipe had finished
         * before anything had begun.
         */
        bool finished = (p->writers == 0 && p->seen_writer);
        rv9_lock_release(d->lock);

        if (finished) return RV9_IO_OK;          /* done stays 0: EOF */

        /* Somebody may yet write. Give up the CPU rather than spin, and
           notice if this process is being stopped while we wait. */
        if (rv9_task_cancelled()) return RV9_IO_ERR_IO;
        rv9_task_delay_ms(PIPE_SLICE_MS);
    }
}

static rv9_io_err_t pipe_write(rv9_path_t *path, const void *buf, size_t len,
                               size_t *done)
{
    pipe_path_t *st = (pipe_path_t *)path->fm_state;
    pipe_dev_t  *d  = (pipe_dev_t *)path->dev->fmgr_state;
    if (st == NULL || d == NULL || buf == NULL) return RV9_IO_ERR_INVAL;
    if (!st->writing) return RV9_IO_ERR_MODE;

    if (done) *done = 0;
    if (len == 0) return RV9_IO_OK;

    pipe_t *p = st->pipe;
    size_t  written = 0;

    while (written < len) {
        rv9_lock_acquire(d->lock);

        uint32_t room = p->capacity - p->count;
        if (room > 0) {
            size_t n = len - written;
            if (n > room) n = room;

            size_t first = p->capacity - p->tail;
            if (first > n) first = n;
            memcpy(p->data + p->tail, (const uint8_t *)buf + written, first);
            if (n > first) {
                memcpy(p->data, (const uint8_t *)buf + written + first,
                       n - first);
            }

            p->tail   = (uint32_t)((p->tail + n) % p->capacity);
            p->count += (uint32_t)n;
            written  += n;

            rv9_lock_release(d->lock);
            continue;
        }

        /*
         * Full. Somebody has to be reading, or this write can never
         * complete and the process would wait for a reader that is not
         * coming -- which is a hang, and a hang is worse than an error.
         */
        bool readers = (p->readers > 0);
        rv9_lock_release(d->lock);

        if (!readers) {
            if (done) *done = written;
            if (!p->warned_no_reader) {
                p->warned_no_reader = true;
                ESP_LOGW(TAG, "%s/%s: full and nobody is reading; "
                              "the writer should stop",
                         path->dev->name, p->name);
            }
            return RV9_IO_ERR_IO;
        }

        if (rv9_task_cancelled()) {
            if (done) *done = written;
            return RV9_IO_ERR_IO;
        }
        rv9_task_delay_ms(PIPE_SLICE_MS);
    }

    if (done) *done = written;
    return RV9_IO_OK;
}

/* A pipe has no position. Seeking one is a question with no answer, and
   saying so is better than quietly pretending to have moved. */
static rv9_io_err_t pipe_seek(rv9_path_t *path, int64_t offset, int whence)
{
    (void)path; (void)offset; (void)whence;
    return RV9_IO_ERR_UNSUPPORTED;
}

static rv9_io_err_t pipe_getstat(rv9_path_t *path, uint32_t code, void *arg)
{
    pipe_path_t *st = (pipe_path_t *)path->fm_state;
    if (st == NULL || arg == NULL) return RV9_IO_ERR_INVAL;

    switch (code) {
    case RV9_GS_READY: {
        /* Ready when there is something to read, or when there never will
           be again -- a reader wants to be woken for the end as well. */
        pipe_t *p = st->pipe;
        bool finished = (p->writers == 0 && p->seen_writer);
        *(uint32_t *)arg = (p->count > 0 || finished) ? 1u : 0u;
        return RV9_IO_OK;
    }
    default:
        return RV9_IO_ERR_UNSUPPORTED;
    }
}

static const rv9_filemgr_t pipefm = {
    .name    = "pipe",
    .open    = pipe_open,
    .close   = pipe_close,
    .read    = pipe_read,
    .write   = pipe_write,
    .seek    = pipe_seek,
    .getstat = pipe_getstat,
    .mount   = pipe_mount,
};

rv9_io_err_t rv9_pipefm_register(void)
{
    return rv9_io_register_filemgr(&pipefm);
}
