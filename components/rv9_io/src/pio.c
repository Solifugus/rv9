/*
 * PIO -- the peripheral file manager.
 *
 * A third discipline, after SCF's character streams and RBF's blocks. Pins,
 * PWM outputs and ADC channels are addressable units carrying a value, and
 * the path names the unit:
 *
 *     /gpio/8        pin 8
 *     /pwm0/3        a PWM output on pin 3
 *     /adc0/1        analogue channel 1
 *
 * A read or write moves one 32-bit value, not text. That is deliberate:
 * the fast path here is a control loop writing a duty cycle every
 * millisecond, and making it format decimal first would be a strange way
 * to spend a microsecond. Utilities convert for humans at the shell, where
 * a microsecond does not matter.
 *
 * Configuration -- direction, pull, frequency -- goes through
 * getstat/setstat, which is what those calls are for.
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"

static const char *TAG = "rv9-pio";

typedef struct {
    uint32_t unit;
    void    *unit_state;
} pio_path_t;

static bool parse_unit(const char *rest, uint32_t *out)
{
    if (rest == NULL || rest[0] == '\0') return false;

    uint32_t v = 0;
    for (uint32_t i = 0; rest[i]; i++) {
        if (rest[i] < '0' || rest[i] > '9') return false;
        v = v * 10 + (uint32_t)(rest[i] - '0');
    }
    *out = v;
    return true;
}

static rv9_io_err_t pio_open(rv9_path_t *path, const char *rest)
{
    rv9_dev_t *dev = path->dev;
    if (dev->drv->unit_open == NULL) return RV9_IO_ERR_UNSUPPORTED;

    uint32_t unit = 0;
    if (!parse_unit(rest, &unit)) {
        ESP_LOGW(TAG, "%s: '%s' is not a unit number", dev->name,
                 rest ? rest : "");
        return RV9_IO_ERR_INVAL;
    }

    pio_path_t *st = rv9_calloc(1, sizeof(*st));
    if (st == NULL) return RV9_IO_ERR_NOMEM;

    st->unit = unit;
    rv9_io_err_t err = dev->drv->unit_open(dev, unit, path->mode,
                                           &st->unit_state);
    if (err != RV9_IO_OK) {
        rv9_free(st);
        return err;
    }

    strncpy(path->name, rest, sizeof(path->name) - 1);
    path->fm_state = st;
    return RV9_IO_OK;
}

static rv9_io_err_t pio_close(rv9_path_t *path)
{
    pio_path_t *st = (pio_path_t *)path->fm_state;
    if (st == NULL) return RV9_IO_OK;

    if (path->dev->drv->unit_close) {
        path->dev->drv->unit_close(path->dev, st->unit_state);
    }
    rv9_free(st);
    path->fm_state = NULL;
    return RV9_IO_OK;
}

static rv9_io_err_t pio_read(rv9_path_t *path, void *buf, size_t len,
                             size_t *done)
{
    pio_path_t *st = (pio_path_t *)path->fm_state;
    if (st == NULL) return RV9_IO_ERR_IO;
    if (len < sizeof(uint32_t)) return RV9_IO_ERR_INVAL;
    if (path->dev->drv->unit_read == NULL) return RV9_IO_ERR_UNSUPPORTED;

    uint32_t value = 0;
    rv9_io_err_t err = path->dev->drv->unit_read(path->dev, st->unit_state,
                                                 &value);
    if (err != RV9_IO_OK) return err;

    memcpy(buf, &value, sizeof(value));
    if (done) *done = sizeof(value);
    return RV9_IO_OK;
}

static rv9_io_err_t pio_write(rv9_path_t *path, const void *buf, size_t len,
                              size_t *done)
{
    pio_path_t *st = (pio_path_t *)path->fm_state;
    if (st == NULL) return RV9_IO_ERR_IO;
    if (len < sizeof(uint32_t)) return RV9_IO_ERR_INVAL;
    if (path->dev->drv->unit_write == NULL) return RV9_IO_ERR_UNSUPPORTED;

    uint32_t value = 0;
    memcpy(&value, buf, sizeof(value));

    rv9_io_err_t err = path->dev->drv->unit_write(path->dev, st->unit_state,
                                                  value);
    if (err == RV9_IO_OK && done) *done = sizeof(value);
    return err;
}

/* A pin has no position. */
static rv9_io_err_t pio_seek(rv9_path_t *path, int64_t offset, int whence)
{
    (void)path; (void)offset; (void)whence;
    return RV9_IO_ERR_UNSUPPORTED;
}

static rv9_io_err_t pio_stat(rv9_path_t *path, bool set, uint32_t code,
                             void *arg)
{
    pio_path_t *st = (pio_path_t *)path->fm_state;
    if (st == NULL || arg == NULL) return RV9_IO_ERR_INVAL;
    if (path->dev->drv->unit_stat == NULL) return RV9_IO_ERR_UNSUPPORTED;

    return path->dev->drv->unit_stat(path->dev, st->unit_state, set, code,
                                     (uint32_t *)arg);
}

static rv9_io_err_t pio_getstat(rv9_path_t *path, uint32_t code, void *arg)
{
    return pio_stat(path, false, code, arg);
}

static rv9_io_err_t pio_setstat(rv9_path_t *path, uint32_t code, void *arg)
{
    return pio_stat(path, true, code, arg);
}

static const rv9_filemgr_t pio = {
    .name    = "pio",
    .open    = pio_open,
    .close   = pio_close,
    .read    = pio_read,
    .write   = pio_write,
    .seek    = pio_seek,
    .getstat = pio_getstat,
    .setstat = pio_setstat,
};

rv9_io_err_t rv9_pio_register(void)
{
    return rv9_io_register_filemgr(&pio);
}
