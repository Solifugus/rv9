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
    uint32_t said_no_password;
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
             * One of these is permanent and the rest are not.
             *
             * Another sshd already serving the device will not go away by
             * trying again, so stop. Anything else is one connection that
             * went wrong -- a client that hung up during the handshake, a
             * port scan, a timeout -- and a server that retired over that
             * would be a server anyone could turn off from across the
             * network by connecting and leaving.
             *
             * EXISTS used to be reached another way too: a background job
             * started over SSH kept the terminal after its session ended,
             * this read that as a second sshd, and retired for good. The
             * session is now hung up when its shell ends (below), so a
             * leftover job no longer holds the device.
             */
            if (c == -RV9_IOE_EXISTS) {
                m_say(env, RV9_STDERR, "sshd: already running\n");
                return -3;
            }

            /*
             * No password is not permanent either -- somebody can run
             * `passwd` -- and stopping over it meant a reboot before the
             * first login. Said once, then waited out quietly.
             */
            if (c == -RV9_IOE_MODE) {
                if (!st->said_no_password) {
                    m_say(env, RV9_STDERR, "sshd: no login password -- run "
                                           "`passwd`; waiting for one\n");
                    st->said_no_password = 1;
                }
                env->sleep_ms(5000);
                continue;
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

        /*
         * The session ends when its shell does -- not when the last path to
         * the terminal closes, which may be never. Anything the shell left
         * running in the background keeps running, and finds its terminal
         * gone; the next client gets a session of its own.
         */
        uint32_t zero = 0;
        env->setstat(c, RV9_SS_HANGUP, &zero);

        env->dup2(SAVE_IN, RV9_STDIN);
        env->dup2(SAVE_OUT, RV9_STDOUT);
        env->close(SAVE_IN);
        env->close(SAVE_OUT);
        env->close(c);

        m_say(env, RV9_STDERR, "sshd: session ended\n");
    }

    return 0;
}
