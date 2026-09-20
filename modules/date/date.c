/*
 * date -- what time it is.
 *
 *   date          2026-09-20 04:11:07 UTC  (asked the network 312s ago)
 *   date epoch    1789891867
 *
 * This board has no clock that survives power, so the time is acquired
 * from the network and is simply unknown until it answers. When it has
 * not, this says so rather than printing 1970 -- a wrong time sends
 * somebody hunting in the wrong hour, which is worse than no time at all.
 *
 * UTC, because where you are standing is not something the board knows.
 *
 * WHY THE CALENDAR MATHS IS HERE
 *
 * A module links against no runtime library, so there is no gmtime() to
 * call -- the same rule that turned a 64-bit divide in `uptime` into a
 * linker error. The civil-from-days conversion below is the standard one:
 * shift the year to start in March so the leap day lands at the end,
 * after which the month lengths follow a pattern with no table.
 */
#include "modlib.h"

typedef struct { rv9_sys_clock_t c; } date_statics_t;

/* Days since 1970-01-01 -> year, month, day. Valid for any date this
   machine will ever see. */
static void civil_from_days(int32_t z, int32_t *y, uint32_t *m, uint32_t *d)
{
    z += 719468;
    int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);            /* [0, 146096] */
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t  yr  = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100); /* [0, 365] */
    uint32_t mp  = (5 * doy + 2) / 153;                     /* [0, 11] */
    uint32_t dy  = doy - (153 * mp + 2) / 5 + 1;            /* [1, 31] */
    uint32_t mn  = mp + (mp < 10 ? 3 : (uint32_t)-9);       /* [1, 12] */

    *y = yr + (mn <= 2);
    *m = mn;
    *d = dy;
}

static void two(const rv9_mod_env_t *env, uint32_t v)
{
    char out[3];
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
    out[2] = '\0';
    m_say(env, RV9_STDOUT, out);
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    date_statics_t *st = (date_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) {
        m_say(env, RV9_STDERR, "date: static_size in build.conf is too small\n");
        return -2;
    }

    if (env->sysinfo(RV9_SYS_CLOCK, &st->c, sizeof(st->c)) < 1) {
        m_say(env, RV9_STDERR, "date: this build keeps no clock\n");
        return -3;
    }

    if (!st->c.set) {
        m_say(env, RV9_STDERR, "date: the time is not known -- the network "
                               "has not answered yet\n");
        return -4;
    }

    char want[8];
    want[0] = '\0';
    if (env->arg != NULL) m_word(env->arg, want, sizeof(want));

    if (m_eq(want, "epoch")) {
        m_num(env, RV9_STDOUT, (int32_t)st->c.epoch);
        m_say(env, RV9_STDOUT, "\n");
        return 0;
    }

    uint32_t secs = st->c.epoch % 86400u;
    int32_t  days = (int32_t)(st->c.epoch / 86400u);

    int32_t  year = 0;
    uint32_t mon = 0, day = 0;
    civil_from_days(days, &year, &mon, &day);

    m_num(env, RV9_STDOUT, year);
    m_say(env, RV9_STDOUT, "-");
    two(env, mon);
    m_say(env, RV9_STDOUT, "-");
    two(env, day);
    m_say(env, RV9_STDOUT, " ");
    two(env, secs / 3600u);
    m_say(env, RV9_STDOUT, ":");
    two(env, (secs / 60u) % 60u);
    m_say(env, RV9_STDOUT, ":");
    two(env, secs % 60u);
    m_say(env, RV9_STDOUT, " UTC  (asked the network ");
    m_num(env, RV9_STDOUT, (int32_t)st->c.age_s);
    m_say(env, RV9_STDOUT, "s ago)\n");
    return 0;
}
