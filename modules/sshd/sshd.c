/*
 * sshd -- a shell over SSH.
 *
 * This is rshd with one string changed, which is the whole argument for
 * where the protocol was put. Key exchange, a host key, a cipher and
 * authentication all happen below the path: `/ssh0` is a character device
 * like any other, so serving a shell over it is still open the connection,
 * point stdin and stdout at it, fork the shell, put them back.
 *
 *   connect with:  ssh <user>@<board-ip>
 *
 * Any user name will do -- RV-9 has processes, not users -- and the
 * password is the one set with `passwd`. The host key fingerprint is in
 * the boot log and from `passwd show`, so the client's question about an
 * unknown host has an answer that did not come over the network.
 */
#include "modlib.h"

#define LISTEN_PATH "/ssh0"
#define SAVE_IN     5
#define SAVE_OUT    6

typedef struct {
    uint32_t sessions;
} sshd_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    sshd_statics_t *st = (sshd_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    for (;;) {
        /* Blocks through the whole handshake. What comes back is a path
           with somebody authenticated on the other end of it. */
        int c = env->open(LISTEN_PATH, RV9_MODE_RW);
        if (c < 0) {
            /*
             * Two of these are permanent and the rest are not.
             *
             * No password and another sshd already listening will not get
             * better by trying again, so stop. Anything else is one
             * connection that went wrong -- a client that hung up during
             * the handshake, a port scan, a timeout -- and a server that
             * retired over that would be a server anyone could turn off
             * from across the network by connecting and leaving.
             */
            if (c == -RV9_IOE_EXISTS) {
                m_say(env, RV9_STDERR, "sshd: already running\n");
                return -3;
            }
            if (c == -RV9_IOE_MODE) {
                m_say(env, RV9_STDERR,
                      "sshd: no login password -- run `passwd` first\n");
                return -4;
            }

            /* Pause, so a condition that fails instantly cannot spin. */
            env->sleep_ms(1000);
            continue;
        }

        st->sessions++;

        env->dup2(RV9_STDIN, SAVE_IN);
        env->dup2(RV9_STDOUT, SAVE_OUT);
        env->dup2(c, RV9_STDIN);
        env->dup2(c, RV9_STDOUT);

        int pid = env->fork_arg("shell", 8, 0);
        if (pid >= 0) {
            int status = 0;
            env->wait(pid, &status, RV9_WAIT_FOREVER);
        } else {
            /* Silence here made a failed fork look exactly like a session
               that ended: the client connected, got nothing, and went. */
            m_say(env, RV9_STDERR, "sshd: cannot start a shell, code ");
            m_num(env, RV9_STDERR, pid);
            m_say(env, RV9_STDERR, "\n");
        }

        env->dup2(SAVE_IN, RV9_STDIN);
        env->dup2(SAVE_OUT, RV9_STDOUT);
        env->close(SAVE_IN);
        env->close(SAVE_OUT);
        env->close(c);

        m_say(env, RV9_STDERR, "sshd: session ended\n");
    }

    return 0;
}
