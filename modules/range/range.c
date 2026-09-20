/*
 * range -- how far away is it, with a three-pin sonic ranger.
 *
 *     range 11            one reading from the sensor on pin 11
 *     range 11 5          five readings
 *     range 11 5 listen   do not trigger; just measure pulses arriving
 *
 * WHAT THE SENSOR IS
 *
 * The three-pin ultrasonic modules -- SIG, VCC, GND, with a transmitter and
 * a receiver can side by side -- are an HC-SR04 with the trigger and echo
 * lines merged onto one wire. The exchange is:
 *
 *     drive SIG high for >=10 us, then let go
 *     the module emits a burst and raises SIG
 *     SIG falls when the echo returns, or after ~30 ms if it does not
 *
 * The measurement is the width of that high pulse. Sound travels about
 * 343 m/s, the pulse covers the round trip, so one centimetre is 58.3 us
 * of pulse. Nothing else about the sensor matters.
 *
 * WHY THIS IS AN ORDINARY PROGRAM
 *
 * It would be reasonable to expect a microsecond measurement to need the
 * real-time class. It does not, and the reason is worth understanding
 * because it applies to every sensor of this shape: the pulse is timed by
 * the interrupt handler (RV9_PIO_GS_PULSE_US), not by this code. Both edges
 * are stamped before anything is woken, so the width is already correct by
 * the time this program is scheduled -- and how late that is affects only
 * how promptly the answer is *collected*, not how good it is.
 *
 * So this polls. A shell command that spun a control loop to read a
 * distance would be claiming the machine for a measurement it is not making.
 *
 * `listen` skips the trigger, which turns this into a general pulse-width
 * meter: a servo or RC receiver frame, a tachometer's mark, anything whose
 * value is a duration. The distance column is meaningless then and is
 * printed anyway, because the caller knows what it wired up.
 *
 * WIRING, AND A WARNING
 *
 * These modules are usually 5 V parts and drive 5 V on SIG. An ESP32-C5 pin
 * is 3.3 V. Try the sensor on 3.3 V first -- most of the clones work, with
 * less maximum range -- and only reach for 5 V with a series resistor and a
 * clamp, never a plain divider: SIG is bidirectional, and a divider that
 * makes the echo safe also drags the trigger below the module's threshold.
 */
#include "modlib.h"

/* Microseconds of pulse per centimetre of distance: the round trip at
   343 m/s. Kept as tenths of a micrometre-free integer -- 583 per 10 cm --
   so the arithmetic stays in 32 bits and millimetres come out directly. */
#define US_PER_MM_X10   583u

/* The module gives up and drops SIG after about 30 ms when nothing comes
   back. Waiting appreciably longer than that is waiting for nothing. */
#define ECHO_TIMEOUT_MS 40

/* Datasheets ask for 10 us; the threshold is a minimum and a little over is
   harmless, so this spins until the clock says at least this much has
   passed rather than trying to be exact. */
#define TRIGGER_US      12

/* The sensor must not be re-triggered until the previous burst's echoes
   have died away, or the next reading is the last one's reflection off the
   far wall. The datasheets say 60 ms; this is that with room to spare. */
#define SETTLE_MS       70

/*
 * Shorter than this is not an echo.
 *
 * Found by running this against a bare pin: it reported 28 us and then
 * 20 us, confidently, as zero millimetres. Those were RV-9's own trigger.
 * The trigger is a rise and a fall on the same wire the echo comes back on,
 * so the handler times it like anything else and it is the *first* pulse
 * every reading -- which meant the answer was always the trigger and never
 * the sensor.
 *
 * Two centimetres is about as close as these modules resolve, and two
 * centimetres of round trip is 116 us. A 12 us trigger and a 116 us echo
 * are not near each other, so a floor between them costs no real reading
 * and discards every trigger. Held at 60 rather than 100 because the pin
 * sees the trigger somewhat stretched -- 28 us for a 12 us pulse, the cost
 * of two writes through the I/O manager -- and the gap is wide enough to
 * pay for that and still not reach a genuine echo.
 */
#define MIN_ECHO_US     60

typedef struct {
    char path[24];
} range_t;

static void say_us(const rv9_mod_env_t *env, uint32_t us)
{
    /* Millimetres from microseconds: us * 10 / 583. The multiply is done
       first and in 32 bits, which is safe because a 40 ms pulse is 40000
       and the product is well inside range. */
    uint32_t mm = (us * 10u) / US_PER_MM_X10;

    m_num(env, RV9_STDOUT, (int32_t)mm);
    m_say(env, RV9_STDOUT, " mm  (");
    m_num(env, RV9_STDOUT, (int32_t)us);
    m_say(env, RV9_STDOUT, " us)\n");
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 11) return 1;   /* time_us */

    range_t *st = (range_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return 2;

    if (env->arg == NULL || env->arg[0] == '\0') {
        m_say(env, RV9_STDOUT, "usage: range <pin> [count] [listen]\n");
        return 3;
    }

    const char *s   = env->arg;
    uint32_t    pin = m_num_parse(s, &s);
    while (*s == ' ') s++;
    uint32_t count = m_num_parse(s, &s);
    while (*s == ' ') s++;
    bool listen = (*s != '\0');

    if (count == 0) count = 1;

    m_devpath(st->path, "/gpio/", pin);

    int p = env->open(st->path, RV9_MODE_RW);
    if (p < 0) {
        m_say(env, RV9_STDERR, "range: cannot open ");
        m_say(env, RV9_STDERR, st->path);
        m_say(env, RV9_STDERR, "\n");
        return 4;
    }

    /*
     * Arm for both edges before anything else. The handler is what times
     * the pulse, so a trigger sent before the pin is armed is a burst whose
     * echo nobody measured.
     */
    uint32_t both = 3;
    if (env->setstat(p, RV9_PIO_SS_EDGE, &both) < 0) {
        m_say(env, RV9_STDERR, "range: this pin cannot report edges\n");
        env->close(p);
        return 5;
    }

    uint32_t seen = 0;
    if (env->getstat(p, RV9_PIO_GS_PULSES, &seen) < 0) {
        m_say(env, RV9_STDERR, "range: this pin does not time pulses; the "
                               "firmware is older than the command\n");
        uint32_t off = 0;
        env->setstat(p, RV9_PIO_SS_EDGE, &off);
        env->close(p);
        return 6;
    }

    int timeouts = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (!listen) {
            /*
             * Trigger, then get out of the way.
             *
             * The pin goes to output for as long as the pulse lasts and
             * straight back to input, because the module answers on the
             * same wire: leaving it an output would have RV-9 and the
             * sensor both driving SIG, and the edge that matters would
             * never be seen.
             */
            uint32_t out = 1, in = 0, v = 1;
            env->setstat(p, RV9_PIO_SS_DIRECTION, &out);

            v = 0; env->write(p, &v, sizeof(v));
            v = 1; env->write(p, &v, sizeof(v));

            uint64_t t0 = env->time_us();
            while (env->time_us() - t0 < TRIGGER_US) { }

            v = 0; env->write(p, &v, sizeof(v));
            env->setstat(p, RV9_PIO_SS_DIRECTION, &in);
        }

        /*
         * Wait for the count to move rather than for a width to appear.
         *
         * A width is always there after the first reading, so testing the
         * width would report the previous distance forever once the sensor
         * was unplugged. The sequence number is the only thing that
         * distinguishes "here is a new answer" from "here is the old one".
         */
        uint32_t deadline = ECHO_TIMEOUT_MS;
        uint32_t now      = seen;
        uint32_t us       = 0;
        bool     got      = false;

        while (deadline > 0) {
            if (env->getstat(p, RV9_PIO_GS_PULSES, &now) < 0) break;
            if (now != seen) {
                seen = now;
                env->getstat(p, RV9_PIO_GS_PULSE_US, &us);
                /* Our own trigger, timed by the same handler. Keep waiting
                   for the sensor's answer; see MIN_ECHO_US. */
                if (us >= MIN_ECHO_US) { got = true; break; }
            }
            env->sleep_ms(1);
            deadline--;
        }

        if (!got) {
            m_say(env, RV9_STDOUT, "no echo\n");
            timeouts++;
        } else {
            say_us(env, us);
        }

        if (!listen && i + 1 < count) env->sleep_ms(SETTLE_MS);
    }

    /*
     * Disarm on the way out. The handler and its event belong to this path
     * and go with it, but saying so explicitly keeps the pin's state the
     * business of whoever set it rather than of whoever closed it last.
     */
    uint32_t off = 0;
    env->setstat(p, RV9_PIO_SS_EDGE, &off);
    env->close(p);

    /* Every reading timed out: that is a wiring answer, not a distance. */
    if (timeouts == (int)count) return 7;
    return 0;
}
