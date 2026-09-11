/*
 * temp -- read the die temperature.   temp        temp 10
 *
 * "The chip feels warm" is not a measurement, and a robot cannot feel
 * anything. With a sample count it watches for a while and reports the
 * range, which is the part that says whether something is heating up or
 * merely warm.
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    uint32_t samples = 1;
    if (env->arg && env->arg[0]) {
        uint32_t v = m_num_parse(env->arg, 0);
        if (v > 0) samples = v > 60 ? 60 : v;
    }

    int p = env->open("/tsens/0", RV9_MODE_READ);
    if (p < 0) {
        m_say(env, RV9_STDOUT, "/tsens/0: cannot open\n");
        return -2;
    }

    int32_t lo = 0x7FFFFFFF, hi = -0x7FFFFFFF;

    for (uint32_t i = 0; i < samples; i++) {
        uint32_t raw = 0;
        if (env->read(p, &raw, sizeof(raw)) < 0) {
            m_say(env, RV9_STDOUT, "read failed\n");
            env->close(p);
            return -3;
        }

        int32_t c = (int32_t)raw;
        if (c < lo) lo = c;
        if (c > hi) hi = c;

        m_num(env, RV9_STDOUT, c / 100);
        m_say(env, RV9_STDOUT, ".");
        int32_t frac = c % 100;
        if (frac < 10) m_say(env, RV9_STDOUT, "0");
        m_num(env, RV9_STDOUT, frac);
        m_say(env, RV9_STDOUT, " C\n");

        if (i + 1 < samples) env->sleep_ms(1000);
    }

    if (samples > 1) {
        m_say(env, RV9_STDOUT, "range ");
        m_num(env, RV9_STDOUT, lo / 100);
        m_say(env, RV9_STDOUT, " to ");
        m_num(env, RV9_STDOUT, hi / 100);
        m_say(env, RV9_STDOUT, " C over ");
        m_num(env, RV9_STDOUT, (int32_t)samples);
        m_say(env, RV9_STDOUT, " s\n");
    }

    env->close(p);
    return 0;
}
