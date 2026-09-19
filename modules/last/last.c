/*
 * last -- the last few lines.
 *
 *   mdir | last           the last ten
 *   mdir | last 3         the last three
 *
 * It has to read everything to know which lines were last, so it keeps a
 * ring of exactly as many as it was asked for and lets the rest go by.
 * Asking for more than the ring holds is answered with what it can hold
 * rather than quietly with fewer -- a tool that silently gives you less
 * than you asked for is worse than one that says it cannot.
 */
#include "modlib.h"

/* Long enough for a log line, which is what this is mostly pointed at.
   96 cut "W (59469) wifi:<ba-add>idx:1, ifx:0, ..." in half, and a tool
   that quietly shortens what it shows you is worse than no tool. */
#define LINE_CAP  144
#define RING_MAX  16

typedef struct {
    m_lines_t in;
    char      ring[RING_MAX][LINE_CAP];
    char      line[LINE_CAP];
} last_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    last_statics_t *st = (last_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        /* Silence here is how four missing bytes cost an afternoon: the
           module returned -2, the shell reported nothing, and the command
           simply produced no output. */
        m_say(env, RV9_STDERR, "last: static_size in build.conf is too small\n");
        return -2;
    }

    uint32_t want = 10;
    if (env->arg != NULL && env->arg[0] >= '0' && env->arg[0] <= '9') {
        const char *end = env->arg;
        want = m_num_parse(env->arg, &end);
    }
    if (want == 0) return 0;
    if (want > RING_MAX) {
        m_say(env, RV9_STDERR, "last: I can hold 16 lines\n");
        want = RING_MAX;
    }

    uint32_t seen = 0;
    for (;;) {
        int n = m_getline(env, RV9_STDIN, &st->in, st->line, LINE_CAP);
        if (n < 0) break;

        char *slot = st->ring[seen % want];
        uint32_t i = 0;
        while (st->line[i] && i < LINE_CAP - 1) { slot[i] = st->line[i]; i++; }
        slot[i] = '\0';
        seen++;
    }

    uint32_t have  = (seen < want) ? seen : want;
    uint32_t start = (seen < want) ? 0 : (seen % want);

    for (uint32_t i = 0; i < have; i++) {
        m_say(env, RV9_STDOUT, st->ring[(start + i) % want]);
        m_say(env, RV9_STDOUT, "\n");
    }
    return 0;
}
