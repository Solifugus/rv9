/*
 * noise -- make the system busy in the ways that hurt.
 *
 * Not arbitrary busywork: it does the two things that hold the locks a
 * real-time process might want. Writing and closing a file on /f0 holds
 * the I/O manager's lock while flash is erased and rewritten -- tens of
 * milliseconds. Forking a process holds the process table's lock.
 *
 * This exists to be run underneath iolat, so that "I/O is bounded" is
 * something measured while the system is under exactly the load that would
 * break it, rather than while it is idle and everything looks fine.
 */
#include "modlib.h"

#define ROUNDS 40

typedef struct { char buf[256]; } noise_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 11) return -1;

    noise_statics_t *st = (noise_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    for (uint32_t i = 0; i < sizeof(st->buf); i++) st->buf[i] = (char)('a' + i % 26);

    for (uint32_t n = 0; n < ROUNDS; n++) {
        /* Flash: the long lock hold. */
        int f = env->open("/f0/noise.tmp", RV9_MODE_WRITE | RV9_MODE_CREATE);
        if (f >= 0) {
            env->write(f, st->buf, sizeof(st->buf));
            env->close(f);
        }

        /* The process table: allocation and bookkeeping under its lock. */
        int pid = env->fork_arg("echo", 8, 0);
        if (pid >= 0) env->wait(pid, 0, 5000);
    }

    env->remove("/f0/noise.tmp");
    return 0;
}
