/*
 * authkey -- trust a public key for SSH login.
 *
 *   authkey           paste one key line; it is appended
 *   authkey list      show what is trusted
 *   authkey clear     trust nothing (password login still works)
 *
 * The key is read from stdin rather than taken as an argument, because a
 * key line runs to a couple of hundred characters and a process argument
 * is sixty-four. Pasting it at a prompt is the natural gesture anyway --
 * it is what the line discipline is for.
 *
 * Keys live in /f0/authkeys, in the same format as an ordinary
 * authorized_keys file, so the line from your own ~/.ssh is the line that
 * goes here.
 *
 * Ed25519 will be accepted by this command and rejected at login: mbedTLS
 * as ESP-IDF ships it has no EdDSA at all. Use an ecdsa-sha2-nistp256 key
 * (ssh-keygen -t ecdsa -b 256), or RSA.
 */
#include "modlib.h"

#define KEYS_PATH "/f0/authkeys"
#define LINE_MAX  512

typedef struct {
    char line[LINE_MAX];
} authkey_statics_t;

static void show(const rv9_mod_env_t *env, authkey_statics_t *st)
{
    int p = env->open(KEYS_PATH, RV9_MODE_READ);
    if (p < 0) { m_say(env, RV9_STDOUT, "no keys are trusted\n"); return; }

    for (;;) {
        int n = env->read(p, st->line, sizeof(st->line) - 1);
        if (n <= 0) break;
        st->line[n] = '\0';
        m_say(env, RV9_STDOUT, st->line);
    }
    env->close(p);
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    authkey_statics_t *st = (authkey_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    if (env->arg && m_eq(env->arg, "list")) { show(env, st); return 0; }

    if (env->arg && m_eq(env->arg, "clear")) {
        int rc = env->remove(KEYS_PATH);
        m_say(env, RV9_STDOUT, rc < 0 ? "nothing to clear\n"
                                      : "no keys are trusted now\n");
        return 0;
    }

    m_say(env, RV9_STDOUT, "paste one public key line:\n");

    int n = env->read(RV9_STDIN, st->line, sizeof(st->line) - 1);
    if (n <= 0) { m_say(env, RV9_STDOUT, "nothing read\n"); return -3; }

    /* The line discipline hands back the newline; we add our own. */
    while (n > 0 && (st->line[n - 1] == '\n' || st->line[n - 1] == '\r')) n--;
    st->line[n] = '\0';

    if (n < 16) { m_say(env, RV9_STDOUT, "that is not a key\n"); return -4; }

    /*
     * Append. Opening without CREATE so an existing file is not truncated
     * -- CREATE means "make it, empty" here -- and only creating one when
     * there was none.
     */
    int p = env->open(KEYS_PATH, RV9_MODE_WRITE);
    if (p < 0) p = env->open(KEYS_PATH, RV9_MODE_WRITE | RV9_MODE_CREATE);
    if (p < 0) {
        m_say(env, RV9_STDOUT, KEYS_PATH ": cannot open\n");
        return -5;
    }

    env->seek(p, 0, RV9_SEEK_END);
    env->write(p, st->line, (uint32_t)n);
    env->write(p, "\n", 1);
    env->close(p);

    m_say(env, RV9_STDOUT, "trusted; try logging in with that key\n");
    return 0;
}
