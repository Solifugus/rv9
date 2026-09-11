/*
 * backlight -- turn the panel up or down.   backlight        backlight 30
 *
 * The display is the largest continuous draw on this board. Turning it
 * down is the cheapest way to run cooler and longer, and it does not cost
 * the system anything else: the console keeps working at any brightness,
 * including none.
 *
 * Not remembered across a reset. A machine that boots with a dark display
 * is unnecessarily hard to diagnose.
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    int p = env->open("/term", RV9_MODE_WRITE);
    if (p < 0) {
        m_say(env, RV9_STDOUT, "/term: cannot open\n");
        return -2;
    }

    if (env->arg == NULL || env->arg[0] == '\0') {
        uint32_t now = 0;
        if (env->getstat(p, RV9_LCD_SS_BRIGHTNESS, &now) < 0) {
            m_say(env, RV9_STDOUT, "cannot read brightness\n");
            env->close(p);
            return -3;
        }
        m_say(env, RV9_STDOUT, "backlight ");
        m_num(env, RV9_STDOUT, (int32_t)now);
        m_say(env, RV9_STDOUT, "%\nusage: backlight <0-100>\n");
        env->close(p);
        return 0;
    }

    uint32_t want = m_num_parse(env->arg, 0);
    if (want > 100) want = 100;

    if (env->setstat(p, RV9_LCD_SS_BRIGHTNESS, &want) < 0) {
        m_say(env, RV9_STDOUT, "cannot set brightness\n");
        env->close(p);
        return -4;
    }

    m_say(env, RV9_STDOUT, "backlight ");
    m_num(env, RV9_STDOUT, (int32_t)want);
    m_say(env, RV9_STDOUT, "%\n");

    env->close(p);
    return 0;
}
