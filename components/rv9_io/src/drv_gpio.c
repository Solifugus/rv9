/*
 * gpio driver -- pins as units.
 *
 *     /gpio/8      pin 8
 *
 * A pin opened for writing becomes an output; one opened for reading
 * becomes an input. That is not a shortcut: the mode a path is opened with
 * already says what the caller intends to do with it, so making them agree
 * saves a configuration step that could only ever disagree. Open RW to set
 * a level and read it back.
 *
 * Direction and pull can still be set explicitly through setstat.
 *
 * Closing a path does not release the pin: a level set stays set. An
 * enable line that dropped when the program that raised it exited would be
 * worse than useless. /pwm0 takes the opposite view for the opposite
 * reason -- see that driver.
 */
#include "rv9/io.h"
#include "rv9/kal.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "rv9-gpio";

/*
 * Pins this board has already spoken for.
 *
 * The USB pair is the important entry and was the one I left out. GPIO 13
 * and 14 carry the USB Serial/JTAG lines, which are the console, the
 * flashing channel and the only way to talk to the board. Reconfiguring
 * either takes the machine away mid-command and leaves it needing a
 * physical reset -- which is exactly what happened the first time this
 * driver met a `pin 13`.
 *
 * A device driver that can disconnect the operator should refuse to,
 * unless asked very deliberately. There is no override here yet; when
 * there is, it should be harder to reach than a typo.
 */
static bool pin_is_reserved(uint32_t pin)
{
    switch (pin) {
    case 13: case 14:                                     /* USB: the console */
    case 6: case 7: case 23: case 24: case 26: case 10:   /* LCD */
    case 4: case 5:                                       /* microSD */
        return true;
    default:
        return false;
    }
}

typedef struct {
    uint32_t pin;
    bool     output;
} gpio_unit_t;

static rv9_io_err_t gpio_unit_open(rv9_dev_t *dev, uint32_t unit,
                                   uint32_t mode, void **out_state)
{
    (void)dev;

    if (!GPIO_IS_VALID_GPIO((gpio_num_t)unit)) return RV9_IO_ERR_NOTFOUND;
    if (pin_is_reserved(unit)) {
        ESP_LOGW(TAG, "pin %lu is spoken for (USB console, display or card)",
                 (unsigned long)unit);
        return RV9_IO_ERR_EXISTS;
    }

    gpio_unit_t *u = rv9_calloc(1, sizeof(*u));
    if (u == NULL) return RV9_IO_ERR_NOMEM;

    u->pin    = unit;
    u->output = (mode & RV9_MODE_WRITE) != 0;

    /*
     * Reclaim the pin first.
     *
     * A GPIO on this chip usually comes up routed to some peripheral
     * through the IOMUX, and configuring it as GPIO does not undo that --
     * so the pin reads and writes as if nothing happened, which is exactly
     * how it presented: every pin accepted a write and read back zero.
     */
    gpio_reset_pin((gpio_num_t)unit);

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << unit,
        .mode = u->output ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        rv9_free(u);
        return RV9_IO_ERR_IO;
    }

    *out_state = u;
    return RV9_IO_OK;
}

static rv9_io_err_t gpio_unit_close(rv9_dev_t *dev, void *state)
{
    (void)dev;
    rv9_free(state);
    return RV9_IO_OK;
}

/*
 * Resident, and so are the two ESP-IDF calls they make: gpio_set_level and
 * gpio_get_level are mapped 'noflash'. A pin is therefore the one device a
 * control loop can drive while the flash cache is off. PWM and the ADC are
 * not: their drivers take mutexes and live in flash.
 */
static RV9_RT_CODE rv9_io_err_t gpio_unit_read(rv9_dev_t *dev, void *state,
                                               uint32_t *value)
{
    (void)dev;
    gpio_unit_t *u = (gpio_unit_t *)state;
    if (u == NULL) return RV9_IO_ERR_IO;

    *value = (uint32_t)gpio_get_level((gpio_num_t)u->pin);
    return RV9_IO_OK;
}

static RV9_RT_CODE rv9_io_err_t gpio_unit_write(rv9_dev_t *dev, void *state,
                                                uint32_t value)
{
    (void)dev;
    gpio_unit_t *u = (gpio_unit_t *)state;
    if (u == NULL) return RV9_IO_ERR_IO;
    if (!u->output) return RV9_IO_ERR_MODE;

    gpio_set_level((gpio_num_t)u->pin, value ? 1 : 0);
    return RV9_IO_OK;
}

static rv9_io_err_t gpio_unit_stat(rv9_dev_t *dev, void *state, bool set,
                                   uint32_t code, uint32_t *value)
{
    (void)dev;
    gpio_unit_t *u = (gpio_unit_t *)state;
    if (u == NULL || value == NULL) return RV9_IO_ERR_IO;

    switch (code) {
    case RV9_PIO_SS_DIRECTION:
        if (!set) { *value = u->output ? 1 : 0; return RV9_IO_OK; }
        u->output = (*value != 0);
        gpio_set_direction((gpio_num_t)u->pin,
                           u->output ? GPIO_MODE_INPUT_OUTPUT
                                     : GPIO_MODE_INPUT);
        return RV9_IO_OK;

    case RV9_PIO_SS_PULL:
        if (!set) return RV9_IO_ERR_UNSUPPORTED;
        gpio_set_pull_mode((gpio_num_t)u->pin,
                           (*value == 1) ? GPIO_PULLUP_ONLY :
                           (*value == 2) ? GPIO_PULLDOWN_ONLY : GPIO_FLOATING);
        return RV9_IO_OK;

    case RV9_PIO_GS_RANGE:
        if (set) return RV9_IO_ERR_UNSUPPORTED;
        *value = 1;                       /* a pin is one bit */
        return RV9_IO_OK;

    default:
        return RV9_IO_ERR_UNSUPPORTED;
    }
}

static const rv9_driver_t gpio_drv = {
    .name       = "gpio",
    .unit_open  = gpio_unit_open,
    .unit_close = gpio_unit_close,
    .unit_read  = gpio_unit_read,
    .unit_write = gpio_unit_write,
    .unit_stat  = gpio_unit_stat,
};

rv9_io_err_t rv9_drv_gpio_register(void)
{
    return rv9_io_register_driver(&gpio_drv);
}
