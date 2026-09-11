/*
 * iolat -- how long does a control loop wait to move a pin?
 *
 * Runs a real-time loop writing a GPIO and reports the worst single write,
 * which is the number a control designer actually needs: not the average,
 * and not the number measured on an idle machine.
 *
 *     rt iolat            2000 activations at 1 kHz, about two seconds
 *
 * The path is opened before the loop starts. Opening and closing are not
 * bounded -- they allocate, and closing a file on a volume writes flash --
 * so a control loop opens what it needs once and then only reads and
 * writes, which is the arrangement this measures.
 *
 * What it found, and why RV9_RT_CODE exists: with the radio associated,
 * the first run after a boot lost about 200 ms in one piece, every time,
 * and never again on that boot. That is the WiFi stack storing calibration
 * data -- a flash write, and a flash write switches off the cache that
 * makes flash readable. Anything in flash is gone while it happens. The
 * write path is in IRAM now, so the loop keeps running through it.
 */
#include "modlib.h"

#define PERIOD_US    1000
#define ACTIVATIONS  2000

typedef struct {
    uint32_t worst_us;
    uint32_t worst_at;
    uint32_t total_us;   /* 32-bit deliberately: a module has no libgcc, so
                            a 64-bit divide would not link */
} iolat_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL) return -1;
    if (env->abi_version < 11 || env->time_us == NULL) {
        m_say(env, RV9_STDOUT, "iolat: needs ABI 11\n");
        return -1;
    }
    if (env->rt_declare == NULL) {
        m_say(env, RV9_STDOUT, "iolat: run it with: rt iolat\n");
        return -2;
    }

    iolat_statics_t *st = (iolat_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -3;

    /* Opened once, before the first deadline exists. */
    int pin = env->open("/gpio/3", RV9_MODE_RW);
    if (pin < 0) {
        m_say(env, RV9_STDOUT, "iolat: cannot open /gpio/3\n");
        return -4;
    }

    /* Declare last. Starting anything before knowing we are real-time left
       it running after a failed attempt, which made the failure harder to
       read than the thing it was measuring. */
    if (env->rt_declare(PERIOD_US) < 0) {
        m_say(env, RV9_STDOUT, "iolat: not a real-time process; use: rt iolat\n");
        env->close(pin);
        return -5;
    }

    for (uint32_t n = 0; n < ACTIVATIONS; n++) {
        uint32_t level = n & 1;

        uint64_t t0 = env->time_us();
        env->write(pin, &level, sizeof(level));
        uint64_t t1 = env->time_us();

        uint32_t took = (uint32_t)(t1 - t0);
        st->total_us += took;
        if (took > st->worst_us) { st->worst_us = took; st->worst_at = n; }

        if (env->rt_wait() < 0) break;
    }

    env->close(pin);

    rv9_rt_report_t r;
    env->rt_stats(&r);

    m_say(env, RV9_STDOUT, "iolat: ");
    m_num(env, RV9_STDOUT, (int32_t)r.activations);
    m_say(env, RV9_STDOUT, " writes to /gpio/3 at ");
    m_num(env, RV9_STDOUT, (int32_t)r.period_us);
    m_say(env, RV9_STDOUT, " us\n  worst write    ");
    m_num(env, RV9_STDOUT, (int32_t)st->worst_us);
    m_say(env, RV9_STDOUT, " us (activation ");
    m_num(env, RV9_STDOUT, (int32_t)st->worst_at);
    m_say(env, RV9_STDOUT, ")\n  mean write     ");
    m_num(env, RV9_STDOUT, (int32_t)(st->total_us / (r.activations ? r.activations : 1)));
    m_say(env, RV9_STDOUT, " us\n  worst wakeup   ");
    m_num(env, RV9_STDOUT, (int32_t)r.max_jitter_us);
    m_say(env, RV9_STDOUT, " us late\n  periods missed ");
    m_num(env, RV9_STDOUT, (int32_t)r.overruns);
    m_say(env, RV9_STDOUT, "\n");

    return 0;
}
