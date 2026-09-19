/*
 * first -- the first few lines, and then stop.
 *
 *   mdir | first          the first ten
 *   mdir | first 3        the first three
 *
 * Stopping matters as much as printing: once it has what it asked for it
 * closes its input, which on a pipe is what tells the stage upstream that
 * nobody is listening any more.
 */
#include "modlib.h"

#define LINE_CAP 192

typedef struct {
    m_lines_t in;
    char      line[LINE_CAP];
} first_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    first_statics_t *st = (first_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        /* Silence here is how four missing bytes cost an afternoon: the
           module returned -2, the shell reported nothing, and the command
           simply produced no output. */
        m_say(env, RV9_STDERR, "first: static_size in build.conf is too small\n");
        return -2;
    }

    uint32_t want = 10;
    if (env->arg != NULL && env->arg[0] >= '0' && env->arg[0] <= '9') {
        const char *end = env->arg;
        want = m_num_parse(env->arg, &end);
    }

    for (uint32_t n = 0; n < want; n++) {
        if (m_getline(env, RV9_STDIN, &st->in, st->line, LINE_CAP) < 0) break;
        m_say(env, RV9_STDOUT, st->line);
        m_say(env, RV9_STDOUT, "\n");
    }
    return 0;
}
