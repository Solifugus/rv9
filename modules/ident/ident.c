/*
 * ident -- who this machine is to its peers.
 *
 *   rv9> ident
 *   1f0c9a4e5b7d03a2c6e8f19b4d27a5c3
 *
 * The id a peer lists this machine by in its /f0/callers, to let it run
 * commands there with `on` (design section 76):
 *
 *   p4> echo c5 1f0c9a4e5b7d03a2c6e8f19b4d27a5c3 >> /f0/callers
 *
 * It names this machine's key, the SSH host key it made for itself the
 * first time it started, and short enough to type: the first sixteen
 * bytes of the SHA-256 of the public key. The key itself never leaves.
 */
#include "modlib.h"

typedef struct {
    rv9_sys_ident_t id;
    char hex[33];
} ident_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 18) return 2;   /* RV9_SYS_IDENT */

    ident_statics_t *st = (ident_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return 2;

    if (env->sysinfo(RV9_SYS_IDENT, &st->id, sizeof(st->id)) != 1) {
        m_say(env, RV9_STDERR, "ident: this machine has no key (it is the SSH host key, "
                               "and this firmware has no SSH)\n");
        return 1;
    }
    for (int i = 0; i < 16; i++) {
        st->hex[2 * i]     = "0123456789abcdef"[st->id.id[i] >> 4];
        st->hex[2 * i + 1] = "0123456789abcdef"[st->id.id[i] & 15];
    }
    st->hex[32] = '\0';
    m_say(env, RV9_STDOUT, st->hex);
    m_say(env, RV9_STDOUT, "\n");
    return 0;
}
