/*
 * MFM -- the module file manager: what this machine holds, as files.
 *
 *     /m0            the directory: one rv9_dirent_t per module
 *     /m0/pubs       pubs, byte for byte as it is in the store
 *
 * Read-only. The modules are the store's, or ones `load` brought in, and
 * changing one is flashing the store or loading a newer revision -- not a
 * write through a path. A module found on the module path is not listed:
 * it is a file somewhere already, and /m0 is what this machine has.
 *
 * WHAT IT IS FOR
 *
 * A module as a file can be copied (`copy /m0/pubs /sd0/cmd/pubs.mod`),
 * compared and served, and serving is the point: `httpd` hands /m0 to a
 * peer, which is step 3 of a module path across a federation (design §74).
 * The bytes are exactly the store's, so a module's signature travels with
 * it and the peer checks the same block this machine was flashed with.
 *
 * The driver beneath it, moddir, has nothing to drive: the module manager
 * is the source, and this asks it directly, as PFM asks its arena.
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"

#include <string.h>

typedef struct {
    bool                   is_dir;
    const rv9_mod_entry_t *e;          /* the module; NULL for the directory */
} mfm_path_t;

static const rv9_mod_entry_t *find_held(const char *name)
{
    for (const rv9_mod_entry_t *e = rv9_mod_dir_next(NULL); e; e = rv9_mod_dir_next(e)) {
        if (rv9_mod_held(e) && strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

static rv9_io_err_t mfm_open(rv9_path_t *path, const char *rest)
{
    if (path->mode & (RV9_MODE_WRITE | RV9_MODE_CREATE)) return RV9_IO_ERR_MODE;

    mfm_path_t *st = rv9_calloc(1, sizeof(*st));
    if (st == NULL) return RV9_IO_ERR_NOMEM;

    if (rest == NULL || rest[0] == '\0') {
        st->is_dir = true;
    } else {
        /* Highest revision of a name, as running it would choose. */
        const rv9_mod_entry_t *best = NULL;
        for (const rv9_mod_entry_t *e = rv9_mod_dir_next(NULL); e; e = rv9_mod_dir_next(e)) {
            if (rv9_mod_held(e) && strcmp(e->name, rest) == 0 &&
                (best == NULL || e->revision > best->revision)) best = e;
        }
        if (best == NULL) {
            rv9_free(st);
            return RV9_IO_ERR_NOTFOUND;
        }
        st->e = best;
    }
    path->fm_state = st;
    return RV9_IO_OK;
}

static rv9_io_err_t mfm_close(rv9_path_t *path)
{
    rv9_free(path->fm_state);
    path->fm_state = NULL;
    return RV9_IO_OK;
}

/* The directory: whole entries only, `pos` counting them. */
static rv9_io_err_t dir_read(rv9_path_t *path, void *buf, size_t len, size_t *done)
{
    size_t room = len / sizeof(rv9_dirent_t), n = 0;
    int64_t index = 0;
    for (const rv9_mod_entry_t *e = rv9_mod_dir_next(NULL); e && n < room;
         e = rv9_mod_dir_next(e)) {
        if (!rv9_mod_held(e)) continue;
        if (index++ < path->pos) continue;
        rv9_dirent_t *d = (rv9_dirent_t *)buf + n++;
        memset(d, 0, sizeof(*d));
        strncpy(d->name, e->name, sizeof(d->name) - 1);
        d->size = e->size;
    }
    path->pos += (int64_t)n;
    *done = n * sizeof(rv9_dirent_t);
    return RV9_IO_OK;
}

static rv9_io_err_t mfm_read(rv9_path_t *path, void *buf, size_t len, size_t *done)
{
    mfm_path_t *st = (mfm_path_t *)path->fm_state;
    if (st == NULL || done == NULL) return RV9_IO_ERR_INVAL;
    if (st->is_dir) return dir_read(path, buf, len, done);

    uint32_t got = 0;
    if (rv9_mod_read_at(st->e, (uint32_t)path->pos, buf, (uint32_t)len, &got) != RV9_MOD_OK) {
        return RV9_IO_ERR_IO;
    }
    path->pos += got;
    *done = got;
    return RV9_IO_OK;
}

static rv9_io_err_t mfm_write(rv9_path_t *path, const void *buf, size_t len, size_t *done)
{
    (void)path; (void)buf; (void)len; (void)done;
    return RV9_IO_ERR_MODE;
}

static rv9_io_err_t mfm_seek(rv9_path_t *path, int64_t offset, int whence)
{
    mfm_path_t *st = (mfm_path_t *)path->fm_state;
    if (st == NULL || st->is_dir) return RV9_IO_ERR_UNSUPPORTED;
    int64_t base = whence == RV9_SEEK_SET ? 0
                 : whence == RV9_SEEK_CUR ? path->pos : (int64_t)st->e->size;
    int64_t to = base + offset;
    if (to < 0 || to > (int64_t)st->e->size) return RV9_IO_ERR_INVAL;
    path->pos = to;
    return RV9_IO_OK;
}

static rv9_io_err_t mfm_getstat(rv9_path_t *path, uint32_t code, void *arg)
{
    mfm_path_t *st = (mfm_path_t *)path->fm_state;
    if (code == RV9_GS_SIZE && arg && st && !st->is_dir) {
        *(uint64_t *)arg = st->e->size;
        return RV9_IO_OK;
    }
    return RV9_IO_ERR_UNSUPPORTED;
}

static rv9_io_err_t mfm_setstat(rv9_path_t *path, uint32_t code, void *arg)
{
    (void)path; (void)code; (void)arg;
    return RV9_IO_ERR_UNSUPPORTED;
}

/* Whether a name is here, for a manifest that declares /m0/<name>. */
static bool mfm_provided(rv9_dev_t *dev, const char *rest)
{
    (void)dev;
    return rest != NULL && find_held(rest) != NULL;
}

static const rv9_filemgr_t mfm = {
    .name     = "mfm",
    .open     = mfm_open,
    .close    = mfm_close,
    .read     = mfm_read,
    .write    = mfm_write,
    .seek     = mfm_seek,
    .getstat  = mfm_getstat,
    .setstat  = mfm_setstat,
    .provided = mfm_provided,
};

rv9_io_err_t rv9_mfm_register(void)
{
    return rv9_io_register_filemgr(&mfm);
}

/* moddir: the driver under /m0, with nothing of its own to do. */
static rv9_io_err_t moddir_init(rv9_dev_t *dev)
{
    (void)dev;
    return RV9_IO_OK;
}

static const rv9_driver_t moddir_drv = {
    .name = "moddir",
    .init = moddir_init,
};

rv9_io_err_t rv9_drv_moddir_register(void)
{
    return rv9_io_register_driver(&moddir_drv);
}
