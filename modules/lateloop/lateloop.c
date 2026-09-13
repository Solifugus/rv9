/*
 * lateloop -- a control loop that misses its deadline on purpose.
 *
 *   rt lateloop          on time for 50 periods, then late once
 *   rt lateloop ontime   never late; runs a minute, for `kill` to stop
 *
 * Through `rt` a number is a period, not an argument -- `rt lateloop 1000`
 * asks for a 1 ms period, which admission refuses against a 2 ms deadline.
 * A process forking it directly may pass N to be late on period N.
 *
 * The manifest says a missed deadline is a fault (on_deadline=fault), that
 * /gpio/2 is this loop's alone, and that it is to be left at 0. The loop
 * drives the pin to 1, runs on time, and on period N spends 5 ms of a 2 ms
 * deadline.
 *
 * What should follow is R9 §15.1, in its order: the pin is parked at 0;
 * the process table says DEADLINE; and the loop is never released again.
 * None of it is code here. The line after the loop -- which says RV-9 let
 * a late loop carry on -- is the one that must never print.
 *
 * The busy wait is deliberate, and deliberately in the loop body. A late
 * loop is late because of what it did, not because of how it waited, and a
 * sleep would be the scheduler's lateness rather than the program's.
 */
#include "modlib.h"

#define PIN          "/gpio/2"
#define LATE_AT      50
#define LATE_BY_US   5000
#define PAST_IT      10        /* periods to keep going, if not stopped */
#define ONTIME_FOR   6000      /* a minute at 100 Hz */

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 13) return -1;
    if (env->rt_declare == NULL || env->rt_wait == NULL) {
        m_say(env, RV9_STDOUT, "lateloop: not a real-time process\n"
                               "  run it with: rt lateloop [N]\n");
        return -2;
    }

    uint32_t late_at = LATE_AT;
    bool never = false;
    if (env->arg && m_eq(env->arg, "ontime")) {
        never = true;
    } else if (env->arg && env->arg[0] >= '1' && env->arg[0] <= '9') {
        late_at = m_num_parse(env->arg, NULL);
    }
    uint32_t periods = never ? ONTIME_FOR : late_at + PAST_IT;

    /* Claimed at fork from the manifest; this open cannot be refused by
       anybody else. Opened before the period exists, as always. */
    int pin = env->open(PIN, RV9_MODE_RW);
    if (pin < 0) {
        m_say(env, RV9_STDOUT, "lateloop: cannot open " PIN "\n");
        return -3;
    }

    /* High, so that the 0 afterwards is evidence and not coincidence. */
    uint32_t one = 1;
    env->write(pin, &one, sizeof(one));

    if (env->rt_declare(0) < 0) {
        env->close(pin);
        m_say(env, RV9_STDOUT, "lateloop: could not declare a period\n");
        return -4;
    }

    for (uint32_t n = 0; n < periods; n++) {
        if (!never && n == late_at) {
            uint64_t t0 = env->time_us();
            while (env->time_us() - t0 < LATE_BY_US) { }
        }

        if (env->rt_wait() < 0) break;
    }

    env->close(pin);
    if (never) {
        m_say(env, RV9_STDOUT, "lateloop: on time throughout\n");
        return 0;
    }

    /* Reaching here means a late activation was followed by another. */
    m_say(env, RV9_STDOUT, "lateloop: still running after a missed "
                           "deadline -- RV-9 did not stop it\n");
    return 1;
}
