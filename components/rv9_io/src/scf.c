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

/* Line discipline */
#define LINE_MAX      128
#define POLL_MS       10
#define KEY_BACKSPACE 8
#define KEY_DELETE    127
#define KEY_CTRL_C    3

typedef struct {
    char   line[LINE_MAX];
    size_t len;
} scf_path_state_t;

static rv9_io_err_t scf_open(rv9_path_t *path)
{
    /* Only readers need a line buffer; a write-only path stays free of it. */
    if (!(path->mode & RV9_MODE_READ)) return RV9_IO_OK;

    path->fm_state = rv9_calloc(1, sizeof(scf_path_state_t));
    return path->fm_state ? RV9_IO_OK : RV9_IO_ERR_NOMEM;
}

static rv9_io_err_t scf_close(rv9_path_t *path)
{
    rv9_free(path->fm_state);
    path->fm_state = NULL;
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

/* Echo is SCF's job, not the driver's. The driver moves bytes; it has no
   idea that what it just read should be reflected back. */
static void echo(const rv9_dev_t *dev, const char *s, size_t n)
{
    if (!dev->opt[OPT_ECHO] || dev->drv->write == NULL) return;
    size_t moved = 0;
    dev->drv->write((rv9_dev_t *)dev, s, n, &moved);
}

/*
 * Read one line, with editing.
 *
 * Blocks until the user presses return. That is the discipline a character
 * device gets for free by sitting under SCF -- the driver is non-blocking
 * and knows nothing about lines, backspace, or when a read is finished.
 */
static rv9_io_err_t scf_read(rv9_path_t *path, void *buf, size_t len,
                             size_t *done)
{
    const rv9_dev_t *dev = path->dev;
    scf_path_state_t *st = (scf_path_state_t *)path->fm_state;

    if (dev->drv->read == NULL) return RV9_IO_ERR_UNSUPPORTED;
    if (st == NULL || len == 0) return RV9_IO_ERR_INVAL;

    for (;;) {
        char ch;
        size_t got = 0;
        rv9_io_err_t err = dev->drv->read((rv9_dev_t *)dev, &ch, 1, &got);

        if (err == RV9_IO_ERR_WOULDBLOCK || got == 0) {
            /* Nothing typed yet. Sleeping rather than spinning is what lets
               other processes run while a shell waits at its prompt. */
            rv9_task_delay_ms(POLL_MS);
            continue;
        }
        if (err != RV9_IO_OK) return err;

        if (ch == '\r' || ch == '\n') {
            echo(dev, "\r\n", 2);

            size_t n = st->len < len ? st->len : len;
            memcpy(buf, st->line, n);
            st->len = 0;
            if (done) *done = n;
            return RV9_IO_OK;
        }

        if (ch == KEY_BACKSPACE || ch == KEY_DELETE) {
            if (st->len > 0) {
                st->len--;
                echo(dev, "\b \b", 3);   /* rub out the character */
            }
            continue;
        }

        if (ch == KEY_CTRL_C) {
            st->len = 0;
            echo(dev, "^C\r\n", 4);
            if (done) *done = 0;
            return RV9_IO_OK;
        }

        if (ch < 32 || ch > 126) continue;    /* ignore what we cannot show */

        if (st->len < LINE_MAX - 1) {
            st->line[st->len++] = ch;
            echo(dev, &ch, 1);
        }
    }
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
