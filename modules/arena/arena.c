/*
 * arena -- a program with memory of its own, and proof of what it was given.
 *
 *   arena          say how big the arena is, and that it arrived zeroed
 *   arena hold     dirty all of it, then hold it until asked to stop
 *   arena spawn    start copies holding theirs until one is refused
 *
 * Its manifest asks for 64 KB of external memory and gives it, and
 * everything it starts, 192 KB of external budget -- so `spawn` gets two
 * children, and the third is refused with RV9_PE_BUDGET. `hold` dirties
 * what it holds so that the next arena carved from the same block shows
 * whether it was really zeroed.
 *
 * `arenain` is the same program with 4 KB of internal arena: the default
 * place, and the only one on a board with no external memory.
 *
 * Returns 0, or what was wrong: -3 no arena, -4 not aligned to 16, -5 not
 * zeroed, -6 did not read back what was written. `spawn` returns the
 * number started when the refusal was the budget, and -1 otherwise.
 */
#include "modlib.h"

#ifndef ARENA_SELF
#define ARENA_SELF "arena"
#endif

#define MAX_KIDS 8

typedef struct {
    int32_t kids[MAX_KIDS];
} arena_statics_t;

static int arena_check(const rv9_mod_env_t *env)
{
    if (env->heap == 0 || env->heap_size == 0) return -3;
    if (((uint32_t)env->heap & 15u) != 0) return -4;

    const uint32_t *w = (const uint32_t *)env->heap;
    uint32_t n = env->heap_size / 4;
    for (uint32_t i = 0; i < n; i++) {
        if (w[i] != 0) return -5;
    }
    return 0;
}

static int arena_dirty(const rv9_mod_env_t *env)
{
    volatile uint32_t *w = (volatile uint32_t *)env->heap;
    uint32_t n = env->heap_size / 4;
    for (uint32_t i = 0; i < n; i++) w[i] = 0xA5A5A5A5u ^ i;
    for (uint32_t i = 0; i < n; i++) {
        if (w[i] != (0xA5A5A5A5u ^ i)) return -6;
    }
    return 0;
}

static int arena_spawn(const rv9_mod_env_t *env, arena_statics_t *st)
{
    int n = 0, refused = 0;
    while (n < MAX_KIDS) {
        int pid = env->fork_arg(ARENA_SELF, 8, "hold");
        if (pid < 0) { refused = pid; break; }
        st->kids[n++] = pid;
    }

    for (int i = 0; i < n; i++) {
        int status = 0;
        env->signal(st->kids[i], RV9_SIG_STOP);
        env->wait(st->kids[i], &status, 2000);
    }

    m_say(env, RV9_STDOUT, ARENA_SELF ": started ");
    m_num(env, RV9_STDOUT, n);
    m_say(env, RV9_STDOUT, refused == -RV9_PE_BUDGET
                               ? ", then stopped by its budget\n"
                               : ", then stopped for another reason\n");
    return (refused == -RV9_PE_BUDGET) ? n : -1;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == 0 || env->abi_version < 15) return -1;

    arena_statics_t *st = (arena_statics_t *)env->statics;
    if (st == 0 || env->statics_size < sizeof(*st)) return -2;

    int rc = arena_check(env);
    if (rc != 0) {
        m_say(env, RV9_STDOUT, ARENA_SELF ": ");
        m_say(env, RV9_STDOUT, rc == -3 ? "no arena\n"
                             : rc == -4 ? "not aligned to 16\n"
                             :            "did not arrive zeroed\n");
        return rc;
    }

    if (env->arg != 0 && m_eq(env->arg, "spawn")) return arena_spawn(env, st);

    if (env->arg != 0 && m_eq(env->arg, "hold")) {
        rc = arena_dirty(env);
        if (rc != 0) return rc;
        while ((env->signals_take() & RV9_SIG_STOP) == 0) env->sleep_ms(20);
        return 0;
    }

    m_say(env, RV9_STDOUT, ARENA_SELF ": ");
    m_num(env, RV9_STDOUT, (int32_t)env->heap_size);
    m_say(env, RV9_STDOUT, " bytes, aligned to 16, arrived zeroed\n");
    return arena_dirty(env);
}
