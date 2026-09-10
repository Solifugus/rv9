/*
 * nettest -- prove NFM over loopback, with no access point involved.
 *
 * Forks netecho to listen, connects to it through 127.0.0.1, sends a
 * message and checks what comes back. Every byte travels
 * path -> nfm -> lwIP and back, and neither module contains the word
 * socket.
 */
#include "modlib.h"

#define PORT_STR  "8042"
#define MESSAGE   "the network is a path"

typedef struct { char in[64]; } nettest_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 8) return -1;

    nettest_statics_t *st = (nettest_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    int pid = env->fork_arg("netecho", 8, PORT_STR);
    if (pid < 0) { m_say(env, RV9_STDOUT, "cannot start netecho\n"); return -3; }

    /*
     * Retry rather than guess how long the listener needs. A single sleep
     * is a race dressed as a delay: connecting before the listener reaches
     * listen() gets ECONNRESET, which looks exactly like a broken stack.
     */
    int p = -1;
    for (int attempt = 0; attempt < 20 && p < 0; attempt++) {
        env->sleep_ms(100);
        p = env->open("/n0/127.0.0.1/" PORT_STR, RV9_MODE_RW);
    }

    if (p < 0) {
        m_say(env, RV9_STDOUT, "connect failed\n");
        int st2 = 0;
        env->wait(pid, &st2, 5000);
        m_say(env, RV9_STDOUT, "netecho exited with ");
        m_num(env, RV9_STDOUT, st2);
        m_say(env, RV9_STDOUT, "\n");
        return -4;
    }

    uint32_t len = m_len(MESSAGE);
    if ((uint32_t)env->write(p, MESSAGE, len) != len) {
        m_say(env, RV9_STDOUT, "send failed\n");
        env->close(p);
        return -5;
    }

    int got = env->read(p, st->in, sizeof(st->in) - 1);
    env->close(p);

    int status = 0;
    env->wait(pid, &status, 5000);

    if (got != (int)len) {
        m_say(env, RV9_STDOUT, "short reply\n");
        return -6;
    }
    st->in[got] = '\0';
    if (!m_eq(st->in, MESSAGE)) {
        m_say(env, RV9_STDOUT, "reply differed\n");
        return -7;
    }

    m_say(env, RV9_STDOUT, "nettest: echoed '");
    m_say(env, RV9_STDOUT, st->in);
    m_say(env, RV9_STDOUT, "' over loopback\n");
    return 0;
}
