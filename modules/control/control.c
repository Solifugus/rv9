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
 *
 * It also publishes. Every period it leaves what it measured in
 * /pub0/CONTROL, as one write of one struct, which is the whole of R9's
 * `expose speed, error, output`: the three numbers become visible
 * together or not at all, and a supervisor in another process reads them
 * as a set. Watch it happen with `watch CONTROL` in another shell.
 */
#include "modlib.h"

#define RUN_ACTIVATIONS   2000      /* two seconds at 1 kHz */

/*
 * What this component exposes.
 *
 * Fixed size, decided at build time, published as one object. Nothing here
 * is allocated and nothing is locked -- the cell was opened before the
 * period was declared, which is the initialisation-then-execution split
 * RV-9 has always had and R9 §16.1 relies on.
 */
typedef struct {
    rv9_pub_t head;
    int32_t   plant;
    int32_t   error;
    int32_t   drive;
} control_pub_t;

typedef struct {
    int32_t       plant;     /* measured value, scaled by 1000 */
    int32_t       integral;
    uint32_t      late_activations;
    control_pub_t out;
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

    /*
     * Zero asks for the period this process was admitted at, which comes
     * from the manifest in build.conf. The rate is a property of the
     * control law and belongs written down beside it, not as a constant
     * here that nothing outside the module can read.
     */
    uint32_t period = 0;
    if (env->arg && env->arg[0]) {
        uint32_t v = 0;
        for (uint32_t i = 0; env->arg[i] >= '0' && env->arg[i] <= '9'; i++) {
            v = v * 10 + (uint32_t)(env->arg[i] - '0');
        }
        if (v >= 100) period = v;      /* below 100 us is not honest here */
    }

    /*
     * Open the cell before declaring the period, never inside the loop.
     * Opening allocates and may block; publishing into an already-open
     * cell is a memcpy and two stores. That is the phase split, and it is
     * why a control loop can publish at all.
     *
     * A refusal here is not fatal: the loop still controls the plant, it
     * simply cannot be watched. Refusing to run because nobody is looking
     * would be the wrong priority for the one process that must keep
     * going.
     */
    int pub = env->open("/pub0/CONTROL", RV9_MODE_WRITE);
    if (pub < 0) {
        m_say(env, RV9_STDOUT, "control: cannot publish (running blind)\n");
    }

    /*
     * Publish once here, before the period exists.
     *
     * Not for the value -- it is zeros -- but for the path. The first write
     * through a path is measurably the most expensive one: it is the call
     * that pulls the I/O manager's code into cache. Doing it inside the
     * loop put 30 microseconds of first-call cost on activation one, which
     * is 60% of this loop's whole declared budget spent warming up.
     *
     * That is what the initialisation phase is for, and it is worth doing
     * deliberately rather than discovering it in the jitter: anything a
     * real-time loop will touch should be touched once before the loop
     * makes a promise about how long it takes.
     */
    if (pub >= 0) {
        st->out.head.len = 3 * sizeof(int32_t);
        env->write(pub, &st->out, sizeof(st->out));
    }

    if (env->rt_declare(period) < 0) {
        if (pub >= 0) env->close(pub);
        m_say(env, RV9_STDOUT, "control: could not declare a period\n");
        return -4;
    }

    const int32_t setpoint = 1000;
    const int32_t kp = 120, ki = 3;

    for (uint32_t n = 0; n < RUN_ACTIVATIONS; n++) {
        /* --- the loop body: read, compute, act --- */
        uint64_t observed = env->time_us();
        int32_t error = setpoint - st->plant;

        st->integral += error;
        if (st->integral >  100000) st->integral =  100000;
        if (st->integral < -100000) st->integral = -100000;

        int32_t drive = (kp * error + ki * st->integral) / 1000;

        /* First-order plant: it moves a fraction of the way each step. */
        st->plant += (drive - st->plant / 8);

        /* --- publish, as one indivisible set --- */
        if (pub >= 0) {
            st->out.plant = st->plant;
            st->out.error = error;
            st->out.drive = drive;
            st->out.head.len = 3 * sizeof(int32_t);

            /* The time the reading was taken, not the time it is being
               handed over. A supervisor asking how stale this is wants
               the first. */
            st->out.head.stamp_us = observed;

            env->write(pub, &st->out, sizeof(st->out));
        }

        /* --- wait for the next period --- */
        int late = env->rt_wait();
        if (late > 0) st->late_activations += (uint32_t)late;
        if (late < 0) break;
    }

    /*
     * Closing the cell does not erase it. The last thing this loop
     * measured, and when it measured it, stays readable after the process
     * is gone -- which is the state anything investigating a stopped
     * machine actually wants.
     */
    if (pub >= 0) env->close(pub);

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
