/*
 * control -- a periodic control loop, and an honest account of its timing.
 *
 * The shape a real control loop has: declare a period, then read, compute,
 * act, wait. What makes it real-time is not the arithmetic in the middle
 * but that the waiting is bounded and the lateness is measured.
 *
 * The "plant" here is a first-order lag driven by a PI controller, which
 * is enough to give the loop realistic work to do. What matters for RV-9
 * is the jitter it reports at the end.
 */
#include "modlib.h"

#define DEFAULT_PERIOD_US 1000      /* 1 kHz */
#define RUN_ACTIVATIONS   2000      /* two seconds at 1 kHz */

typedef struct {
    int32_t  plant;          /* measured value, scaled by 1000 */
    int32_t  integral;
    uint32_t late_activations;
} control_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 10)      return -1;
    if (env->rt_declare == NULL || env->rt_wait == NULL) {
        m_say(env, RV9_STDOUT, "control: not a real-time process\n"
                               "  run it with: rt control [period_us]\n");
        return -2;
    }

    control_statics_t *st = (control_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -3;

    uint32_t period = DEFAULT_PERIOD_US;
    if (env->arg && env->arg[0]) {
        uint32_t v = 0;
        for (uint32_t i = 0; env->arg[i] >= '0' && env->arg[i] <= '9'; i++) {
            v = v * 10 + (uint32_t)(env->arg[i] - '0');
        }
        if (v >= 100) period = v;      /* below 100 us is not honest here */
    }

    if (env->rt_declare(period) < 0) {
        m_say(env, RV9_STDOUT, "control: could not declare a period\n");
        return -4;
    }

    const int32_t setpoint = 1000;
    const int32_t kp = 120, ki = 3;

    for (uint32_t n = 0; n < RUN_ACTIVATIONS; n++) {
        /* --- the loop body: read, compute, act --- */
        int32_t error = setpoint - st->plant;

        st->integral += error;
        if (st->integral >  100000) st->integral =  100000;
        if (st->integral < -100000) st->integral = -100000;

        int32_t drive = (kp * error + ki * st->integral) / 1000;

        /* First-order plant: it moves a fraction of the way each step. */
        st->plant += (drive - st->plant / 8);

        /* --- wait for the next period --- */
        int late = env->rt_wait();
        if (late > 0) st->late_activations += (uint32_t)late;
        if (late < 0) break;
    }

    rv9_rt_report_t r;
    if (env->rt_stats(&r) < 0) {
        m_say(env, RV9_STDOUT, "control: no timing available\n");
        return -5;
    }

    m_say(env, RV9_STDOUT, "control: ");
    m_num(env, RV9_STDOUT, (int32_t)r.activations);
    m_say(env, RV9_STDOUT, " activations at ");
    m_num(env, RV9_STDOUT, (int32_t)r.period_us);
    m_say(env, RV9_STDOUT, " us\n  worst jitter   ");
    m_num(env, RV9_STDOUT, (int32_t)r.max_jitter_us);
    m_say(env, RV9_STDOUT, " us\n  worst execute  ");
    m_num(env, RV9_STDOUT, (int32_t)r.max_exec_us);
    m_say(env, RV9_STDOUT, " us\n  overruns       ");
    m_num(env, RV9_STDOUT, (int32_t)r.overruns);
    m_say(env, RV9_STDOUT, "\n  plant settled at ");
    m_num(env, RV9_STDOUT, st->plant);
    m_say(env, RV9_STDOUT, " (setpoint 1000)\n");

    return (int)r.overruns;
}
