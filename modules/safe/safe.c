/*
 * safe -- safe mode, asked about or asked for.
 *
 *   safe          whether this run is in safe mode, and why, and for how long
 *   safe on       restart in safe mode, and stay there until `safe off`
 *   safe off      restart normally, the count of bad ends cleared
 *
 * Safe mode runs the core and nothing a crash could have come from: the
 * console, SSH and rshd, the network and peer discovery, the store's
 * modules, and /f0/essential. Not the modules on /f0 and /sd0, /f0/startup,
 * httpd, execd or the self-tests. A board goes there by itself after three
 * bad ends in a row (a panic, a watchdog, a brownout), and finds its own
 * way back: after a healthy hour it tries a normal boot, and if that fails
 * waits four times as long, up to a day. Put there by hand it stays.
 * Design section 82.
 */
#include "modlib.h"

typedef struct { rv9_sys_health_t h; } safe_statics_t;

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 20) return 2;   /* RV9_SYS_HEALTH */

    safe_statics_t *st = (safe_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return 2;

    const char *a = env->arg ? env->arg : "";
    if (m_eq(a, "on") || m_eq(a, "off")) {
        m_say(env, RV9_STDOUT, m_eq(a, "on") ? "restarting in safe mode\n"
                                             : "restarting normally\n");
        st->h.op = m_eq(a, "on") ? RV9_HEALTH_SAFE : RV9_HEALTH_NORMAL;
        env->sysinfo(RV9_SYS_HEALTH, &st->h, sizeof(st->h));
        m_say(env, RV9_STDERR, "safe: the restart did not happen\n");
        return 1;
    }
    if (a[0]) {
        m_say(env, RV9_STDERR, "usage: safe [on|off]\n");
        return 2;
    }

    st->h.op = RV9_HEALTH_ASK;
    if (env->sysinfo(RV9_SYS_HEALTH, &st->h, sizeof(st->h)) != 1) {
        m_say(env, RV9_STDERR, "safe: this firmware has no safe mode\n");
        return 1;
    }
    if (!st->h.safe) {
        m_say(env, RV9_STDOUT, st->h.trial ? "normal, trying its way out of safe mode\n"
                                           : "normal\n");
        if (st->h.fails) {
            m_num(env, RV9_STDOUT, st->h.fails);
            m_say(env, RV9_STDOUT, " bad end(s) in a row; at three the next boot is safe mode\n");
        }
        return 0;
    }
    m_say(env, RV9_STDOUT, "SAFE MODE: ");
    if (st->h.manual) {
        m_say(env, RV9_STDOUT, "put here by hand; `safe off` to leave\n");
    } else {
        m_num(env, RV9_STDOUT, st->h.fails);
        m_say(env, RV9_STDOUT, " bad ends in a row (`boots`); a normal boot is tried in ");
        m_num(env, RV9_STDOUT, (int32_t)(st->h.retry_in_s / 60));
        m_say(env, RV9_STDOUT, " min, or `safe off` now\n");
    }
    return 3;
}
