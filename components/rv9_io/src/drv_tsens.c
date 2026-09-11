/*
 * tsens driver -- the die temperature, as a device.
 *
 *     /tsens/0     the chip's own temperature
 *
 * Reads return hundredths of a degree Celsius, so 4512 is 45.12 C. An
 * integer because the PIO discipline carries integers, and because a
 * control loop deciding whether to throttle does not want a float.
 *
 * Worth having on a board that will end up somewhere warm with its radio
 * on: "the chip feels hot" is not a measurement, and a robot cannot feel
 * anything.
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include "driver/temperature_sensor.h"
#include "esp_log.h"

static const char *TAG = "rv9-tsens";

typedef struct {
    temperature_sensor_handle_t handle;
    bool                        enabled;
} tsens_dev_t;

static rv9_io_err_t tsens_init(rv9_dev_t *dev)
{
    tsens_dev_t *t = rv9_calloc(1, sizeof(*t));
    if (t == NULL) return RV9_IO_ERR_NOMEM;

    /* The range chosen affects accuracy; -10..80 covers a board indoors
       and a board in a warm enclosure without straying into the wide,
       less precise settings. */
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);

    if (temperature_sensor_install(&cfg, &t->handle) != ESP_OK) {
        ESP_LOGE(TAG, "no temperature sensor");
        rv9_free(t);
        return RV9_IO_ERR_IO;
    }
    if (temperature_sensor_enable(t->handle) != ESP_OK) {
        rv9_free(t);
        return RV9_IO_ERR_IO;
    }

    t->enabled = true;
    dev->drv_state = t;

    float c = 0;
    if (temperature_sensor_get_celsius(t->handle, &c) == ESP_OK) {
        ESP_LOGI(TAG, "die temperature %d.%02d C",
                 (int)c, (int)((c - (int)c) * 100));
    }
    return RV9_IO_OK;
}

static rv9_io_err_t tsens_unit_open(rv9_dev_t *dev, uint32_t unit,
                                    uint32_t mode, void **out_state)
{
    (void)dev;
    if (unit != 0) return RV9_IO_ERR_NOTFOUND;
    if (mode & RV9_MODE_WRITE) return RV9_IO_ERR_MODE;

    *out_state = (void *)1;     /* one unit; no per-path state to keep */
    return RV9_IO_OK;
}

static rv9_io_err_t tsens_unit_close(rv9_dev_t *dev, void *state)
{
    (void)dev; (void)state;
    return RV9_IO_OK;
}

static rv9_io_err_t tsens_unit_read(rv9_dev_t *dev, void *state,
                                    uint32_t *value)
{
    (void)state;
    tsens_dev_t *t = (tsens_dev_t *)dev->drv_state;
    if (t == NULL || !t->enabled) return RV9_IO_ERR_IO;

    float c = 0;
    if (temperature_sensor_get_celsius(t->handle, &c) != ESP_OK) {
        return RV9_IO_ERR_IO;
    }

    *value = (uint32_t)(int32_t)(c * 100.0f);
    return RV9_IO_OK;
}

static const rv9_driver_t tsens_drv = {
    .name       = "tsens",
    .init       = tsens_init,
    .unit_open  = tsens_unit_open,
    .unit_close = tsens_unit_close,
    .unit_read  = tsens_unit_read,
};

rv9_io_err_t rv9_drv_tsens_register(void)
{
    return rv9_io_register_driver(&tsens_drv);
}
