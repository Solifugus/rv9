/*
 * forkbomb -- start processes until RV-9 says no, then clean up.
 *
 * A bounded one: it starts `deaf` children, one after another, until a
 * fork is refused, holds them for half a second so the refusal can be
 * looked at, then kills every one it started and says how many that was.
 *
 * Its manifest gives it a 12 KB budget. Each child is charged to it, so it
 * is stopped at four or five children with RV9_PE_BUDGET -- not at the
 * machine's last kilobyte with "no memory", which is what happened before
 * budgets, and which took `kill` down with it.
 *
 * Returns the number started when the refusal was the budget, and -1 when
 * it was anything else: the point is to be stopped for the right reason.
 */
#include "modlib.h"

#define MAX_KIDS 16

typedef struct {
    int32_t kids[MAX_KIDS];
} forkbomb_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 13) return -1;

    forkbomb_statics_t *st = (forkbomb_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    int n = 0, refused = 0;
    while (n < MAX_KIDS) {
        int pid = env->fork_arg("deaf", 8, 0);
        if (pid < 0) { refused = pid; break; }
        st->kids[n++] = pid;
    }

    env->sleep_ms(500);

    for (int i = 0; i < n; i++) {
        int status = 0;
        env->kill(st->kids[i]);
        env->wait(st->kids[i], &status, 1000);
    }

    m_say(env, RV9_STDOUT, "forkbomb: started ");
    m_num(env, RV9_STDOUT, n);
    m_say(env, RV9_STDOUT, refused == -RV9_PE_BUDGET
                               ? ", then stopped by its budget\n"
                               : ", then stopped for another reason\n");

    return (refused == -RV9_PE_BUDGET) ? n : -1;
}
