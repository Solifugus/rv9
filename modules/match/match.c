/*
 * match -- the lines that contain something.
 *
 *   mdir | match desc
 *   procs | match active
 *   match error < /f0/log.txt
 *
 * Plain text, not a pattern language. A substring search is what nearly
 * every use of this actually wants, it fits in a few hundred bytes, and it
 * never surprises anybody by treating a dot or a bracket as punctuation.
 * When something here needs regular expressions they can arrive as their
 * own tool rather than as a second meaning for this one.
 *
 * Reads standard input; a file arrives through `<`.
 */
#include "modlib.h"

#define CHUNK     128
#define LINE_CAP  192

typedef struct {
    char buf[CHUNK];
    char line[LINE_CAP];
} match_statics_t;

/* Does `hay` contain `needle`? Both NUL-terminated, needle non-empty. */
static bool contains(const char *hay, const char *needle)
{
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*b == '\0') return true;
    }
    return false;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    match_statics_t *st = (match_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        /* Silence here is how four missing bytes cost an afternoon: the
           module returned -2, the shell reported nothing, and the command
           simply produced no output. */
        m_say(env, RV9_STDERR, "match: static_size in build.conf is too small\n");
        return -2;
    }

    char want[48];
    want[0] = '\0';
    if (env->arg != NULL) m_word(env->arg, want, sizeof(want));

    if (want[0] == '\0') {
        m_say(env, RV9_STDERR, "usage: match <text>\n");
        return -3;
    }

    uint32_t len   = 0;
    bool     found = false;

    for (;;) {
        int n = env->read(RV9_STDIN, st->buf, CHUNK);
        if (n <= 0) break;

        for (int i = 0; i < n; i++) {
            char c = st->buf[i];

            /*
             * A line longer than the buffer is tested and emitted as far
             * as it fits rather than dropped. Losing a line silently
             * because it was long is worse than showing a long line
             * short, and this is a machine where a buffer is always
             * smaller than somebody would like.
             */
            if (c != '\n' && len < LINE_CAP - 1) st->line[len++] = c;

            if (c == '\n') {
                st->line[len] = '\0';
                if (contains(st->line, want)) {
                    m_say(env, RV9_STDOUT, st->line);
                    m_say(env, RV9_STDOUT, "\n");
                    found = true;
                }
                len = 0;
            }
        }
    }

    /* Whatever was left without a newline is still a line. */
    if (len > 0) {
        st->line[len] = '\0';
        if (contains(st->line, want)) {
            m_say(env, RV9_STDOUT, st->line);
            m_say(env, RV9_STDOUT, "\n");
            found = true;
        }
    }

    /* Nothing matched is not a failure to run; it is an answer. Status
       says which, so a script can ask without reading the output. */
    return found ? 0 : 1;
}
