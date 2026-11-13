/*
 * reboot -- restart this machine, as asked.
 *
 * Counted as asked for, not as a bad end (`boots` says "asked to"), and
 * safe mode, if it is in it, stays as it is: `safe off` is the way out.
 */
#include "modlib.h"

typedef struct { rv9_sys_health_t h; } reboot_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 20) return 2;   /* RV9_SYS_HEALTH */
    reboot_statics_t *st = (reboot_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return 2;

    m_say(env, RV9_STDOUT, "restarting\n");
    st->h.op = RV9_HEALTH_REBOOT;
    env->sysinfo(RV9_SYS_HEALTH, &st->h, sizeof(st->h));
    m_say(env, RV9_STDERR, "reboot: the restart did not happen\n");
    return 1;
}
