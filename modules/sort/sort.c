/*
 * sort -- the lines, in order.
 *
 *   mdir | field 1 | sort
 *   mdir | field 2 | sort | unique
 *
 * Sorting needs every line at once, which on a machine like this means a
 * bound has to exist somewhere. It is here and it is stated: a hundred and twenty-eight lines
 * of eighty characters. Input past that is **reported, not dropped** --
 * a sort that silently returned some of your lines in order would be
 * worse than useless, because the answer would look right.
 *
 * An insertion sort, because a hundred items is not where an algorithm
 * matters and the whole thing is twenty lines. Comparison is by byte, so
 * capitals sort before lowercase; that is the order of the character set
 * rather than an opinion about alphabets.
 */
#include "modlib.h"

#define LINE_CAP  64
#define LINE_MAXN 128

typedef struct {
    m_lines_t in;
    char      line[LINE_CAP];
    char      lines[LINE_MAXN][LINE_CAP];
} sort_statics_t;

/* Negative, zero or positive, like every comparison ever written. */
static int cmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void copy_into(char *dst, const char *src)
{
    uint32_t i = 0;
    while (src[i] && i < LINE_CAP - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    sort_statics_t *st = (sort_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        /* Silence here is how four missing bytes cost an afternoon: the
           module returned -2, the shell reported nothing, and the command
           simply produced no output. */
        m_say(env, RV9_STDERR, "sort: static_size in build.conf is too small\n");
        return -2;
    }

    uint32_t n = 0;
    bool     full = false, toolong = false;

    for (;;) {
        if (m_getline(env, RV9_STDIN, &st->in, st->line, LINE_CAP) < 0) break;

        if (n == LINE_MAXN) { full = true; break; }

        /* A line at the cap was truncated on the way in by m_getline, and
           quietly sorting a shortened line is the other way to give an
           answer that looks right and is not. */
        if (m_len(st->line) >= LINE_CAP - 1) { toolong = true; break; }

        /* Insert where it belongs, shifting the tail down one. */
        uint32_t at = n;
        while (at > 0 && cmp(st->lines[at - 1], st->line) > 0) {
            copy_into(st->lines[at], st->lines[at - 1]);
            at--;
        }
        copy_into(st->lines[at], st->line);
        n++;
    }

    if (full) {
        m_say(env, RV9_STDERR, "sort: more than 128 lines; refusing to "
                               "answer with only some of them\n");
        return -3;
    }
    if (toolong) {
        m_say(env, RV9_STDERR, "sort: a line longer than 63 characters; "
                               "refusing to sort it shortened\n");
        return -4;
    }

    for (uint32_t i = 0; i < n; i++) {
        m_say(env, RV9_STDOUT, st->lines[i]);
        m_say(env, RV9_STDOUT, "\n");
    }
    return 0;
}
