/*
 * uptime -- how long since the board came up.
 *
 *   uptime           up 2h 14m 8s
 *
 * The first question anybody asks a machine that is misbehaving, and
 * until now the only way to answer it was to read a log timestamp off the
 * serial line -- which needs a cable, and which resets the very thing you
 * were asking about if you reach for the wrong button.
 */
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;
    if (env->time_ms == NULL) return -2;

    /*
     * Narrowed before the divide, not after. A module links against no
     * runtime library, so a 64-bit division is an undefined reference to
     * __udivdi3 rather than an instruction -- the linker catches it, which
     * is the second time today the module rules have caught something
     * real.
     *
     * Thirty-two bits of milliseconds is forty-nine days, which is longer
     * than this board has ever been up and is stated here so that whoever
     * first sees it wrap knows why.
     */
    uint32_t ms = (uint32_t)env->time_ms();
    uint32_t s  = ms / 1000u;

    uint32_t days  = s / 86400u; s %= 86400u;
    uint32_t hours = s / 3600u;  s %= 3600u;
    uint32_t mins  = s / 60u;    s %= 60u;

    m_say(env, RV9_STDOUT, "up ");
    if (days)  { m_num(env, RV9_STDOUT, (int32_t)days);  m_say(env, RV9_STDOUT, "d "); }
    if (hours || days) { m_num(env, RV9_STDOUT, (int32_t)hours); m_say(env, RV9_STDOUT, "h "); }
    if (mins || hours || days) { m_num(env, RV9_STDOUT, (int32_t)mins); m_say(env, RV9_STDOUT, "m "); }
    m_num(env, RV9_STDOUT, (int32_t)s);
    m_say(env, RV9_STDOUT, "s\n");
    return 0;
}
