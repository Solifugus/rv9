/*
 * count -- how much came through.
 *
 *   mdir | count            42 lines, 130 words, 980 bytes
 *   mdir | count lines      42
 *
 * Reads standard input and nothing else. A file reaches it through `<`,
 * which costs no process; `cat thing | count` would work too and spends a
 * second one for nothing, which on this machine is a real price.
 *
 * Naming one field prints that number alone, because a number on its own
 * is what anything downstream can use, and a sentence is what a person
 * can. Both are worth having and they are not the same output.
 */
#include "modlib.h"

#define CHUNK 128

typedef struct { char buf[CHUNK]; } count_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    count_statics_t *st = (count_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        /* Silence here is how four missing bytes cost an afternoon: the
           module returned -2, the shell reported nothing, and the command
           simply produced no output. */
        m_say(env, RV9_STDERR, "count: static_size in build.conf is too small\n");
        return -2;
    }

    char want[8];
    want[0] = '\0';
    if (env->arg != NULL) m_word(env->arg, want, sizeof(want));

    uint32_t lines = 0, words = 0, bytes = 0;
    bool in_word = false;

    for (;;) {
        int n = env->read(RV9_STDIN, st->buf, CHUNK);
        if (n <= 0) break;

        for (int i = 0; i < n; i++) {
            char c = st->buf[i];
            bytes++;
            if (c == '\n') lines++;

            bool space = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
            if (space)            in_word = false;
            else if (!in_word)  { in_word = true; words++; }
        }
    }

    /*
     * A last line with no newline on the end is still a line. Counting
     * only the newlines reports one fewer than anybody can see, which is
     * the kind of small wrongness that makes a tool untrustworthy.
     */
    if (bytes > 0 && in_word) lines++;

    if (m_eq(want, "lines")) { m_num(env, RV9_STDOUT, (int32_t)lines); }
    else if (m_eq(want, "words")) { m_num(env, RV9_STDOUT, (int32_t)words); }
    else if (m_eq(want, "bytes")) { m_num(env, RV9_STDOUT, (int32_t)bytes); }
    else if (want[0] != '\0') {
        m_say(env, RV9_STDERR, "count: say lines, words or bytes\n");
        return -3;
    } else {
        m_num(env, RV9_STDOUT, (int32_t)lines);
        m_say(env, RV9_STDOUT, " lines, ");
        m_num(env, RV9_STDOUT, (int32_t)words);
        m_say(env, RV9_STDOUT, " words, ");
        m_num(env, RV9_STDOUT, (int32_t)bytes);
        m_say(env, RV9_STDOUT, " bytes");
    }
    m_say(env, RV9_STDOUT, "\n");
    return 0;
}
