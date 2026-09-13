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

/* If/else returning literals, not a table: a table of string pointers is
   an absolute address, and the build refuses modules that hold one. */
static const char *fault_name(uint8_t f)
{
    if (f == RV9_FAULT_STACK)    return "STACK";
    if (f == RV9_FAULT_KILLED)   return "killed";
    if (f == RV9_FAULT_DEADLINE) return "DEADLINE";
    if (f == RV9_FAULT_RUNAWAY)  return "RUNAWAY";
    return "fault ?";
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 5) return -1;

    procs_statics_t *st = (procs_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    int n = env->sysinfo(RV9_SYS_PROCS, st->procs, sizeof(st->procs));
    if (n < 0) return -3;

    /* Six wide: pids wrap at 65535 now rather than growing without end, and
       a five-digit pid ran into the next column. */
    m_say(env, RV9_STDOUT, "pid   par   name        state   base eff ended\n");

    for (int i = 0; i < n; i++) {
        const rv9_sys_proc_t *p = &st->procs[i];
        m_numpad(env, RV9_STDOUT, p->pid, 6);
        m_numpad(env, RV9_STDOUT, p->parent, 6);
        m_pad(env, RV9_STDOUT, p->name, 12);
        m_pad(env, RV9_STDOUT, state_name(p->state), 8);
        m_numpad(env, RV9_STDOUT, p->base_priority, 5);
        m_numpad(env, RV9_STDOUT, p->effective_priority, 4);

        /* How it ended, which for a machine that moves is the column that
           matters: returning, being stopped, and missing a deadline are
           three different stories about the same actuator. */
        if (p->state == 3) {
            if (p->fault != RV9_FAULT_NONE) {
                m_say(env, RV9_STDOUT, fault_name(p->fault));
            } else {
                m_say(env, RV9_STDOUT, "status ");
                m_num(env, RV9_STDOUT, p->status);
            }
        }
        m_say(env, RV9_STDOUT, "\n");
    }
    return 0;
}
