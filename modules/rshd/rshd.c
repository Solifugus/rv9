/*
 * rshd -- a shell over the network.
 *
 * There is almost nothing here, which is the point. A connection is a
 * path; the shell reads stdin and writes stdout and does not care what
 * they are; a child inherits its parent's paths. So serving a shell over
 * the network is: open the connection, point stdin and stdout at it, fork
 * the shell, put them back.
 *
 * No terminal handling, no protocol, no knowledge of sockets. The pieces
 * that make this short were all built for other reasons.
 *
 *   connect with:  nc <board-ip> 2300
 *
 * This is not secure and does not pretend to be: anyone who can reach the
 * port gets a shell. It is for a workbench LAN. SSH is the eventual
 * answer and is a real piece of work -- key exchange, a cipher, host
 * keys -- rather than something to bolt on here.
 */
#include "modlib.h"

#define LISTEN_PATH "/n0/listen/2300"
#define SAVE_IN     5
#define SAVE_OUT    6
/*
 * And somewhere to park standard error, which this daemon used to leave
 * pointing at the console.
 *
 * Every "cannot open", every usage line, every refusal a tool wrote to
 * stderr went to the board's log instead of to the person who typed the
 * command -- so over a network session a failing command produced a
 * status and no explanation. A session's errors belong in the session.
 */
#define SAVE_ERR    7

typedef struct {
    uint32_t sessions;
} rshd_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    rshd_statics_t *st = (rshd_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    for (;;) {
        /* Blocks until somebody connects, or times out and we listen
           again. Opening a path that is not ready yet is not a special
           case. */
        int c = env->open(LISTEN_PATH, RV9_MODE_RW);
        if (c < 0) continue;

        st->sessions++;

        /* Park our own console, point the standard paths at the
           connection, and hand them to a shell. */
        env->dup2(RV9_STDIN, SAVE_IN);
        env->dup2(RV9_STDOUT, SAVE_OUT);
        env->dup2(RV9_STDERR, SAVE_ERR);
        env->dup2(c, RV9_STDIN);
        env->dup2(c, RV9_STDOUT);
        env->dup2(c, RV9_STDERR);

        int pid = env->fork_arg("shell", 8, 0);
        if (pid >= 0) {
            int status = 0;
            env->wait(pid, &status, 3600000);
        }

        /* Put the console back, whatever happened to the session. */
        env->dup2(SAVE_IN, RV9_STDIN);
        env->dup2(SAVE_OUT, RV9_STDOUT);
        env->dup2(SAVE_ERR, RV9_STDERR);
        env->close(SAVE_IN);
        env->close(SAVE_OUT);
        env->close(SAVE_ERR);
        env->close(c);

        m_say(env, RV9_STDERR, "rshd: session ended\n");
    }

    return 0;
}
