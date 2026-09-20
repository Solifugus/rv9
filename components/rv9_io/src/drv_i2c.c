/*
 * i2c -- the two-wire bus, as a transaction device under IFM.
 *
 * Until this existed, RV-9 could read an analogue voltage and its own die
 * temperature, and that was the whole of its sensing. Almost every sensor
 * anybody would reach for -- inertial units, time-of-flight rangers,
 * magnetometers, encoders, pressure -- speaks I2C, so a sensor-to-actuator
 * loop could be built with an actuator and no sensor.
 *
 * The bus is opened once at attach. Each path opened under it adds a
 * device handle for one address, which is how ESP-IDF's driver wants to be
 * used and also how the hardware behaves: the bus is shared, the address
 * is not.
 *
 * Descriptor options:
 *   opt[0]  SDA pin          (default 8)
 *   opt[1]  SCL pin          (default 9)
 *   opt[2]  bus speed in kHz (default 100)
 *
 * Both lines need pull-ups. The internal ones are enabled because a bare
 * sensor on a breadboard often has none, and a bus with no pull-up reads
 * as a bus with nothing on it -- which is a confusing way to spend an
 * evening. They are weak; a board carrying its own 4.7k resistors is
 * better, and at 400 kHz it is required.
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "rv9-i2c";

#define OPT_SDA   0
#define OPT_SCL   1
#define OPT_KHZ   2

#define DEFAULT_SDA  8
#define DEFAULT_SCL  9
#define DEFAULT_KHZ  100

/* Long enough that a slow device is not mistaken for a missing one, short
   enough that a real-time caller finds out rather than waiting. */
#define XFER_TIMEOUT_MS 50

typedef struct {
    i2c_master_bus_handle_t bus;
    uint32_t                khz;
} i2c_bus_t;

typedef struct {
    i2c_master_dev_handle_t dev;
    uint32_t                addr;
} i2c_unit_t;

static rv9_io_err_t i2c_init(rv9_dev_t *dev)
{
    uint32_t sda = dev->opt[OPT_SDA] ? dev->opt[OPT_SDA] : DEFAULT_SDA;
    uint32_t scl = dev->opt[OPT_SCL] ? dev->opt[OPT_SCL] : DEFAULT_SCL;
    uint32_t khz = dev->opt[OPT_KHZ] ? dev->opt[OPT_KHZ] : DEFAULT_KHZ;

    if (sda == scl) {
        ESP_LOGE(TAG, "%s: SDA and SCL are both pin %lu", dev->name,
                 (unsigned long)sda);
        return RV9_IO_ERR_INVAL;
    }

    i2c_bus_t *b = rv9_calloc(1, sizeof(*b));
    if (b == NULL) return RV9_IO_ERR_NOMEM;

    i2c_master_bus_config_t cfg = {
        .i2c_port          = -1,        /* let the driver choose a port */
        .sda_io_num        = (int)sda,
        .scl_io_num        = (int)scl,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };

    if (i2c_new_master_bus(&cfg, &b->bus) != ESP_OK) {
        ESP_LOGE(TAG, "%s: no bus on SDA %lu SCL %lu", dev->name,
                 (unsigned long)sda, (unsigned long)scl);
        rv9_free(b);
        return RV9_IO_ERR_IO;
    }

    b->khz         = khz;
    dev->drv_state = b;

    ESP_LOGI(TAG, "%s: SDA %lu, SCL %lu, %lu kHz", dev->name,
             (unsigned long)sda, (unsigned long)scl, (unsigned long)khz);
    return RV9_IO_OK;
}

static rv9_io_err_t i2c_xfer_open(rv9_dev_t *dev, uint32_t unit, uint32_t mode,
                                  void **unit_state)
{
    (void)mode;
    i2c_bus_t *b = (i2c_bus_t *)dev->drv_state;
    if (b == NULL) return RV9_IO_ERR_IO;

    /* Seven-bit addressing. 0x00 is the general call and 0x78 upward are
       reserved; refusing them here is better than a transaction that fails
       for a reason the caller cannot see. */
    if (unit == 0 || unit > 0x77u) {
        ESP_LOGW(TAG, "%s: 0x%02lx is not a usable address", dev->name,
                 (unsigned long)unit);
        return RV9_IO_ERR_INVAL;
    }

    i2c_unit_t *u = rv9_calloc(1, sizeof(*u));
    if (u == NULL) return RV9_IO_ERR_NOMEM;

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = (uint16_t)unit,
        .scl_speed_hz    = b->khz * 1000u,
    };

    if (i2c_master_bus_add_device(b->bus, &dcfg, &u->dev) != ESP_OK) {
        rv9_free(u);
        return RV9_IO_ERR_IO;
    }

    u->addr     = unit;
    *unit_state = u;
    return RV9_IO_OK;
}

static rv9_io_err_t i2c_xfer_close(rv9_dev_t *dev, void *unit_state)
{
    (void)dev;
    i2c_unit_t *u = (i2c_unit_t *)unit_state;
    if (u == NULL) return RV9_IO_OK;

    i2c_master_bus_rm_device(u->dev);
    rv9_free(u);
    return RV9_IO_OK;
}

/*
 * The three shapes, which are one call because that is what the hardware
 * does. Write-then-read is the one that matters: it keeps the bus through
 * the turnaround, which is what a datasheet means by a repeated start and
 * what several devices require rather than merely prefer.
 */
static rv9_io_err_t i2c_xfer(rv9_dev_t *dev, void *unit_state,
                             const void *wbuf, size_t wlen,
                             void *rbuf, size_t rlen)
{
    (void)dev;
    i2c_unit_t *u = (i2c_unit_t *)unit_state;
    if (u == NULL) return RV9_IO_ERR_IO;

    esp_err_t e;
    if (wlen > 0 && rlen > 0) {
        e = i2c_master_transmit_receive(u->dev, (const uint8_t *)wbuf, wlen,
                                        (uint8_t *)rbuf, rlen,
                                        XFER_TIMEOUT_MS);
    } else if (wlen > 0) {
        e = i2c_master_transmit(u->dev, (const uint8_t *)wbuf, wlen,
                                XFER_TIMEOUT_MS);
    } else if (rlen > 0) {
        e = i2c_master_receive(u->dev, (uint8_t *)rbuf, rlen, XFER_TIMEOUT_MS);
    } else {
        return RV9_IO_OK;
    }

    if (e == ESP_ERR_TIMEOUT) return RV9_IO_ERR_TIMEOUT;
    return (e == ESP_OK) ? RV9_IO_OK : RV9_IO_ERR_IO;
}

/*
 * Does anything answer here?
 *
 * One byte on the bus, and the absence of an acknowledgement is the
 * answer rather than a fault -- which is what makes scanning a bus a
 * reasonable thing to do rather than a hundred error messages.
 */
static rv9_io_err_t i2c_xfer_probe(rv9_dev_t *dev, uint32_t unit)
{
    i2c_bus_t *b = (i2c_bus_t *)dev->drv_state;
    if (b == NULL) return RV9_IO_ERR_IO;

    esp_err_t e = i2c_master_probe(b->bus, (uint16_t)unit, XFER_TIMEOUT_MS);
    return (e == ESP_OK) ? RV9_IO_OK : RV9_IO_ERR_NOTFOUND;
}

static const rv9_driver_t i2c = {
    .name = "i2c",
    /*
     * A sensor keeps whatever it was configured to do when the last path
     * to it closes -- the chip is still awake, still at the sample rate it
     * was given. RV-9 did not put it into that state and cannot take it
     * out; saying `retains` is the truthful description of a device this
     * layer does not own the state of.
     */
    .retains    = true,
    .init       = i2c_init,
    .xfer_open  = i2c_xfer_open,
    .xfer_close = i2c_xfer_close,
    .xfer       = i2c_xfer,
    .xfer_probe = i2c_xfer_probe,
};

rv9_io_err_t rv9_drv_i2c_register(void)
{
    return rv9_io_register_driver(&i2c);
}
