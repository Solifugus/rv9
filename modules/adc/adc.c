/*
 * adc -- read an analogue channel.   adc 1      adc 1 10
 *
 * Raw conversions, not millivolts: calibration belongs to the board and
 * the sensor, not to the converter.
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;
    if (env->arg == NULL || env->arg[0] == '\0') {
        m_say(env, RV9_STDOUT, "usage: adc <channel> [samples]\n");
        return -2;
    }

    char num[8];
    const char *rest = m_word(env->arg, num, sizeof(num));

    char path[24];
    uint32_t at = 0;
    const char *pre = "/adc0/";
    while (pre[at]) { path[at] = pre[at]; at++; }
    for (uint32_t i = 0; num[i]; i++) path[at++] = num[i];
    path[at] = '\0';

    uint32_t samples = m_num_parse(rest, 0);
    if (samples == 0) samples = 1;
    if (samples > 64) samples = 64;

    int p = env->open(path, RV9_MODE_READ);
    if (p < 0) {
        m_say(env, RV9_STDOUT, path);
        m_say(env, RV9_STDOUT, ": cannot open\n");
        return -3;
    }

    uint32_t range = 0;
    env->getstat(p, RV9_PIO_GS_RANGE, &range);

    for (uint32_t i = 0; i < samples; i++) {
        uint32_t v = 0;
        if (env->read(p, &v, sizeof(v)) < 0) {
            m_say(env, RV9_STDOUT, "read failed\n");
            env->close(p);
            return -4;
        }
        m_num(env, RV9_STDOUT, (int32_t)v);
        m_say(env, RV9_STDOUT, i + 1 < samples ? " " : "");
        if (samples > 1) env->sleep_ms(50);
    }

    m_say(env, RV9_STDOUT, "\nof ");
    m_num(env, RV9_STDOUT, (int32_t)range);
    m_say(env, RV9_STDOUT, " full scale\n");

    env->close(p);
    return 0;
}
