/*
 * taps -- watch the touch panel.
 *
 *     taps            until stopped
 *     taps 20         for twenty seconds
 *
 * Prints one line per event, which is the whole of what it does. It exists
 * because a stream of events is invisible otherwise: a touch driver that
 * reports nothing and a touch driver that reports the wrong coordinates
 * look identical from the outside, and the only way to tell is to put a
 * finger on the glass and read what came out.
 *
 * It is also the answer to "is the mirroring right": press the top-left
 * corner and the numbers should be small, not large.
 */
#include "rv9/module.h"
#include "modlib.h"

typedef struct {
    uint32_t seen;
} taps_statics_t;

static const char *kind_name(uint8_t kind)
{
    switch (kind) {
    case RV9_TOUCH_DOWN: return "down";
    case RV9_TOUCH_MOVE: return "move";
    case RV9_TOUCH_UP:   return "up  ";
    default:             return "?   ";
    }
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 13) return -1;

    taps_statics_t *st = (taps_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;
    st->seen = 0;

    uint32_t secs = 0;
    if (env->arg != NULL) secs = m_num_parse(env->arg, 0);

    int tp = env->open("/touch", RV9_MODE_READ);
    if (tp < 0) {
        m_say(env, RV9_STDERR, "taps: no /touch on this machine\n");
        return 1;
    }

    /* Its own idea of the glass, so the numbers mean something. */
    rv9_physical_t ph;
    if (env->getstat(tp, RV9_GS_PHYSICAL, &ph) == 0) {
        m_say(env, RV9_STDOUT, "touch over ");
        m_num(env, RV9_STDOUT, (int32_t)ph.width_px);
        m_say(env, RV9_STDOUT, "x");
        m_num(env, RV9_STDOUT, (int32_t)ph.height_px);
        m_say(env, RV9_STDOUT, " px");
        if (ph.kind == RV9_PHYS_FIXED) {
            m_say(env, RV9_STDOUT, ", ");
            m_num(env, RV9_STDOUT, (int32_t)(ph.width_um / 1000));
            m_say(env, RV9_STDOUT, "x");
            m_num(env, RV9_STDOUT, (int32_t)(ph.height_um / 1000));
            m_say(env, RV9_STDOUT, " mm");
        }
        m_say(env, RV9_STDOUT, "\n");
    }

    m_say(env, RV9_STDOUT, "touch the glass; ctrl-c or `kill` to stop\n");

    uint64_t until = secs ? env->time_us() + (uint64_t)secs * 1000000u : 0;

    for (;;) {
        if (until && env->time_us() >= until) break;

        /* Four at a time: a drag with two fingers produces events faster
           than one read each would collect them. */
        rv9_touch_event_t ev[4];
        int n = env->read(tp, ev, sizeof(ev));
        if (n < 0) break;

        int count = n / (int)sizeof(rv9_touch_event_t);
        for (int i = 0; i < count; i++) {
            st->seen++;
            m_say(env, RV9_STDOUT, kind_name(ev[i].kind));
            m_say(env, RV9_STDOUT, "  finger ");
            m_num(env, RV9_STDOUT, (int32_t)ev[i].id);
            m_say(env, RV9_STDOUT, "  at ");
            m_numpad(env, RV9_STDOUT, (int32_t)ev[i].x, 4);
            m_say(env, RV9_STDOUT, ",");
            m_numpad(env, RV9_STDOUT, (int32_t)ev[i].y, 4);
            m_say(env, RV9_STDOUT, "  ");
            m_num(env, RV9_STDOUT, (int32_t)ev[i].at_ms);
            m_say(env, RV9_STDOUT, " ms\n");
        }
    }

    env->close(tp);

    m_say(env, RV9_STDOUT, "taps: ");
    m_num(env, RV9_STDOUT, (int32_t)st->seen);
    m_say(env, RV9_STDOUT, " event(s)\n");
    return 0;
}
