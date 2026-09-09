/*
 * SCF -- sequential character file manager.
 *
 * Line discipline for character streams, and nothing else. It knows about
 * newlines and echo; it does not know whether the bytes are going to a UART,
 * a screen, or a socket. That is the driver's business.
 *
 * Everything here is shared by every character device in the system, which
 * is the payoff for the layering: adding a character driver gets all of this
 * for free.
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include <string.h>

/* Descriptor options, by index into rv9_devdesc_t.opt */
#define OPT_ECHO   0
#define OPT_AUTOLF 1

#define XLAT_CHUNK 64

static rv9_io_err_t scf_open(rv9_path_t *path)
{
    (void)path;
    return RV9_IO_OK;   /* SCF keeps no per-path state yet */
}

static rv9_io_err_t scf_close(rv9_path_t *path)
{
    (void)path;
    return RV9_IO_OK;
}

static rv9_io_err_t scf_write(rv9_path_t *path, const void *buf, size_t len,
                              size_t *done)
{
    const rv9_dev_t *dev = path->dev;
    const char *src = (const char *)buf;

    if (dev->drv->write == NULL) return RV9_IO_ERR_UNSUPPORTED;

    /* Without newline translation there is nothing to do but pass it on. */
    if (!dev->opt[OPT_AUTOLF]) {
        return dev->drv->write((rv9_dev_t *)dev, buf, len, done);
    }

    /* Translate \n to \r\n in chunks, so a long write does not need a
       buffer proportional to its length. */
    char out[XLAT_CHUNK];
    size_t o = 0, written = 0;

    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\n') {
            if (o > XLAT_CHUNK - 2) {
                size_t moved = 0;
                rv9_io_err_t err = dev->drv->write((rv9_dev_t *)dev, out, o, &moved);
                if (err != RV9_IO_OK) { if (done) *done = written; return err; }
                o = 0;
            }
            out[o++] = '\r';
        } else if (o == XLAT_CHUNK) {
            size_t moved = 0;
            rv9_io_err_t err = dev->drv->write((rv9_dev_t *)dev, out, o, &moved);
            if (err != RV9_IO_OK) { if (done) *done = written; return err; }
            o = 0;
        }

        out[o++] = src[i];
        written++;
    }

    if (o > 0) {
        size_t moved = 0;
        rv9_io_err_t err = dev->drv->write((rv9_dev_t *)dev, out, o, &moved);
        if (err != RV9_IO_OK) { if (done) *done = written; return err; }
    }

    /* Report what the caller gave us, not what went on the wire -- the
       translation is ours and the caller should not see it. */
    if (done) *done = written;
    return RV9_IO_OK;
}

static rv9_io_err_t scf_read(rv9_path_t *path, void *buf, size_t len,
                             size_t *done)
{
    const rv9_dev_t *dev = path->dev;
    if (dev->drv->read == NULL) return RV9_IO_ERR_UNSUPPORTED;

    rv9_io_err_t err = dev->drv->read((rv9_dev_t *)dev, buf, len, done);

    /* Echo is SCF's job, not the driver's -- the driver may not even be able
       to write back to where the input came from. */
    if (err == RV9_IO_OK && dev->opt[OPT_ECHO] && done && *done > 0 &&
        dev->drv->write != NULL) {
        size_t moved = 0;
        dev->drv->write((rv9_dev_t *)dev, buf, *done, &moved);
    }
    return err;
}

/* A character stream has no position. Saying so is more useful than
   pretending the call succeeded. */
static rv9_io_err_t scf_seek(rv9_path_t *path, int64_t offset, int whence)
{
    (void)path; (void)offset; (void)whence;
    return RV9_IO_ERR_UNSUPPORTED;
}

static rv9_io_err_t scf_getstat(rv9_path_t *path, uint32_t code, void *arg)
{
    rv9_dev_t *dev = path->dev;

    switch (code) {
    case RV9_SS_ECHO:
        if (arg == NULL) return RV9_IO_ERR_INVAL;
        *(uint32_t *)arg = dev->opt[OPT_ECHO];
        return RV9_IO_OK;
    case RV9_SS_AUTOLF:
        if (arg == NULL) return RV9_IO_ERR_INVAL;
        *(uint32_t *)arg = dev->opt[OPT_AUTOLF];
        return RV9_IO_OK;
    default:
        /* Anything SCF does not understand belongs to the driver. */
        if (dev->drv->getstat) return dev->drv->getstat(dev, code, arg);
        return RV9_IO_ERR_UNSUPPORTED;
    }
}

static rv9_io_err_t scf_setstat(rv9_path_t *path, uint32_t code, void *arg)
{
    rv9_dev_t *dev = path->dev;

    switch (code) {
    case RV9_SS_ECHO:
        if (arg == NULL) return RV9_IO_ERR_INVAL;
        dev->opt[OPT_ECHO] = *(uint32_t *)arg;
        return RV9_IO_OK;
    case RV9_SS_AUTOLF:
        if (arg == NULL) return RV9_IO_ERR_INVAL;
        dev->opt[OPT_AUTOLF] = *(uint32_t *)arg;
        return RV9_IO_OK;
    default:
        if (dev->drv->setstat) return dev->drv->setstat(dev, code, arg);
        return RV9_IO_ERR_UNSUPPORTED;
    }
}

static const rv9_filemgr_t scf = {
    .name    = "scf",
    .open    = scf_open,
    .close   = scf_close,
    .read    = scf_read,
    .write   = scf_write,
    .seek    = scf_seek,
    .getstat = scf_getstat,
    .setstat = scf_setstat,
};

rv9_io_err_t rv9_scf_register(void)
{
    return rv9_io_register_filemgr(&scf);
}
