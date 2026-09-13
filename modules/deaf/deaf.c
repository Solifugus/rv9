/*
 * deaf -- a process that is asked to stop and does not.
 *
 *   deaf           sleep, ignoring signals
 *   deaf talk      the same, writing a line to its terminal twice a second
 *   deaf listen    the same, reading from its terminal all the while
 *   deaf accept    wait for a connection on port 2399, inside an open
 *
 * `accept` is the case that taught the rest to be careful: a process
 * killed while an open is still waiting for a network connection. The open
 * has made a listening socket that only it can close, so RV-9 must not
 * stop the process there -- it asks the wait to give up, and the kill
 * lands once the socket is gone.
 *
 * It reads its signals, so it has been told, and ignores them. That is the
 * case `kill` exists for: a program that has stopped listening, whether
 * through a bug, a wait that never ends, or simply never having been
 * written to care. Asking is not enough, and something has to be.
 *
 * `talk` and `listen` are for the other thing a background job does to a
 * terminal: use it after the session it came from has ended. Started with
 * `&` over SSH and left behind by `exit`, a talker is in the middle of a
 * write and a listener is waiting on a read that no client will ever
 * answer -- both of them inside the driver of a session that has to be
 * torn down so the next login can have the device. Neither should stop the
 * job, and neither should crash the machine. Errors are ignored, and a
 * failed read is paced so it cannot spin.
 *
 * Ten minutes and then it gives up on its own, so a copy started and
 * forgotten does not outlive the afternoon.
 */
#include "modlib.h"

#define NAP_MS  100
#define NAPS    6000

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 2) return -1;

    bool talk   = (env->arg != NULL && m_eq(env->arg, "talk"));
    bool listen = (env->arg != NULL && m_eq(env->arg, "listen"));

    if (env->arg != NULL && m_eq(env->arg, "accept")) {
        int c = env->open("/n0/listen/2399", RV9_MODE_RW);
        if (c < 0) return -5;           /* could not listen */
        env->close(c);                  /* somebody connected; enough */
        return 0;
    }

    char buf[16];
    uint64_t start = env->time_ms();

    for (uint32_t i = 0; i < NAPS; i++) {
        (void)env->signals_take();       /* heard, and ignored */

        if (listen) {
            if (env->read(RV9_STDIN, buf, sizeof(buf)) <= 0) {
                env->sleep_ms(NAP_MS);
            }
            if (env->time_ms() - start > (uint64_t)NAPS * NAP_MS) break;
            continue;
        }

        if (talk && (i % 5) == 0) {
            m_say(env, RV9_STDOUT, "deaf: still here\n");
        }
        env->sleep_ms(NAP_MS);
    }
    return 0;
}
