/*
 * log -- what the machine has been saying.
 *
 *   log                    everything still held
 *   log | last 20          the tail
 *   log | match error      only the trouble
 *
 * Until this existed, the board's own account of itself went out of the
 * USB port and was gone -- so the answer to "why did that fail" lived on
 * a wire, and reading it meant a cable and being in the room. On a
 * machine whose whole point is that you work with it over the network,
 * that was the one thing you could not do remotely.
 *
 * It writes to standard output like anything else, so the filters do the
 * rest: there is no reason for this to grow options for searching or
 * counting when `match` and `count` already exist and already compose.
 */
#include "modlib.h"

typedef struct { rv9_sys_log_t q; } log_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    log_statics_t *st = (log_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        m_say(env, RV9_STDERR, "log: static_size in build.conf is too small\n");
        return -2;
    }

    uint32_t from = 0;
    for (;;) {
        st->q.from = from;
        st->q.want = sizeof(st->q.text);

        int n = env->sysinfo(RV9_SYS_LOG, &st->q, sizeof(st->q));
        if (n <= 0) break;

        env->write(RV9_STDOUT, st->q.text, st->q.got);
        from += st->q.got;

        /*
         * The ring keeps filling while this reads it. Stopping at what was
         * held when we started would be neater and would hide the last few
         * lines, which on a machine you are reading the log of are the
         * ones you want most.
         */
        if (from >= st->q.held) break;
    }

    if (from == 0) m_say(env, RV9_STDERR, "log: nothing kept\n");
    return 0;
}
