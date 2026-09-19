/*
 * field -- one column out of each line.
 *
 *   mdir | field 1        just the names
 *   procs | field 3       just the process names
 *
 * Columns are separated by runs of spaces or tabs, and counted from one,
 * because everything this machine prints is aligned with spaces and
 * nobody counting columns on a screen starts at zero.
 *
 * A line with fewer columns than asked for produces nothing rather than a
 * blank line: the blank would be a row in the output that did not exist
 * in the input, and anything counting afterwards would believe it.
 */
#include "modlib.h"

#define LINE_CAP 192

typedef struct {
    m_lines_t in;
    char      line[LINE_CAP];
} field_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    field_statics_t *st = (field_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        /* Silence here is how four missing bytes cost an afternoon: the
           module returned -2, the shell reported nothing, and the command
           simply produced no output. */
        m_say(env, RV9_STDERR, "field: static_size in build.conf is too small\n");
        return -2;
    }

    uint32_t want = 0;
    if (env->arg != NULL && env->arg[0] >= '0' && env->arg[0] <= '9') {
        const char *end = env->arg;
        want = m_num_parse(env->arg, &end);
    }
    if (want == 0) {
        m_say(env, RV9_STDERR, "usage: field <n>, counting from 1\n");
        return -3;
    }

    for (;;) {
        if (m_getline(env, RV9_STDIN, &st->in, st->line, LINE_CAP) < 0) break;

        char    *p = st->line;
        uint32_t n = 0;

        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0') break;

            n++;
            char *start = p;
            while (*p && *p != ' ' && *p != '\t') p++;

            if (n == want) {
                char keep = *p;
                *p = '\0';
                m_say(env, RV9_STDOUT, start);
                m_say(env, RV9_STDOUT, "\n");
                *p = keep;
                break;
            }
        }
    }
    return 0;
}
