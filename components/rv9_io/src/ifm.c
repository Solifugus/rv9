/*
 * IFM -- the interface file manager.
 *
 * A fourth discipline, after SCF's character streams, RBF's blocks and
 * PIO's values. It serves devices on a shared bus, where the path names
 * which device and what moves is a string of bytes:
 *
 *     /i2c0/0x68     the chip answering at address 0x68
 *     /i2c0/104      the same chip, said in decimal
 *
 * WHY PIO COULD NOT DO THIS
 *
 * PIO carries one 32-bit value per read or write, which is exactly right
 * for a pin or a duty cycle and wrong for a sensor. Reading six bytes of
 * acceleration has to be *one* transaction: three axes sampled at one
 * instant, fetched without letting go of the bus, or the numbers are from
 * three different moments and the vector they form never existed.
 *
 * REGISTERS, WHICH ARE MOST OF WHAT THIS IS FOR
 *
 * Almost every sensor is a set of numbered registers, and almost every
 * datasheet describes reading one as: write the register number, then
 * *without releasing the bus*, read. That repeated start is not a detail
 * -- a device that is addressed and then dropped will often serve the
 * wrong register to whoever asks next.
 *
 * So the register lives on the path:
 *
 *     setstat(p, RV9_IFM_SS_REG, &reg);   read(p, buf, 6);
 *
 * and the read becomes the combined transaction the datasheet asked for.
 * On the *path* rather than the device, so two programs reading different
 * registers of the same chip do not quietly corrupt each other.
 *
 * Set it to RV9_IFM_REG_NONE and a read is a plain read, for the devices
 * that just stream.
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "rv9-ifm";

/* Enough for a burst read from an inertial unit, which is the demanding
   case: fourteen bytes is accelerometer, temperature and gyroscope in one
   go. Sixty-four leaves room without making a path expensive. */
#define IFM_MAX_XFER 64

typedef struct {
    uint32_t unit;          /* the address, or the chip select */
    void    *unit_state;    /* the driver's own */
    uint32_t reg;           /* RV9_IFM_REG_NONE, or the register to precede a read */
} ifm_path_t;

/*
 * "0x68" or "104" -- both, because a datasheet always says the first and a
 * shell script usually says the second. Refusing one of them would be a
 * small cruelty repeated every time somebody types an address.
 */
static bool parse_unit(const char *rest, uint32_t *out)
{
    if (rest == NULL || rest[0] == '\0') return false;

    uint32_t v = 0;
    bool     hex = (rest[0] == '0' && (rest[1] == 'x' || rest[1] == 'X'));
    const char *p = hex ? rest + 2 : rest;

    if (*p == '\0') return false;

    for (; *p; p++) {
        uint32_t d;
        if (*p >= '0' && *p <= '9')      d = (uint32_t)(*p - '0');
        else if (hex && *p >= 'a' && *p <= 'f') d = (uint32_t)(*p - 'a' + 10);
        else if (hex && *p >= 'A' && *p <= 'F') d = (uint32_t)(*p - 'A' + 10);
        else return false;

        v = v * (hex ? 16u : 10u) + d;
        if (v > 0xFFFFu) return false;
    }

    *out = v;
    return true;
}

static rv9_io_err_t ifm_open(rv9_path_t *path, const char *rest)
{
    rv9_dev_t *dev = path->dev;

    if (dev->drv == NULL || dev->drv->xfer == NULL) {
        return RV9_IO_ERR_UNSUPPORTED;
    }

    uint32_t unit = 0;
    if (!parse_unit(rest, &unit)) {
        ESP_LOGW(TAG, "%s: '%s' is not an address; try %s/0x68",
                 dev->name, rest ? rest : "", dev->name);
        return RV9_IO_ERR_INVAL;
    }

    ifm_path_t *st = rv9_calloc(1, sizeof(*st));
    if (st == NULL) return RV9_IO_ERR_NOMEM;

    st->unit = unit;
    st->reg  = RV9_IFM_REG_NONE;

    if (dev->drv->xfer_open != NULL) {
        rv9_io_err_t e = dev->drv->xfer_open(dev, unit, path->mode,
                                             &st->unit_state);
        if (e != RV9_IO_OK) {
            rv9_free(st);
            return e;
        }
    }

    path->fm_state = st;
    return RV9_IO_OK;
}

static rv9_io_err_t ifm_close(rv9_path_t *path)
{
    ifm_path_t *st = (ifm_path_t *)path->fm_state;
    if (st == NULL) return RV9_IO_OK;

    if (path->dev->drv->xfer_close != NULL) {
        path->dev->drv->xfer_close(path->dev, st->unit_state);
    }

    path->fm_state = NULL;
    rv9_free(st);
    return RV9_IO_OK;
}

static rv9_io_err_t ifm_read(rv9_path_t *path, void *buf, size_t len,
                             size_t *done)
{
    ifm_path_t *st = (ifm_path_t *)path->fm_state;
    if (st == NULL || buf == NULL) return RV9_IO_ERR_INVAL;
    if (done) *done = 0;
    if (len == 0) return RV9_IO_OK;
    if (len > IFM_MAX_XFER) len = IFM_MAX_XFER;

    rv9_io_err_t e;
    if (st->reg == RV9_IFM_REG_NONE) {
        e = path->dev->drv->xfer(path->dev, st->unit_state, NULL, 0, buf, len);
    } else {
        /* The combined transaction: the register number goes out, and the
           bus is held through the turnaround. */
        uint8_t reg = (uint8_t)st->reg;
        e = path->dev->drv->xfer(path->dev, st->unit_state, &reg, 1, buf, len);
    }

    if (e == RV9_IO_OK && done) *done = len;
    return e;
}

static rv9_io_err_t ifm_write(rv9_path_t *path, const void *buf, size_t len,
                              size_t *done)
{
    ifm_path_t *st = (ifm_path_t *)path->fm_state;
    if (st == NULL || buf == NULL) return RV9_IO_ERR_INVAL;
    if (done) *done = 0;
    if (len == 0) return RV9_IO_OK;
    if (len > IFM_MAX_XFER - 1) return RV9_IO_ERR_INVAL;

    rv9_io_err_t e;
    if (st->reg == RV9_IFM_REG_NONE) {
        e = path->dev->drv->xfer(path->dev, st->unit_state, buf, len, NULL, 0);
    } else {
        /*
         * A register write is the register number followed by the bytes,
         * in one transaction. Joined here rather than asked of the caller,
         * because the alternative is every caller building the same array
         * and one of them getting it wrong.
         */
        uint8_t out[IFM_MAX_XFER];
        out[0] = (uint8_t)st->reg;
        memcpy(out + 1, buf, len);
        e = path->dev->drv->xfer(path->dev, st->unit_state, out, len + 1,
                                 NULL, 0);
    }

    if (e == RV9_IO_OK && done) *done = len;
    return e;
}

static rv9_io_err_t ifm_setstat(rv9_path_t *path, uint32_t code, void *arg)
{
    ifm_path_t *st = (ifm_path_t *)path->fm_state;
    if (st == NULL || arg == NULL) return RV9_IO_ERR_INVAL;

    switch (code) {
    case RV9_IFM_SS_REG:
        st->reg = *(uint32_t *)arg;
        return RV9_IO_OK;
    default:
        return RV9_IO_ERR_UNSUPPORTED;
    }
}

static rv9_io_err_t ifm_getstat(rv9_path_t *path, uint32_t code, void *arg)
{
    ifm_path_t *st = (ifm_path_t *)path->fm_state;
    if (st == NULL || arg == NULL) return RV9_IO_ERR_INVAL;

    switch (code) {
    case RV9_IFM_SS_REG:
        *(uint32_t *)arg = st->reg;
        return RV9_IO_OK;

    case RV9_IFM_GS_PRESENT: {
        if (path->dev->drv->xfer_probe == NULL) return RV9_IO_ERR_UNSUPPORTED;
        rv9_io_err_t e = path->dev->drv->xfer_probe(path->dev, st->unit);
        *(uint32_t *)arg = (e == RV9_IO_OK) ? 1u : 0u;
        return RV9_IO_OK;     /* absent is an answer, not a failure */
    }

    default:
        return RV9_IO_ERR_UNSUPPORTED;
    }
}

/* A bus has no position to seek to, and saying so is better than
   pretending to have moved. */
static rv9_io_err_t ifm_seek(rv9_path_t *path, int64_t offset, int whence)
{
    (void)path; (void)offset; (void)whence;
    return RV9_IO_ERR_UNSUPPORTED;
}

static const rv9_filemgr_t ifm = {
    .name    = "ifm",
    .open    = ifm_open,
    .close   = ifm_close,
    .read    = ifm_read,
    .write   = ifm_write,
    .seek    = ifm_seek,
    .getstat = ifm_getstat,
    .setstat = ifm_setstat,
};

rv9_io_err_t rv9_ifm_register(void)
{
    return rv9_io_register_filemgr(&ifm);
}
