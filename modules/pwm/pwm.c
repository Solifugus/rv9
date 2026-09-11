/*
 * pwm -- drive a PWM output.   pwm 3 1200      pwm 3 1200 50
 *
 * Duty is raw, 0 to the range the device reports. At the default 50 Hz and
 * 14 bits, a hobby servo wants roughly 820 (1 ms) to 1640 (2 ms).
 *
 * The path stays open only for the length of this command, so the output
 * stops when it exits -- an actuator left running because a program
 * finished is a bad way to discover that it was.
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;
    if (env->arg == NULL || env->arg[0] == '\0') {
        m_say(env, RV9_STDOUT, "usage: pwm <pin> <duty> [hz]\n");
        return -2;
    }

    char num[8];
    const char *rest = m_word(env->arg, num, sizeof(num));

    char path[24];
    uint32_t at = 0;
    const char *pre = "/pwm0/";
    while (pre[at]) { path[at] = pre[at]; at++; }
    for (uint32_t i = 0; num[i]; i++) path[at++] = num[i];
    path[at] = '\0';

    const char *after = rest;
    uint32_t duty = m_num_parse(rest, &after);
    while (*after == ' ') after++;
    uint32_t hz = m_num_parse(after, 0);

    int p = env->open(path, RV9_MODE_WRITE);
    if (p < 0) {
        m_say(env, RV9_STDOUT, path);
        m_say(env, RV9_STDOUT, ": cannot open\n");
        return -3;
    }

    if (hz >= 1) env->setstat(p, RV9_PIO_SS_FREQUENCY, &hz);

    uint32_t range = 0;
    env->getstat(p, RV9_PIO_GS_RANGE, &range);

    if (env->write(p, &duty, sizeof(duty)) < 0) {
        env->close(p);
        m_say(env, RV9_STDOUT, "write failed\n");
        return -4;
    }

    m_say(env, RV9_STDOUT, path);
    m_say(env, RV9_STDOUT, " duty ");
    m_num(env, RV9_STDOUT, (int32_t)duty);
    m_say(env, RV9_STDOUT, " of ");
    m_num(env, RV9_STDOUT, (int32_t)range);
    m_say(env, RV9_STDOUT, "\n(output stops when this command exits)\n");

    env->sleep_ms(2000);
    env->close(p);
    return 0;
}
