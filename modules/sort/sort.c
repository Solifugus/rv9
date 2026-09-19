/*
 * sort -- the lines, in order.
 *
 *   mdir | field 1 | sort
 *   mdir | field 2 | sort | unique
 *
 * Sorting needs every line at once, so a bound has to exist. There are
 * two, and both are **reported, not silently applied**: a sort that
 * returned some of your lines in order, or all of them shortened, would be
 * worse than one that refused, because the answer would look right.
 *
 * WHY THE LINES ARE PACKED AND THE OFFSETS ARE SORTED
 *
 * The first version kept a fixed grid: 128 rows of 80 characters, 10 KB of
 * statics whether the lines were eighty characters or four. That made it
 * the fattest module on the machine, and in a four-stage pipeline it
 * fitted and left nothing for the stage after it -- which is the same as
 * not fitting.
 *
 * So the text goes into one arena end to end, and what gets sorted is an
 * array of offsets into it. Half the memory, twice the lines, and the
 * capacity now depends on what the lines actually are rather than on the
 * longest one imaginable: `mdir`'s ninety-nine lines average thirty-six
 * characters and use a third of the arena.
 *
 * An insertion sort on the offsets, because a hundred items is not where
 * an algorithm matters and moving two-byte offsets is cheap. Comparison is
 * by byte, so capitals sort before lowercase -- the order of the character
 * set rather than an opinion about alphabets.
 */
#include "modlib.h"

#define LINE_CAP   128      /* the longest single line it will take */
#define TEXT_CAP   4096     /* all the lines together */
#define LINES_MAX  192      /* how many, whatever their length */

typedef struct {
    m_lines_t in;
    char      line[LINE_CAP];
    char      text[TEXT_CAP];
    uint16_t  off[LINES_MAX];
} sort_statics_t;

/* Negative, zero or positive, like every comparison ever written. */
static int cmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    sort_statics_t *st = (sort_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        m_say(env, RV9_STDERR, "sort: static_size in build.conf is too small\n");
        return -2;
    }

    uint32_t n = 0, used = 0;
    bool too_many = false, too_long = false, too_much = false;

    for (;;) {
        int len = m_getline(env, RV9_STDIN, &st->in, st->line, LINE_CAP);
        if (len < 0) break;

        if (len >= LINE_CAP - 1)        { too_long = true; break; }
        if (n == LINES_MAX)             { too_many = true; break; }
        if (used + (uint32_t)len + 1 > TEXT_CAP) { too_much = true; break; }

        /* Park the line at the end of the arena, then slide its offset
           into place. The text never moves; only the offsets do. */
        char *at = st->text + used;
        for (int i = 0; i <= len; i++) at[i] = st->line[i];

        uint32_t slot = n;
        while (slot > 0 && cmp(st->text + st->off[slot - 1], at) > 0) {
            st->off[slot] = st->off[slot - 1];
            slot--;
        }
        st->off[slot] = (uint16_t)used;

        used += (uint32_t)len + 1;
        n++;
    }

    if (too_long) {
        m_say(env, RV9_STDERR, "sort: a line longer than 127 characters; "
                               "refusing to sort it shortened\n");
        return -4;
    }
    if (too_many) {
        m_say(env, RV9_STDERR, "sort: more than 192 lines; refusing to "
                               "answer with only some of them\n");
        return -3;
    }
    if (too_much) {
        m_say(env, RV9_STDERR, "sort: more than 4096 characters; refusing to "
                               "answer with only some of them\n");
        return -5;
    }

    for (uint32_t i = 0; i < n; i++) {
        m_say(env, RV9_STDOUT, st->text + st->off[i]);
        m_say(env, RV9_STDOUT, "\n");
    }
    return 0;
}
