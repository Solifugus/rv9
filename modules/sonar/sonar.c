/*
 * sonar -- a real-time ranging loop, and what it does about getting close.
 *
 *     rt sonar             the sensor is on pin 11
 *
 * THE PIN IS NOT AN ARGUMENT, AND CANNOT BE
 *
 * It was one, briefly, and that was wrong in a way worth recording. This
 * module *declares* /gpio/11 -- exclusively, and as its failsafe -- in its
 * manifest, and RV-9 claims both at fork, before a line of this code runs.
 * A pin chosen from argv would mean the declaration named one pin while the
 * work happened on another: the claim would protect the wrong wire and the
 * failsafe would park the wrong wire.
 *
 * So a declared device is a property of the program, not of the invocation.
 * That is not a limitation of this module; it is what makes admission mean
 * anything. `range` takes a pin because it declares nothing and promises
 * nothing.
 *
 * This is the shape RV-9 was built for: a loop with a declared period and
 * deadline, a real input from the world, a published reading anything may
 * watch, and a declared failsafe applied by the system rather than by this
 * code. `range` is the same sensor read by hand; this is the same sensor
 * read on a contract.
 *
 * THE TRICK, WHICH IS THE WHOLE POINT
 *
 * A sonic ranger takes up to 30 milliseconds to answer, and a real-time
 * loop must not spend 30 milliseconds waiting. Both are true, and the
 * resolution is not to compromise between them:
 *
 *     trigger at the top of this activation
 *     read the answer to the *previous* activation's trigger
 *
 * The echo arrives about 100 milliseconds before anybody asks about it, and
 * the interrupt handler has already timed it (RV9_PIO_GS_PULSE_US). So this
 * loop never waits for the sensor at all. Its execution time is two
 * getstats, a publish and a pin wiggle -- microseconds -- for a device whose
 * physical response is four orders of magnitude slower.
 *
 * What it costs is that every reading is one period old. For a 100 ms loop
 * watching something approach, that is the honest price and it is stated in
 * the publication: `stamp_us` is when the echo was *timed*, not when it was
 * handed over, so a watcher asking how stale this is gets a true answer.
 *
 * WHAT IT DOES ABOUT GETTING CLOSE
 *
 * Below STOP_MM it drives the parked pin low and keeps going -- a control
 * loop that stops controlling because it saw something it did not like is
 * not safe, it is absent. It stops for one reason only: the sensor going
 * silent. A ranger that answers nothing for MISSES_FATAL periods in a row
 * is a ranger that has been unplugged, and continuing to steer on a reading
 * from three seconds ago is worse than admitting the loop is blind. So it
 * returns, and RV-9 applies the declared failsafe on the way out -- not
 * this code, which by then may be the thing that failed.
 */
#include "modlib.h"

/* Declared in build.conf as well, as an exclusive and as the failsafe.
   The two must agree; see the note above. */
#define SENSOR_PIN      11

/* Ignore anything shorter: it is our own trigger coming back, timed by the
   same handler. Two centimetres is 116 us of round trip, so a floor here
   discards every trigger and no real echo. See range, which found this by
   confidently reporting 28 us as zero millimetres. */
#define MIN_ECHO_US     60

/* Microseconds of echo per ten millimetres: the round trip at 343 m/s. */
#define US_PER_MM_X10   583u

/* Datasheets ask for at least 10 us of trigger. */
#define TRIGGER_US      12

/* Closer than this and the parked pin goes low. 300 mm is far enough to
   stop something moving slowly and near enough to be easy to test with a
   hand. */
#define STOP_MM         300

/*
 * Silent periods before the loop gives up.
 *
 * Five at 100 ms is half a second, which is longer than any single missed
 * echo (a soft or angled surface swallows one now and then) and shorter
 * than anything moving can travel while unobserved.
 */
#define MISSES_FATAL    5

#define RUN_ACTIVATIONS 600         /* a minute at 100 ms */

/* What this component exposes, published as one object. */
typedef struct {
    rv9_pub_t head;
    int32_t   mm;          /* distance, or -1 when the last echo was missed */
    int32_t   echo_us;     /* what was actually measured */
    int32_t   misses;      /* consecutive silences, so a watcher sees it coming */
    int32_t   stopped;     /* 1 while the pin is held low for being too close */
} sonar_pub_t;

typedef struct {
    char        path[24];
    uint32_t    pulses;    /* the sequence number last seen */
    uint32_t    misses;
    uint32_t    readings;
    uint32_t    closest_mm;
    sonar_pub_t out;
} sonar_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 14)      return 1;
    if (env->rt_declare == NULL || env->rt_wait == NULL) {
        m_say(env, RV9_STDOUT, "sonar: not a real-time process\n"
                               "  run it with: rt sonar [pin]\n");
        return 2;
    }

    sonar_statics_t *st = (sonar_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        m_say(env, RV9_STDERR, "sonar: static_size in build.conf is too small\n");
        return 3;
    }

    m_devpath(st->path, "/gpio/", SENSOR_PIN);

    /*
     * Everything that allocates or blocks happens here, before the period
     * exists. Opening a path, arming an interrupt and the first write
     * through the I/O manager are all expensive exactly once; `control`
     * learned that the hard way, spending 60% of its budget on activation
     * one warming up a path it had opened but never used.
     */
    int p = env->open(st->path, RV9_MODE_RW);
    if (p < 0) {
        m_say(env, RV9_STDERR, "sonar: cannot open ");
        m_say(env, RV9_STDERR, st->path);
        m_say(env, RV9_STDERR, "\n");
        return 4;
    }

    uint32_t both = 3;
    if (env->setstat(p, RV9_PIO_SS_EDGE, &both) < 0) {
        m_say(env, RV9_STDERR, "sonar: that pin cannot report edges\n");
        env->close(p);
        return 5;
    }

    if (env->getstat(p, RV9_PIO_GS_PULSES, &st->pulses) < 0) {
        m_say(env, RV9_STDERR, "sonar: that pin does not time pulses\n");
        uint32_t off = 0;
        env->setstat(p, RV9_PIO_SS_EDGE, &off);
        env->close(p);
        return 6;
    }

    int pub = env->open("/pub0/DISTANCE", RV9_MODE_WRITE);
    if (pub < 0) {
        m_say(env, RV9_STDOUT, "sonar: cannot publish (running blind)\n");
    } else {
        /* Once, for the path rather than for the value. */
        st->out.head.len = 4 * sizeof(int32_t);
        st->out.mm       = -1;
        env->write(pub, &st->out, sizeof(st->out));
    }

    /* And the pin, once, for the same reason -- and because a loop whose
       failsafe is "drive it low" should start from there. */
    uint32_t dir = 1, zero = 0;
    env->setstat(p, RV9_PIO_SS_DIRECTION, &dir);
    env->write(p, &zero, sizeof(zero));

    st->closest_mm = 0xFFFFFFFFu;

    if (env->rt_declare(0) < 0) {
        m_say(env, RV9_STDOUT, "sonar: could not declare a period\n");
        if (pub >= 0) env->close(pub);
        env->close(p);
        return 7;
    }

    int blind = 0;

    for (uint32_t n = 0; n < RUN_ACTIVATIONS; n++) {
        /*
         * --- read the answer to last time's question ---
         *
         * The sequence number, not the width: a width is always there once
         * one echo has arrived, so testing the width would report the same
         * distance forever the moment the sensor was unplugged. The count
         * is the only thing that distinguishes a new answer from the old
         * one, which is precisely the failure this loop must notice.
         */
        uint32_t seen = st->pulses, us = 0;
        int32_t  mm   = -1;

        env->getstat(p, RV9_PIO_GS_PULSES, &seen);
        if (seen != st->pulses) {
            st->pulses = seen;
            env->getstat(p, RV9_PIO_GS_PULSE_US, &us);
            if (us >= MIN_ECHO_US) {
                mm = (int32_t)((us * 10u) / US_PER_MM_X10);
                st->misses = 0;
                st->readings++;
                if ((uint32_t)mm < st->closest_mm) st->closest_mm = (uint32_t)mm;
            }
        }
        if (mm < 0) st->misses++;

        /*
         * --- act ---
         *
         * Too close: hold the pin low and keep running. A loop that exits
         * because it saw something it did not like has not made anything
         * safe; it has removed the only thing that was watching.
         */
        bool stop = (mm >= 0 && mm < STOP_MM);
        uint32_t dirn = 1;
        env->setstat(p, RV9_PIO_SS_DIRECTION, &dirn);
        uint32_t level = 0;
        env->write(p, &level, sizeof(level));

        /*
         * --- trigger for next time, and let go of the wire ---
         *
         * The pin must genuinely stop driving: the sensor answers on the
         * same wire, and a pin still held low is a wire it cannot raise.
         */
        uint32_t one = 1;
        env->write(p, &one, sizeof(one));
        uint64_t t0 = env->time_us();
        while (env->time_us() - t0 < TRIGGER_US) { }
        env->write(p, &level, sizeof(level));

        uint32_t in = 0;
        env->setstat(p, RV9_PIO_SS_DIRECTION, &in);

        /* --- publish, as one indivisible set --- */
        if (pub >= 0) {
            st->out.mm      = mm;
            st->out.echo_us = (int32_t)us;
            st->out.misses  = (int32_t)st->misses;
            st->out.stopped = stop ? 1 : 0;
            st->out.head.len      = 4 * sizeof(int32_t);
            st->out.head.stamp_us = t0;
            env->write(pub, &st->out, sizeof(st->out));
        }

        if (st->misses >= MISSES_FATAL) { blind = 1; break; }

        int late = env->rt_wait();
        if (late < 0) break;
    }

    if (pub >= 0) env->close(pub);
    env->close(p);

    rv9_rt_report_t r;
    if (env->rt_stats(&r) < 0) {
        m_say(env, RV9_STDOUT, "sonar: no timing available\n");
        return 8;
    }

    m_say(env, RV9_STDOUT, "sonar: ");
    m_num(env, RV9_STDOUT, (int32_t)r.activations);
    m_say(env, RV9_STDOUT, " activations at ");
    m_num(env, RV9_STDOUT, (int32_t)r.period_us);
    m_say(env, RV9_STDOUT, " us\n  readings       ");
    m_num(env, RV9_STDOUT, (int32_t)st->readings);
    m_say(env, RV9_STDOUT, "\n  closest        ");
    m_num(env, RV9_STDOUT, st->closest_mm == 0xFFFFFFFFu
                           ? -1 : (int32_t)st->closest_mm);
    m_say(env, RV9_STDOUT, " mm\n  worst jitter   ");
    m_num(env, RV9_STDOUT, (int32_t)r.max_jitter_us);
    m_say(env, RV9_STDOUT, " us\n  worst execute  ");
    m_num(env, RV9_STDOUT, (int32_t)r.max_exec_us);
    m_say(env, RV9_STDOUT, " us\n  overruns       ");
    m_num(env, RV9_STDOUT, (int32_t)r.overruns);
    m_say(env, RV9_STDOUT, "\n");

    if (blind) {
        m_say(env, RV9_STDOUT, "  the sensor stopped answering; failsafe applied\n");
        return 9;
    }
    return (int)r.overruns;
}
