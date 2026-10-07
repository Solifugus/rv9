/*
 * touch -- a capacitive touch controller, as a stream of events.
 *
 * Reading /touch gives whole rv9_touch_event_t records and blocks until
 * there is one -- SCF does the waiting, the way it does for a terminal
 * with nobody typing -- so a widget loop is a read in a loop. The
 * reasoning for
 * events rather than a polled cell is in rv9/module.h beside the struct;
 * the short of it is that state falls out of events and taps do not fall
 * out of state.
 *
 * THE CHIP: a GT911, five points, sixteen-bit register addresses.
 *
 * Its interrupt line is not connected on this board, so this polls. 60 Hz
 * is a compromise with nothing clever about it: a finger does not move far
 * in 16 ms, and the cost is one short transaction per tick on a bus that
 * is otherwise idle. Were the line wired, this file would change in one
 * place -- the ticker -- and nothing above it would notice.
 *
 * THE BUS IS NOT THIS DRIVER'S. The controller shares the two wires that
 * /i2c0 serves, and which of them attaches first is decided by the order
 * descriptors happen to be in, so neither creates it: see i2c_bus.h, which
 * is the same arrangement panel.h makes for the display and the card on
 * the C5.
 *
 * THE GLASS IS FITTED UPSIDE DOWN on the P4 and the panel turns the
 * picture round in hardware. The controller reports in the glass's own
 * frame, so it needs the same turn or a finger lands where nothing is
 * drawn. That fact is asked of the panel -- rv9_panel_mounting() -- rather
 * than written here, because two places that must agree are two places to
 * disagree.
 */
#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"

#include "i2c_bus.h"
#include "panel.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "rv9-touch";

/* Descriptor options:
 *   opt[0]  poll interval in ms (default 16, about 60 Hz)
 *   opt[1]  queue depth in events (default 32) */
#define OPT_MS     0
#define OPT_DEPTH  1

#define DEFAULT_MS     16
#define DEFAULT_DEPTH  32
#define MAX_DEPTH      256

#define GT911_ADDR         0x5D      /* and 0x14 on boards strapped the other way */
#define GT911_ADDR_ALT     0x14

#define REG_PRODUCT_ID     0x8140    /* four ASCII bytes: "911" and a NUL */
#define REG_STATUS         0x814E

/*
 * 0x814F, which is REG_STATUS + 1 and not 0x8150.
 *
 * Getting this one byte wrong is the whole of a day's confusion: every
 * field shifts, so x's high byte is read as its low byte, the track id is
 * read as part of a coordinate, and the id therefore changes as the finger
 * moves -- which makes every poll look like a new finger going down. The
 * coordinates came out past the edge of the panel and the clamp below
 * turned them into a tidy 1023,599, so the fault presented as "every touch
 * is in the bottom-right corner" rather than as nonsense.
 *
 * Relative to this address each point is eight bytes: track id, x low, x
 * high, y low, y high, strength low, strength high, reserved.
 */
#define REG_POINT0         0x814F

#define STATUS_READY       0x80
#define STATUS_COUNT_MASK  0x0F

#define GT911_MAX_POINTS   5
#define POINT_BYTES        8

/* One finger as the controller last reported it, so DOWN, MOVE and UP can
   be told apart by comparing one poll against the one before. */
typedef struct {
    bool     down;
    uint16_t x, y;
    uint16_t pressure;
} finger_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t chip;

    int      w, h;              /* the panel's, for mirroring */
    bool     mirror_x, mirror_y, swap_xy;

    uint32_t ms;
    rv9_task_t ticker;

    finger_t last[GT911_MAX_POINTS];
    bool     said_outside;      /* complained once; see place() */

    /* The queue. Head is where the next event goes, tail the oldest
       unread. One slot stays empty so full and empty differ. */
    rv9_touch_event_t *q;
    uint32_t           depth;
    uint32_t           head, tail;
    uint32_t           dropped;
    rv9_lock_t         lock;
} touch_t;

/* ---- the chip ---- */

static rv9_io_err_t reg_read(touch_t *t, uint16_t reg, uint8_t *buf, size_t n)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };

    /* Write the register then read without letting go of the bus. The
       repeated start is the whole point: let go and the chip forgets which
       register was asked for. */
    if (i2c_master_transmit_receive(t->chip, addr, sizeof(addr), buf, n,
                                    50) != ESP_OK) {
        return RV9_IO_ERR_IO;
    }
    return RV9_IO_OK;
}

static rv9_io_err_t reg_write8(touch_t *t, uint16_t reg, uint8_t v)
{
    uint8_t b[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), v };

    if (i2c_master_transmit(t->chip, b, sizeof(b), 50) != ESP_OK) {
        return RV9_IO_ERR_IO;
    }
    return RV9_IO_OK;
}

/* ---- the queue ---- */

static uint32_t q_used(const touch_t *t)
{
    return (t->head + t->depth - t->tail) % t->depth;
}

/*
 * Put an event in, giving up a MOVE rather than a DOWN or an UP.
 *
 * A reader that has stopped reading must not stall the ticker, so
 * something has to go when the queue is full. Which one matters: a stale
 * position is corrected by the next MOVE, but a lost UP leaves whatever
 * was being pressed held down by a finger that is no longer on the glass,
 * and nothing later ever contradicts it.
 *
 * So a MOVE arriving at a full queue is dropped, and a DOWN or an UP makes
 * room by dropping the oldest MOVE it can find. Only if there is no MOVE
 * to drop -- a queue of nothing but presses and releases, which means a
 * reader that has been gone a long time -- does the oldest event go.
 */
static void q_put(touch_t *t, const rv9_touch_event_t *e)
{
    rv9_lock_acquire(t->lock);

    if (q_used(t) == t->depth - 1) {
        if (e->kind == RV9_TOUCH_MOVE) {
            t->dropped++;
            rv9_lock_release(t->lock);
            return;
        }

        uint32_t victim = t->depth;
        for (uint32_t i = t->tail; i != t->head; i = (i + 1) % t->depth) {
            if (t->q[i].kind == RV9_TOUCH_MOVE) { victim = i; break; }
        }
        if (victim == t->depth) victim = t->tail;

        /* Close the gap by shuffling the older side forward, so order is
           kept. The queue is small and this happens only when a reader has
           stopped; the alternative is a hole to skip on every read. */
        for (uint32_t i = victim; i != t->tail;) {
            uint32_t prev = (i + t->depth - 1) % t->depth;
            t->q[i] = t->q[prev];
            i = prev;
        }
        t->tail = (t->tail + 1) % t->depth;
        t->dropped++;
    }

    t->q[t->head] = *e;
    t->head = (t->head + 1) % t->depth;

    rv9_lock_release(t->lock);
}

/* ---- polling ---- */

static void emit(touch_t *t, uint8_t kind, uint8_t id, const finger_t *f)
{
    rv9_touch_event_t e = {
        .kind     = kind,
        .id       = id,
        .x        = f->x,
        .y        = f->y,
        .pressure = f->pressure,
        .at_ms    = (uint32_t)rv9_time_ms(),
    };
    q_put(t, &e);
}

static void place(touch_t *t, finger_t *f, uint16_t rx, uint16_t ry)
{
    uint16_t x = rx, y = ry;

    if (t->swap_xy) { uint16_t s = x; x = y; y = s; }
    if (t->mirror_x && t->w > 0) x = (uint16_t)(t->w - 1 - x);
    if (t->mirror_y && t->h > 0) y = (uint16_t)(t->h - 1 - y);

    /*
     * The controller occasionally reports a point just outside the active
     * area, and clamping is kinder than letting a widget index off its
     * edge. But it is said out loud the first time, because a clamp is
     * also how a decoding error disguises itself: with the point register
     * off by one, every reading ran past the edge and arrived here as a
     * neat 1023,599, which looks like a corner rather than like rubbish.
     * A fault that produces plausible values is the expensive kind.
     */
    bool outside = (t->w > 0 && x >= (uint16_t)t->w) ||
                   (t->h > 0 && y >= (uint16_t)t->h);

    if (t->w > 0 && x >= (uint16_t)t->w) x = (uint16_t)(t->w - 1);
    if (t->h > 0 && y >= (uint16_t)t->h) y = (uint16_t)(t->h - 1);

    if (outside && !t->said_outside) {
        t->said_outside = true;
        ESP_LOGW(TAG, "a point arrived outside %dx%d (raw %u,%u) and was "
                      "clamped; if every touch looks like a corner, suspect "
                      "the decoding rather than the glass",
                 t->w, t->h, (unsigned)rx, (unsigned)ry);
    }

    f->x = x;
    f->y = y;
}

static void poll_once(touch_t *t)
{
    uint8_t status = 0;
    if (reg_read(t, REG_STATUS, &status, 1) != RV9_IO_OK) return;
    if (!(status & STATUS_READY)) return;

    uint32_t n = status & STATUS_COUNT_MASK;
    if (n > GT911_MAX_POINTS) n = GT911_MAX_POINTS;

    uint8_t raw[GT911_MAX_POINTS * POINT_BYTES];
    if (n > 0 && reg_read(t, REG_POINT0, raw, n * POINT_BYTES) != RV9_IO_OK) {
        /* Tell the chip we are done anyway, or it never reports again. */
        (void)reg_write8(t, REG_STATUS, 0);
        return;
    }

    /* Acknowledged as early as possible: until this is written the chip
       holds its buffer and the next touch is not reported at all. */
    (void)reg_write8(t, REG_STATUS, 0);

    finger_t now[GT911_MAX_POINTS];
    memset(now, 0, sizeof(now));

    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *p = raw + i * POINT_BYTES;
        uint8_t id = p[0] & 0x0F;
        if (id >= GT911_MAX_POINTS) continue;

        now[id].down     = true;
        now[id].pressure = (uint16_t)(p[5] | (p[6] << 8));
        place(t, &now[id], (uint16_t)(p[1] | (p[2] << 8)),
                           (uint16_t)(p[3] | (p[4] << 8)));
    }

    for (uint8_t id = 0; id < GT911_MAX_POINTS; id++) {
        finger_t *was = &t->last[id];
        finger_t *is  = &now[id];

        if (is->down && !was->down) {
            emit(t, RV9_TOUCH_DOWN, id, is);
        } else if (is->down && was->down) {
            /* Only when it actually moved: a finger resting still would
               otherwise produce sixty identical events a second. */
            if (is->x != was->x || is->y != was->y) {
                emit(t, RV9_TOUCH_MOVE, id, is);
            }
        } else if (!is->down && was->down) {
            /* The release carries the last place it was, which is what a
               widget needs to decide whether the finger left inside it. */
            is->x = was->x;
            is->y = was->y;
            emit(t, RV9_TOUCH_UP, id, is);
        }

        *was = *is;
    }
}

static void ticker(void *arg)
{
    touch_t *t = (touch_t *)arg;

    for (;;) {
        poll_once(t);
        rv9_task_delay_ms(t->ms);
    }
}

/* ---- the driver ---- */

static rv9_io_err_t touch_init(rv9_dev_t *dev)
{
    touch_t *t = rv9_calloc(1, sizeof(*t));
    if (t == NULL) return RV9_IO_ERR_NOMEM;

    t->ms    = dev->opt[OPT_MS]    ? dev->opt[OPT_MS]    : DEFAULT_MS;
    t->depth = dev->opt[OPT_DEPTH] ? dev->opt[OPT_DEPTH] : DEFAULT_DEPTH;
    if (t->depth < 4)         t->depth = 4;
    if (t->depth > MAX_DEPTH) t->depth = MAX_DEPTH;

    rv9_io_err_t err = rv9_i2c_bus_claim(0, &t->bus);
    if (err != RV9_IO_OK) { rv9_free(t); return err; }

    /* Which address this board straps the chip to is not knowable without
       asking, so ask. Neither answering is a board with no touch panel,
       which is not a failure -- the device simply does not attach. */
    uint32_t addr = GT911_ADDR;
    if (i2c_master_probe(t->bus, GT911_ADDR, 50) != ESP_OK) {
        if (i2c_master_probe(t->bus, GT911_ADDR_ALT, 50) != ESP_OK) {
            ESP_LOGI(TAG, "%s: no touch controller on the bus", dev->name);
            rv9_free(t);
            return RV9_IO_ERR_NOTFOUND;
        }
        addr = GT911_ADDR_ALT;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 100 * 1000,
    };
    if (i2c_master_bus_add_device(t->bus, &cfg, &t->chip) != ESP_OK) {
        ESP_LOGE(TAG, "%s: could not address 0x%02lx", dev->name,
                 (unsigned long)addr);
        rv9_free(t);
        return RV9_IO_ERR_IO;
    }

    /* "911" in ASCII, which is the chip saying it is the chip. Probing an
       address only proves something is there. */
    uint8_t id[4] = { 0 };
    if (reg_read(t, REG_PRODUCT_ID, id, sizeof(id)) != RV9_IO_OK) {
        ESP_LOGE(TAG, "%s: 0x%02lx will not answer", dev->name,
                 (unsigned long)addr);
        rv9_free(t);
        return RV9_IO_ERR_IO;
    }

    /* The glass this reports against, and how it is fitted. */
    rv9_panel_size(&t->w, &t->h);
    rv9_panel_mounting(&t->mirror_x, &t->mirror_y, &t->swap_xy);

    t->q = rv9_calloc(t->depth, sizeof(*t->q));
    if (t->q == NULL || rv9_lock_create(&t->lock) != RV9_OK) {
        rv9_free(t->q);
        rv9_free(t);
        return RV9_IO_ERR_NOMEM;
    }

    dev->drv_state = t;

    if (rv9_task_create(ticker, "rv9-touch", 3072, t, RV9_PRIO_HIGH,
                        &t->ticker) != RV9_OK) {
        ESP_LOGE(TAG, "%s: no task to poll with", dev->name);
        dev->drv_state = NULL;
        rv9_free(t->q);
        rv9_free(t);
        return RV9_IO_ERR_NOMEM;
    }

    ESP_LOGI(TAG, "%s: GT911 '%c%c%c' at 0x%02lx, %dx%d, every %lu ms%s",
             dev->name, id[0] ? id[0] : '?', id[1] ? id[1] : '?',
             id[2] ? id[2] : '?', (unsigned long)addr, t->w, t->h,
             (unsigned long)t->ms,
             (t->mirror_x || t->mirror_y) ? ", mirrored to the panel" : "");
    return RV9_IO_OK;
}

/*
 * Whole events, oldest first, and never half of one.
 *
 * A reader asking for less than one event gets RV9_IO_ERR_INVAL rather
 * than a fragment: a caller that has to reassemble records across reads is
 * a caller that will one day get it wrong, and there is no reason to make
 * that possible when the record size is fixed and published.
 */
static rv9_io_err_t touch_read(rv9_dev_t *dev, void *buf, size_t len,
                               size_t *done)
{
    touch_t *t = (touch_t *)dev->drv_state;
    if (t == NULL || buf == NULL) return RV9_IO_ERR_IO;
    if (len < sizeof(rv9_touch_event_t)) return RV9_IO_ERR_INVAL;

    uint32_t want = (uint32_t)(len / sizeof(rv9_touch_event_t));
    uint8_t *out  = (uint8_t *)buf;
    uint32_t n    = 0;

    rv9_lock_acquire(t->lock);
    while (n < want && q_used(t) > 0) {
        memcpy(out + n * sizeof(rv9_touch_event_t), &t->q[t->tail],
               sizeof(rv9_touch_event_t));
        t->tail = (t->tail + 1) % t->depth;
        n++;
    }
    uint32_t lost = t->dropped;
    t->dropped = 0;
    rv9_lock_release(t->lock);

    if (lost > 0) {
        ESP_LOGW(TAG, "%s: %lu event(s) dropped; nobody was reading",
                 dev->name, (unsigned long)lost);
    }

    if (n == 0) {
        if (done) *done = 0;
        return RV9_IO_ERR_WOULDBLOCK;   /* SCF sleeps and asks again */
    }

    if (done) *done = n * sizeof(rv9_touch_event_t);
    return RV9_IO_OK;
}

static rv9_io_err_t touch_getstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    touch_t *t = (touch_t *)dev->drv_state;
    if (t == NULL) return RV9_IO_ERR_IO;

    /* The glass it reports against, so a program can scale without having
       to open /term or /w0 just to ask. */
    if (code == RV9_GS_PHYSICAL) {
        if (arg == NULL) return RV9_IO_ERR_INVAL;
        /* Through locals: rv9_physical_t is packed, so taking the address
           of a member of it is an unaligned pointer the compiler is right
           to object to. */
        uint32_t w_um = 0, h_um = 0;
        uint8_t  kind = 0;
        rv9_panel_physical(&w_um, &h_um, &kind);

        rv9_physical_t *p = (rv9_physical_t *)arg;
        memset(p, 0, sizeof(*p));
        p->width_px  = (uint32_t)t->w;
        p->height_px = (uint32_t)t->h;
        p->width_um  = w_um;
        p->height_um = h_um;
        p->kind      = kind;
        return RV9_IO_OK;
    }

    return RV9_IO_ERR_UNSUPPORTED;
}

static const rv9_driver_t touch = {
    .name       = "touch",
    .raw_stream = true,       /* records, not lines: see rv9/io.h */
    .init       = touch_init,
    .read       = touch_read,
    .getstat    = touch_getstat,
};

rv9_io_err_t rv9_drv_touch_register(void)
{
    return rv9_io_register_driver(&touch);
}
