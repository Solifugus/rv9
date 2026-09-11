/*
 * evlat -- how long after a pin changes does the process that cares run?
 *
 *     rt evlat               watches /gpio/3, 400 edges, makes its own
 *     rt evlat 0 3 400 2     pin 3, 400 edges, generator every 2 ms
 *     rt evlat 0 3 400 0     pin 3, 400 edges, someone else drives the pin
 *     rt evlat 0 3 400 2 5000  ... declaring a 5 ms bound a 2 ms generator
 *                                  will violate, to see it noticed
 *
 * (the leading 0 is the period argument rt passes through, which an
 * event-driven process has no use for)
 *
 * This is the reactive counterpart of iolat. iolat asks how long a control
 * loop waits to move a pin; this asks how long it waits to find out that
 * one moved. Both numbers matter and they are not the same number: the
 * first is a scheduling question, the second is an interrupt question with
 * a scheduling question inside it.
 *
 * HOW IT IS MEASURED
 *
 * The interrupt handler stamps the microsecond clock the moment the edge
 * arrives, before it wakes anyone. The process then compares that stamp
 * with the clock when it actually resumed. Nothing in between can flatter
 * the result -- in particular the process is genuinely blocked when the
 * edge happens, which is the case that costs, because it includes waking a
 * task and preempting whatever was running.
 *
 * That is also why the edges come from a separate, ordinary process:
 * generating them here would mean measuring an interrupt that arrived while
 * this task was already running, which is the easy half of the problem.
 *
 * The pin needs no wiring. It is opened INPUT_OUTPUT, so the level that
 * edgegen drives is fed back into the input and the pin interrupts itself.
 */
#include "modlib.h"

#define DEFAULT_PIN    3
#define DEFAULT_EDGES  400

typedef struct {
    uint32_t worst_us;      /* worst edge-to-process, measured here */
    uint32_t worst_at;
    uint32_t total_us;      /* 32-bit: a module has no libgcc, so a 64-bit
                               divide would not link */
    uint32_t coalesced;
} evlat_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL) return -1;
    if (env->abi_version < 12 || env->rt_declare_event == NULL) {
        m_say(env, RV9_STDOUT, "evlat: needs ABI 12\n");
        return -1;
    }

    evlat_statics_t *st = (evlat_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    /* rt passes its own argument through, so the first word is whatever
       period the shell gave it. We do not have a period; skip it. */
    const char *s = env->arg ? env->arg : "";
    (void)m_num_parse(s, &s);
    while (*s == ' ') s++;

    uint32_t pin = m_num_parse(s, &s);
    while (*s == ' ') s++;
    uint32_t edges = m_num_parse(s, &s);
    while (*s == ' ') s++;

    /*
     * An interval of 0 means "something else is driving this pin" -- do not
     * start a generator, just wait.
     *
     * This is not a convenience. The generator evlat starts is an ordinary
     * process, which means it stops when the whole machine stops: if a
     * flash erase freezes everything that lives in flash, no edges are
     * produced during the freeze and none are waiting at the far end of it.
     * That measures the interrupt path honestly and the stall not at all.
     * A source that keeps going -- a real sensor, or a real-time process in
     * IRAM -- is the one that shows what a stall costs a reactive system.
     */
    bool have_interval = (*s >= '0' && *s <= '9');
    uint32_t interval = have_interval ? m_num_parse(s, &s) : 2;
    while (*s == ' ') s++;

    /* The minimum inter-arrival to declare. Worth being able to set wrong
       on purpose: an instrument that can only be told the truth never shows
       what it does when it is lied to. */
    bool have_bound = (*s >= '0' && *s <= '9');
    uint32_t bound  = have_bound ? m_num_parse(s, &s)
                                 : (interval ? interval * 1000 / 2 : 0);

    if (pin == 0)   pin = DEFAULT_PIN;
    if (edges == 0) edges = DEFAULT_EDGES;

    char name[24];
    m_devpath(name, "/gpio/", pin);

    int p = env->open(name, RV9_MODE_RW);
    if (p < 0) {
        m_say(env, RV9_STDOUT, "evlat: cannot open ");
        m_say(env, RV9_STDOUT, name);
        m_say(env, RV9_STDOUT, "\n");
        return -3;
    }

    /*
     * Someone has to make the edges. Started before arming, so that its own
     * open cannot land in the middle of the setup below. Generate more than
     * we wait for: an edge nobody sees is cheaper than a measurement that
     * blocks forever.
     */
    int gpid = -1;
    if (interval > 0) {
        char arg[40];
        m_devpath(arg, "", pin);
        uint32_t a = m_len(arg);
        arg[a++] = ' ';
        m_devpath(&arg[a], "", edges + 20);
        a += m_len(&arg[a]);
        arg[a++] = ' ';
        m_devpath(&arg[a], "", interval);

        gpid = env->fork_arg("edgegen", 8, arg);
        if (gpid < 0) {
            m_say(env, RV9_STDOUT, "evlat: cannot start edgegen\n");
            env->close(p);
            return -4;
        }
    }

    /* Arm the pin, then ask the pin which event it signals on. */
    uint32_t both = 3;
    if (env->setstat(p, RV9_PIO_SS_EDGE, &both) < 0) {
        m_say(env, RV9_STDOUT, "evlat: pin will not arm\n");
        env->close(p);
        return -5;
    }

    uint32_t ev = 0;
    if (env->getstat(p, RV9_PIO_GS_EVENT, &ev) < 0 || ev == 0) {
        m_say(env, RV9_STDOUT, "evlat: pin has no event\n");
        env->close(p);
        return -6;
    }

    /*
     * Declare last. The default bound is half the interval we asked the
     * generator for, which leaves room for the host's tick to be early
     * without crying flood and still catches a source that has genuinely
     * run away. An external source defaults to 0 -- we do not know what it
     * will do, and guessing would be a promise made on its behalf.
     */
    if (env->rt_declare_event((int)ev, bound) < 0) {
        m_say(env, RV9_STDOUT, "evlat: not a real-time process; use: rt evlat\n");
        env->close(p);
        return -7;
    }

    for (uint32_t n = 0; n < edges; n++) {
        int coalesced = env->rt_wait();
        if (coalesced < 0) break;

        uint64_t now = env->time_us();
        uint32_t level = 0;
        env->read(p, &level, sizeof(level));

        /*
         * The KAL measures edge-to-process for the report. This measures it
         * again from up here, after the read, which is where a reactive
         * program actually has the world in its hands. The two differ by
         * the cost of the read, and seeing both is how you tell a slow
         * wake-up from a slow device.
         */
        uint32_t took = (uint32_t)(env->time_us() - now);
        st->total_us += took;
        if (took > st->worst_us) { st->worst_us = took; st->worst_at = n; }
        st->coalesced += (uint32_t)coalesced;
    }

    env->close(p);

    rv9_rt_report_t r;
    env->rt_stats(&r);

    m_say(env, RV9_STDOUT, "evlat: ");
    m_num(env, RV9_STDOUT, (int32_t)r.activations);
    m_say(env, RV9_STDOUT, " edges on ");
    m_say(env, RV9_STDOUT, name);
    m_say(env, RV9_STDOUT, "\n  worst edge to process  ");
    m_num(env, RV9_STDOUT, (int32_t)r.max_jitter_us);
    m_say(env, RV9_STDOUT, " us\n  worst read after wake  ");
    m_num(env, RV9_STDOUT, (int32_t)st->worst_us);
    m_say(env, RV9_STDOUT, " us (edge ");
    m_num(env, RV9_STDOUT, (int32_t)st->worst_at);
    m_say(env, RV9_STDOUT, ")\n  mean read after wake   ");
    m_num(env, RV9_STDOUT,
          (int32_t)(st->total_us / (r.activations ? r.activations : 1)));
    m_say(env, RV9_STDOUT, " us\n  edges coalesced        ");
    m_num(env, RV9_STDOUT, (int32_t)st->coalesced);
    m_say(env, RV9_STDOUT, "\n");

    if (gpid >= 0) {
        int status = 0;
        env->wait(gpid, &status, 5000);
    }
    return 0;
}
