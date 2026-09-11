/*
 * pwm driver -- a duty cycle on a pin.
 *
 *     /pwm0/3      PWM on pin 3
 *
 * The unit is the pin, not the hardware channel: a caller wanting to drive
 * a servo knows which wire it is on and should not have to know how many
 * LEDC channels the chip has or which are free. The driver allocates a
 * channel and remembers the mapping.
 *
 * Writes carry duty directly, 0 to the range reported by RV9_PIO_GS_RANGE.
 * Frequency is a setstat, because it is configuration rather than data --
 * and for a servo it is set once and never touched again.
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "rv9-pwm";

/* Descriptor options */
#define OPT_FREQUENCY 0
#define OPT_BITS      1

#define DEFAULT_FREQ_HZ 50        /* servos, unless told otherwise */
#define DEFAULT_BITS    LEDC_TIMER_14_BIT
#define MAX_CHANNELS    4

typedef struct {
    uint32_t          pin;
    ledc_channel_t    channel;
    bool              in_use;
} pwm_unit_t;

typedef struct {
    uint32_t     freq_hz;
    uint32_t     bits;
    bool         timer_ready;
    pwm_unit_t   units[MAX_CHANNELS];
} pwm_dev_t;

static rv9_io_err_t pwm_init(rv9_dev_t *dev)
{
    pwm_dev_t *p = rv9_calloc(1, sizeof(*p));
    if (p == NULL) return RV9_IO_ERR_NOMEM;

    p->freq_hz = dev->opt[OPT_FREQUENCY] ? dev->opt[OPT_FREQUENCY]
                                         : DEFAULT_FREQ_HZ;
    p->bits    = dev->opt[OPT_BITS] ? dev->opt[OPT_BITS] : DEFAULT_BITS;

    dev->drv_state = p;
    ESP_LOGI(TAG, "%lu Hz, %lu-bit duty (0..%lu)",
             (unsigned long)p->freq_hz, (unsigned long)p->bits,
             (unsigned long)((1u << p->bits) - 1));
    return RV9_IO_OK;
}

static rv9_io_err_t ensure_timer(pwm_dev_t *p)
{
    if (p->timer_ready) return RV9_IO_OK;

    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = (ledc_timer_bit_t)p->bits,
        .freq_hz         = p->freq_hz,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&t) != ESP_OK) return RV9_IO_ERR_IO;

    p->timer_ready = true;
    return RV9_IO_OK;
}

static rv9_io_err_t pwm_unit_open(rv9_dev_t *dev, uint32_t unit,
                                  uint32_t mode, void **out_state)
{
    (void)mode;
    pwm_dev_t *p = (pwm_dev_t *)dev->drv_state;
    if (p == NULL) return RV9_IO_ERR_IO;
    if (!GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)unit)) return RV9_IO_ERR_NOTFOUND;

    rv9_io_err_t err = ensure_timer(p);
    if (err != RV9_IO_OK) return err;

    pwm_unit_t *u = NULL;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (!p->units[i].in_use) {
            u = &p->units[i];
            u->channel = (ledc_channel_t)i;
            break;
        }
    }
    if (u == NULL) return RV9_IO_ERR_NOPATHS;

    ledc_channel_config_t c = {
        .gpio_num   = (int)unit,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = u->channel,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&c) != ESP_OK) return RV9_IO_ERR_IO;

    u->pin    = unit;
    u->in_use = true;

    *out_state = u;
    ESP_LOGI(TAG, "pin %lu on channel %d", (unsigned long)unit, (int)u->channel);
    return RV9_IO_OK;
}

static rv9_io_err_t pwm_unit_close(rv9_dev_t *dev, void *state)
{
    (void)dev;
    pwm_unit_t *u = (pwm_unit_t *)state;
    if (u == NULL) return RV9_IO_OK;

    /* Stop driving rather than leaving the last duty applied: an actuator
       left running because a program exited is a bad way to find out. */
    ledc_stop(LEDC_LOW_SPEED_MODE, u->channel, 0);
    u->in_use = false;
    return RV9_IO_OK;
}

static rv9_io_err_t pwm_unit_read(rv9_dev_t *dev, void *state, uint32_t *value)
{
    (void)dev;
    pwm_unit_t *u = (pwm_unit_t *)state;
    if (u == NULL) return RV9_IO_ERR_IO;

    *value = ledc_get_duty(LEDC_LOW_SPEED_MODE, u->channel);
    return RV9_IO_OK;
}

static rv9_io_err_t pwm_unit_write(rv9_dev_t *dev, void *state, uint32_t value)
{
    pwm_dev_t *p = (pwm_dev_t *)dev->drv_state;
    pwm_unit_t *u = (pwm_unit_t *)state;
    if (u == NULL || p == NULL) return RV9_IO_ERR_IO;

    uint32_t max = (1u << p->bits) - 1;
    if (value > max) value = max;

    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, u->channel, value) != ESP_OK ||
        ledc_update_duty(LEDC_LOW_SPEED_MODE, u->channel) != ESP_OK) {
        return RV9_IO_ERR_IO;
    }
    return RV9_IO_OK;
}

static rv9_io_err_t pwm_unit_stat(rv9_dev_t *dev, void *state, bool set,
                                  uint32_t code, uint32_t *value)
{
    pwm_dev_t *p = (pwm_dev_t *)dev->drv_state;
    (void)state;
    if (p == NULL || value == NULL) return RV9_IO_ERR_IO;

    switch (code) {
    case RV9_PIO_SS_FREQUENCY:
        if (!set) { *value = p->freq_hz; return RV9_IO_OK; }
        if (ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, *value) != ESP_OK) {
            return RV9_IO_ERR_IO;
        }
        p->freq_hz = *value;
        return RV9_IO_OK;

    case RV9_PIO_GS_RANGE:
        if (set) return RV9_IO_ERR_UNSUPPORTED;
        *value = (1u << p->bits) - 1;
        return RV9_IO_OK;

    default:
        return RV9_IO_ERR_UNSUPPORTED;
    }
}

static const rv9_driver_t pwm_drv = {
    .name       = "pwm",
    .init       = pwm_init,
    .unit_open  = pwm_unit_open,
    .unit_close = pwm_unit_close,
    .unit_read  = pwm_unit_read,
    .unit_write = pwm_unit_write,
    .unit_stat  = pwm_unit_stat,
};

rv9_io_err_t rv9_drv_pwm_register(void)
{
    return rv9_io_register_driver(&pwm_drv);
}
