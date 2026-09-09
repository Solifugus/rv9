/*
 * procs -- list the process table.
 *
 * Note that it always finds itself in the list: it is a process, forked by
 * the shell, asking the system what processes exist.
 */
#include "modlib.h"

#define MAX_PROCS 16

typedef struct {
    rv9_sys_proc_t procs[MAX_PROCS];
} procs_statics_t;

static const char *state_name(uint8_t s)
{
    switch (s) {
    case 1:  return "active";
    case 2:  return "waiting";
    case 3:  return "exited";
    default: return "?";
    }
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 5) return -1;

    procs_statics_t *st = (procs_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    int n = env->sysinfo(RV9_SYS_PROCS, st->procs, sizeof(st->procs));
    if (n < 0) return -3;

    m_say(env, RV9_STDOUT, "pid par name        state   base eff\n");

    for (int i = 0; i < n; i++) {
        m_numpad(env, RV9_STDOUT, st->procs[i].pid, 4);
        m_numpad(env, RV9_STDOUT, st->procs[i].parent, 4);
        m_pad(env, RV9_STDOUT, st->procs[i].name, 12);
        m_pad(env, RV9_STDOUT, state_name(st->procs[i].state), 8);
        m_numpad(env, RV9_STDOUT, st->procs[i].base_priority, 5);
        m_num(env, RV9_STDOUT, st->procs[i].effective_priority);
        m_say(env, RV9_STDOUT, "\n");
    }
    return 0;
}
