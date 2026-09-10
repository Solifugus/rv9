/*
 * filetest -- create a file, write it, read it back, check it.
 *
 * Deliberately paranoid, like hello was for the loader: this module's job
 * is to prove RBF works, so it verifies rather than assumes. It knows
 * nothing about sectors, bitmaps or segment lists.
 */
#include "modlib.h"

#define TEST_PATH "/r0/notes.txt"
#define PATTERN_LEN 600     /* deliberately more than one 512-byte sector */

typedef struct {
    char out[PATTERN_LEN];
    char in[PATTERN_LEN];
} filetest_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 7) return -1;

    filetest_statics_t *st = (filetest_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    /* A repeating pattern that crosses a sector boundary, so the segment
       walk and the read-modify-write path both get exercised. */
    for (int i = 0; i < PATTERN_LEN; i++) {
        st->out[i] = (char)('a' + (i % 26));
    }

    int p = env->open(TEST_PATH, RV9_MODE_WRITE | RV9_MODE_CREATE);
    if (p < 0) { m_say(env, RV9_STDOUT, "create failed\n"); return -3; }

    int wrote = env->write(p, st->out, PATTERN_LEN);
    env->close(p);

    if (wrote != PATTERN_LEN) {
        m_say(env, RV9_STDOUT, "short write: ");
        m_num(env, RV9_STDOUT, wrote);
        m_say(env, RV9_STDOUT, "\n");
        return -4;
    }

    p = env->open(TEST_PATH, RV9_MODE_READ);
    if (p < 0) { m_say(env, RV9_STDOUT, "reopen failed\n"); return -5; }

    int got = env->read(p, st->in, PATTERN_LEN);
    env->close(p);

    if (got != PATTERN_LEN) {
        m_say(env, RV9_STDOUT, "short read: ");
        m_num(env, RV9_STDOUT, got);
        m_say(env, RV9_STDOUT, "\n");
        return -6;
    }

    for (int i = 0; i < PATTERN_LEN; i++) {
        if (st->in[i] != st->out[i]) {
            m_say(env, RV9_STDOUT, "mismatch at byte ");
            m_num(env, RV9_STDOUT, i);
            m_say(env, RV9_STDOUT, "\n");
            return -7;
        }
    }

    m_say(env, RV9_STDOUT, "filetest: wrote and verified ");
    m_num(env, RV9_STDOUT, PATTERN_LEN);
    m_say(env, RV9_STDOUT, " bytes to " TEST_PATH "\n");
    return 0;
}
