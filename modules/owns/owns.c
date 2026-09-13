/*
 * owns -- who has which device.
 *
 * A count of open paths would answer "is it busy". This answers "who has
 * it", which is the question worth asking when something physical is
 * happening and only one program should be causing it.
 *
 * One line per owner, so a device shared by five processes is five lines.
 * `reserved` marks a claim taken from a program's manifest at fork rather
 * than by an open: that program owns the device for its whole life, not
 * just while it has it open, and nothing else will get it in between.
 */
#include "modlib.h"

#define MAX 12

typedef struct { rv9_sys_claim_t c[MAX]; } owns_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    owns_t *st = (owns_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    /*
     * The count comes back from the same call that fills the buffer, and
     * may exceed what fits: sysinfo says how many there are, not how many
     * it wrote. Asking for a count first is not an option -- sysinfo
     * refuses a null buffer, which is the right refusal for every other
     * record and merely inconvenient here.
     */
    int total = env->sysinfo(RV9_SYS_CLAIM, st->c, sizeof(st->c));
    if (total < 0) {
        m_say(env, RV9_STDOUT, "this system does not track ownership\n");
        return -3;
    }
    if (total == 0) {
        m_say(env, RV9_STDOUT, "nothing is owned\n");
        return 0;
    }

    int n = (total > MAX) ? MAX : total;

    m_say(env, RV9_STDOUT, "resource                  owner  refs  held as\n");

    for (int i = 0; i < n; i++) {
        m_pad(env, RV9_STDOUT, st->c[i].name, 26);

        if (st->c[i].owner == 0) {
            /* A driver's own hold, not a process's: no exit will end it. */
            m_pad(env, RV9_STDOUT, "sys", 7);
        } else {
            m_numpad(env, RV9_STDOUT, st->c[i].owner, 5);
            m_say(env, RV9_STDOUT, "  ");
        }

        m_numpad(env, RV9_STDOUT, (int32_t)st->c[i].refs, 4);
        m_say(env, RV9_STDOUT, "  ");
        m_say(env, RV9_STDOUT, st->c[i].exclusive ? "exclusive" : "shared");
        if (st->c[i].reserved_at_fork) m_say(env, RV9_STDOUT, ", reserved");
        m_say(env, RV9_STDOUT, "\n");
    }

    if (total > n) {
        m_say(env, RV9_STDOUT, "... and ");
        m_num(env, RV9_STDOUT, total - n);
        m_say(env, RV9_STDOUT, " more\n");
    }
    return 0;
}
