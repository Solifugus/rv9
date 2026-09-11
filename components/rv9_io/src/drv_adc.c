/*
 * adc driver -- analogue inputs as units.
 *
 *     /adc0/1      channel 1
 *
 * Reads return the raw conversion, not millivolts. Calibration is a
 * property of the board and the sensor rather than of the converter, and
 * a control loop usually wants the raw number anyway -- it is going to
 * scale it into its own units regardless, and doing that twice loses
 * precision for no benefit.
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "rv9-adc";

/* Descriptor options */
#define OPT_ATTEN 0

typedef struct {
    adc_oneshot_unit_handle_t handle;
    adc_atten_t               atten;
} adc_dev_t;

typedef struct {
    adc_channel_t channel;
} adc_unit_state_t;

static rv9_io_err_t adc_init(rv9_dev_t *dev)
{
    adc_dev_t *a = rv9_calloc(1, sizeof(*a));
    if (a == NULL) return RV9_IO_ERR_NOMEM;

    /* 12 dB by default: the widest input range, which is what a sensor
       wired to 3.3 V needs. */
    a->atten = dev->opt[OPT_ATTEN] ? (adc_atten_t)dev->opt[OPT_ATTEN]
                                   : ADC_ATTEN_DB_12;

    adc_oneshot_unit_init_cfg_t cfg = {
        .unit_id = ADC_UNIT_1,
    };
    if (adc_oneshot_new_unit(&cfg, &a->handle) != ESP_OK) {
        ESP_LOGE(TAG, "could not open ADC1");
        rv9_free(a);
        return RV9_IO_ERR_IO;
    }

    dev->drv_state = a;
    ESP_LOGI(TAG, "ADC1 ready, attenuation %d", (int)a->atten);
    return RV9_IO_OK;
}

static rv9_io_err_t adc_unit_open(rv9_dev_t *dev, uint32_t unit, uint32_t mode,
                                  void **out_state)
{
    adc_dev_t *a = (adc_dev_t *)dev->drv_state;
    if (a == NULL) return RV9_IO_ERR_IO;
    if (mode & RV9_MODE_WRITE) return RV9_IO_ERR_MODE;   /* it is an input */

    adc_oneshot_chan_cfg_t cfg = {
        .atten    = a->atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(a->handle, (adc_channel_t)unit,
                                   &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "channel %lu not available", (unsigned long)unit);
        return RV9_IO_ERR_NOTFOUND;
    }

    adc_unit_state_t *u = rv9_calloc(1, sizeof(*u));
    if (u == NULL) return RV9_IO_ERR_NOMEM;

    u->channel = (adc_channel_t)unit;
    *out_state = u;
    return RV9_IO_OK;
}

static rv9_io_err_t adc_unit_close(rv9_dev_t *dev, void *state)
{
    (void)dev;
    rv9_free(state);
    return RV9_IO_OK;
}

static rv9_io_err_t adc_unit_read(rv9_dev_t *dev, void *state, uint32_t *value)
{
    adc_dev_t *a = (adc_dev_t *)dev->drv_state;
    adc_unit_state_t *u = (adc_unit_state_t *)state;
    if (a == NULL || u == NULL) return RV9_IO_ERR_IO;

    int raw = 0;
    if (adc_oneshot_read(a->handle, u->channel, &raw) != ESP_OK) {
        return RV9_IO_ERR_IO;
    }

    *value = (uint32_t)raw;
    return RV9_IO_OK;
}

static rv9_io_err_t adc_unit_stat(rv9_dev_t *dev, void *state, bool set,
                                  uint32_t code, uint32_t *value)
{
    (void)dev; (void)state;
    if (value == NULL) return RV9_IO_ERR_INVAL;

    if (code == RV9_PIO_GS_RANGE && !set) {
        *value = 4095;               /* 12-bit conversions */
        return RV9_IO_OK;
    }
    return RV9_IO_ERR_UNSUPPORTED;
}

static const rv9_driver_t adc_drv = {
    .name       = "adc",
    .init       = adc_init,
    .unit_open  = adc_unit_open,
    .unit_close = adc_unit_close,
    .unit_read  = adc_unit_read,
    .unit_stat  = adc_unit_stat,
};

rv9_io_err_t rv9_drv_adc_register(void)
{
    return rv9_io_register_driver(&adc_drv);
}
