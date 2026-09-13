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
    uint32_t    pin;
    bool        output;

    /* Set when the pin has been armed for edges. The event is this unit's
       own: two processes watching the same pin open it twice and get one
       each, and neither can starve the other by draining a shared queue. */
    rv9_event_t event;
    uint32_t    edge;
} gpio_unit_t;

/*
 * The shared GPIO interrupt, installed once and on demand.
 *
 * ESP_INTR_FLAG_IRAM is the whole reason this is worth doing carefully: it
 * promises that neither ESP-IDF's dispatcher nor our handler will touch
 * flash, so an edge arriving while the cache is off still reaches the
 * process waiting for it. A pin that stops interrupting whenever the radio
 * saves its calibration data would not be an input a control system could
 * be built on.
 */
static bool s_isr_service;

/*
 * Resident. Everything it calls is too -- rv9_event_signal_from_isr and the
 * timestamp it takes -- and nothing here allocates, logs or takes a lock
 * that ordinary code holds.
 */
static RV9_RT_CODE void gpio_edge_isr(void *arg)
{
    gpio_unit_t *u = (gpio_unit_t *)arg;
    rv9_event_signal_from_isr(u->event);
}

/*
 * How many paths are open on each pin, and how many of them want to drive
 * it.
 *
 * A pin is one piece of hardware and more than one process may hold it --
 * one watching for edges, another changing the level. Without this, the
 * second open reset the pin and took the first opener's configuration with
 * it: direction, pull, and (once there were interrupts) the arming that the
 * first process was blocked waiting on. The pin stayed open and simply
 * stopped doing what it had been told.
 *
 * So the reset happens once, on the first open, and direction is the union
 * of what the openers asked for: if anyone wants to drive it, it is an
 * output, because an output on this chip still reads back.
 */
#define MAX_PINS 40
static uint8_t s_open_count[MAX_PINS];
static uint8_t s_output_count[MAX_PINS];

/*
 * Two things about a pin that outlive every path to it.
 *
 * `reclaimed` -- RV-9 has taken this pin away from the IOMUX. Doing that
 * again is destructive: gpio_reset_pin restores the peripheral routing and
 * the pull-up, which is exactly what we spent the first open undoing. It
 * must happen once per pin, not once per generation of openers.
 *
 * `driving` -- somebody has, at some point, opened this pin for writing.
 * It stays an output from then on. Direction is the union of what openers
 * want *and what the pin already is*, because the level a previous opener
 * set is only still there while something is driving it.
 *
 * Both were missing, and between them they made a documented guarantee
 * false: closing a pin preserved its level, and the very next open threw
 * it away. `pin 2 0` then `pin 2` read 1 -- the reset had re-enabled the
 * pull-up and the lone reader had stopped driving the pin. Everything the
 * close does carefully was undone by the open.
 */
static uint8_t s_reclaimed[MAX_PINS];
static uint8_t s_driving[MAX_PINS];
static rv9_lock_t s_lock;

static rv9_io_err_t gpio_unit_open(rv9_dev_t *dev, uint32_t unit,
                                   uint32_t mode, void **out_state)
{
    (void)dev;

    if (!GPIO_IS_VALID_GPIO((gpio_num_t)unit)) return RV9_IO_ERR_NOTFOUND;
    if (unit >= MAX_PINS) return RV9_IO_ERR_NOTFOUND;
    if (pin_is_reserved(unit)) {
        ESP_LOGW(TAG, "pin %lu is spoken for (USB console, display or card)",
                 (unsigned long)unit);
        return RV9_IO_ERR_EXISTS;
    }

    gpio_unit_t *u = rv9_calloc(1, sizeof(*u));
    if (u == NULL) return RV9_IO_ERR_NOMEM;

    u->pin    = unit;
    u->output = (mode & RV9_MODE_WRITE) != 0;

    rv9_lock_acquire(s_lock);
    bool first = (s_open_count[unit] == 0);
    s_open_count[unit]++;
    if (u->output) { s_output_count[unit]++; s_driving[unit] = 1; }

    /* Sticky: once a pin is an output it stays one. A reader arriving
       after a writer has gone must not stop the pin driving what the
       writer left on it. */
    bool drive     = (s_output_count[unit] > 0) || s_driving[unit];
    bool reclaim   = first && !s_reclaimed[unit];
    if (reclaim) s_reclaimed[unit] = 1;
    rv9_lock_release(s_lock);

    /*
     * Reclaim the pin, but only ever once.
     *
     * A GPIO on this chip usually comes up routed to some peripheral
     * through the IOMUX, and configuring it as GPIO does not undo that --
     * so the pin reads and writes as if nothing happened, which is exactly
     * how it presented: every pin accepted a write and read back zero.
     *
     * Doing it a second time is not harmless. gpio_reset_pin puts the
     * routing and the pull-up back, so an enable line somebody set is
     * quietly dropped the next time anything opens the pin -- including a
     * command that only wanted to read it.
     */
    esp_err_t err;
    if (first) {
        if (reclaim) gpio_reset_pin((gpio_num_t)unit);

        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << unit,
            .mode = drive ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&cfg);
    } else {
        /* Direction only. Everything else already belongs to someone. */
        err = gpio_set_direction((gpio_num_t)unit,
                                 drive ? GPIO_MODE_INPUT_OUTPUT
                                       : GPIO_MODE_INPUT);
    }

    if (err != ESP_OK) {
        rv9_lock_acquire(s_lock);
        s_open_count[unit]--;
        if (u->output) s_output_count[unit]--;
        rv9_lock_release(s_lock);
        rv9_free(u);
        return RV9_IO_ERR_IO;
    }

    *out_state = u;
    return RV9_IO_OK;
}

static rv9_io_err_t gpio_unit_close(rv9_dev_t *dev, void *state)
{
    (void)dev;
    gpio_unit_t *u = (gpio_unit_t *)state;
    if (u == NULL) return RV9_IO_OK;

    /* Disarm before freeing, in that order: an interrupt that arrives
       between the two would be handed a pointer to nothing. */
    if (u->event != NULL) {
        gpio_intr_disable((gpio_num_t)u->pin);
        gpio_isr_handler_remove((gpio_num_t)u->pin);
        rv9_event_destroy(u->event);
        u->event = NULL;
    }

    /* The level stays; the accounting does not. Closing the last path that
       wanted to drive the pin does not turn it back into an input either --
       see the note at the top about why a level survives its process. */
    rv9_lock_acquire(s_lock);
    if (u->pin < MAX_PINS) {
        if (s_open_count[u->pin] > 0)                 s_open_count[u->pin]--;
        if (u->output && s_output_count[u->pin] > 0)  s_output_count[u->pin]--;
    }
    rv9_lock_release(s_lock);

    rv9_free(u);
    return RV9_IO_OK;
}

/*
 * Arm or disarm the pin.
 *
 * Unlike a level, this does not survive the close -- see the note at the
 * top of this file about why a level does. An interrupt exists to wake a
 * particular process, so it has no meaning once that process is gone, and
 * leaving one armed would keep firing into a handler nobody reads.
 */
static rv9_io_err_t gpio_set_edge(gpio_unit_t *u, uint32_t mode)
{
    static const gpio_int_type_t types[] = {
        GPIO_INTR_DISABLE, GPIO_INTR_POSEDGE,
        GPIO_INTR_NEGEDGE, GPIO_INTR_ANYEDGE,
    };
    if (mode > 3) return RV9_IO_ERR_INVAL;

    if (mode == 0) {
        if (u->event == NULL) return RV9_IO_OK;
        gpio_intr_disable((gpio_num_t)u->pin);
        gpio_isr_handler_remove((gpio_num_t)u->pin);
        rv9_event_destroy(u->event);
        u->event = NULL;
        u->edge  = 0;
        return RV9_IO_OK;
    }

    if (!s_isr_service) {
        esp_err_t e = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return RV9_IO_ERR_IO;
        s_isr_service = true;
    }

    if (u->event == NULL) {
        if (rv9_event_create(&u->event) != RV9_OK) return RV9_IO_ERR_NOMEM;
        if (gpio_isr_handler_add((gpio_num_t)u->pin, gpio_edge_isr, u)
            != ESP_OK) {
            rv9_event_destroy(u->event);
            u->event = NULL;
            return RV9_IO_ERR_IO;
        }
    }

    if (gpio_set_intr_type((gpio_num_t)u->pin, types[mode]) != ESP_OK) {
        return RV9_IO_ERR_IO;
    }
    if (gpio_intr_enable((gpio_num_t)u->pin) != ESP_OK) return RV9_IO_ERR_IO;

    u->edge = mode;
    ESP_LOGI(TAG, "pin %lu armed (%s), event %d", (unsigned long)u->pin,
             mode == 1 ? "rising" : mode == 2 ? "falling" : "both",
             rv9_event_id(u->event));
    return RV9_IO_OK;
}

/*
 * Resident, and so are the two ESP-IDF calls they make -- but only because
 * we asked. gpio_set_level and gpio_get_level are mapped 'noflash' when
 * CONFIG_GPIO_CTRL_FUNC_IN_IRAM is set, and it is off by default; this file
 * claimed they were resident before the option was in sdkconfig.defaults,
 * which made the claim aspirational. Marking our own code IRAM and then
 * calling into flash on the last instruction would have been a thorough way
 * to achieve nothing.
 *
 * A pin is therefore the one device a control loop can drive while the
 * flash cache is off. PWM and the ADC are not: their drivers take mutexes
 * and live in flash.
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

    case RV9_PIO_SS_EDGE:
        if (!set) { *value = u->edge; return RV9_IO_OK; }
        return gpio_set_edge(u, *value);

    case RV9_PIO_GS_EVENT:
        if (set) return RV9_IO_ERR_UNSUPPORTED;
        *value = (uint32_t)rv9_event_id(u->event);
        return RV9_IO_OK;

    default:
        return RV9_IO_ERR_UNSUPPORTED;
    }
}

static const rv9_driver_t gpio_drv = {
    .name       = "gpio",
    /* A pin keeps its level after the last path closes -- see the comment
       on gpio_unit_open. That is what makes a failsafe on a pin outlive
       the program that declared it. */
    .retains    = true,
    .unit_open  = gpio_unit_open,
    .unit_close = gpio_unit_close,
    .unit_read  = gpio_unit_read,
    .unit_write = gpio_unit_write,
    .unit_stat  = gpio_unit_stat,
};

rv9_io_err_t rv9_drv_gpio_register(void)
{
    if (s_lock == NULL && rv9_lock_create(&s_lock) != RV9_OK) {
        return RV9_IO_ERR_NOMEM;
    }
    return rv9_io_register_driver(&gpio_drv);
}
