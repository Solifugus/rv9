/*
 * budgets -- what each running process is charged, and what it may hold.
 *
 * `own` is a process's footprint: its stack, its statics, and what RV-9
 * keeps about it. `held` adds every live process it started, and theirs.
 * A fork that would take any process's `held` past its `budget` is refused
 * -- so the column to watch is how close `held` is to `budget` for a
 * shell running a lot in the background.
 */
#include "modlib.h"

#define MAX_ROWS 24

typedef struct {
    rv9_sys_budget_t rows[MAX_ROWS];
} budgets_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 13) return -1;

    budgets_statics_t *st = (budgets_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    int n = env->sysinfo(RV9_SYS_BUDGETS, st->rows, sizeof(st->rows));
    if (n < 0) {
        m_say(env, RV9_STDOUT, "budgets: this system keeps none\n");
        return -3;
    }
    if (n > MAX_ROWS) n = MAX_ROWS;

    m_say(env, RV9_STDOUT, "pid   par   name             own   held  budget\n");
    for (int i = 0; i < n; i++) {
        const rv9_sys_budget_t *r = &st->rows[i];
        m_numpad(env, RV9_STDOUT, r->pid, 6);
        m_numpad(env, RV9_STDOUT, r->parent, 6);
        m_pad(env, RV9_STDOUT, r->name, 14);
        m_numpad(env, RV9_STDOUT, (int32_t)r->footprint, 7);
        m_numpad(env, RV9_STDOUT, (int32_t)r->held, 7);
        m_num(env, RV9_STDOUT, (int32_t)r->budget);
        m_say(env, RV9_STDOUT, "\n");
    }
    return 0;
}
