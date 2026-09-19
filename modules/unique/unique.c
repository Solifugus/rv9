/*
 * unique -- drop a line that repeats the one before it.
 *
 *   mdir | field 2 | sort | unique       which kinds of module exist
 *
 * Adjacent only, which is why it is nearly always preceded by `sort`.
 * That is not a limitation to apologise for: remembering every line ever
 * seen is unbounded memory, and on this machine an unbounded tool is one
 * that works until the day it does not.
 */
#include "modlib.h"

#define LINE_CAP 96

typedef struct {
    m_lines_t in;
    char      line[LINE_CAP];
    char      prev[LINE_CAP];
} unique_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    unique_statics_t *st = (unique_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        /* Silence here is how four missing bytes cost an afternoon: the
           module returned -2, the shell reported nothing, and the command
           simply produced no output. */
        m_say(env, RV9_STDERR, "unique: static_size in build.conf is too small\n");
        return -2;
    }

    bool have_prev = false;

    for (;;) {
        if (m_getline(env, RV9_STDIN, &st->in, st->line, LINE_CAP) < 0) break;

        if (have_prev && m_eq(st->line, st->prev)) continue;

        m_say(env, RV9_STDOUT, st->line);
        m_say(env, RV9_STDOUT, "\n");

        uint32_t i = 0;
        while (st->line[i] && i < LINE_CAP - 1) { st->prev[i] = st->line[i]; i++; }
        st->prev[i] = '\0';
        have_prev = true;
    }
    return 0;
}
