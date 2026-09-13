/*
 * watch -- print a published value every time it changes.
 *
 * The observing half of a publication, and the shape R9's reactive layer
 * has: block until the value moves, take a coherent snapshot, act, block
 * again. No polling loop, no sleep guessed at, no missed change -- the
 * wait is against a sequence number rather than an edge, so a publication
 * that lands between two waits is still seen.
 *
 *     watch CONTROL            until interrupted
 *     watch CONTROL 20         twenty changes, then stop
 *
 * Publications that arrive faster than this can print are coalesced, which
 * is exactly what a supervisor wants: it is looking for the current state
 * of the machine, not for a transcript of how it got there.
 */
#include "modlib.h"

#define DEV        "/pub0/"
#define MAX_WORDS  12
#define TICK_MS    500      /* how often to notice a stop request */

typedef struct {
    char    path[48];
    uint8_t snap[sizeof(rv9_pub_t) + MAX_WORDS * sizeof(int32_t)];
} watch_statics_t;

static const char *fault_word(uint8_t f)
{
    if (f == RV9_FAULT_STACK)    return "STACK";
    if (f == RV9_FAULT_KILLED)   return "killed";
    if (f == RV9_FAULT_DEADLINE) return "DEADLINE";
    if (f == RV9_FAULT_RUNAWAY)  return "RUNAWAY";
    return "faulted";
}

static void show(const rv9_mod_env_t *env, const rv9_pub_t *head,
                 const int32_t *val, uint64_t now_us)
{
    m_numpad(env, RV9_STDOUT, (int32_t)head->seq, 7);

    /* How old the observation is, not how old the publication is. They
       differ by however long the publisher spent computing, and the second
       number is the one a reactive layer must not confuse for the first. */
    m_numpad(env, RV9_STDOUT, (int32_t)m_age_ms(now_us, head->stamp_us), 7);

    uint32_t words = head->len / sizeof(int32_t);
    if (words > MAX_WORDS) words = MAX_WORDS;
    for (uint32_t i = 0; i < words; i++) {
        m_numpad(env, RV9_STDOUT, val[i], 10);
    }
    m_say(env, RV9_STDOUT, "\n");
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 11) return -1;

    watch_statics_t *st = (watch_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    if (env->arg == NULL || env->arg[0] == '\0') {
        m_say(env, RV9_STDOUT, "usage: watch <name> [changes]\n"
                               "  blocks on /pub0/<name>; see 'pubs'\n");
        return -3;
    }

    char name[RV9_PUB_MAX_NAME];
    const char *rest = m_word(env->arg, name, sizeof(name));
    uint32_t want = m_num_parse(rest, 0);

    const char *dev = DEV;
    uint32_t i = 0;
    while (*dev && i < sizeof(st->path) - 1) st->path[i++] = *dev++;
    for (uint32_t k = 0; name[k] && i < sizeof(st->path) - 1; k++) {
        st->path[i++] = name[k];
    }
    st->path[i] = '\0';

    int p = env->open(st->path, RV9_MODE_READ);
    if (p < 0) {
        m_say(env, RV9_STDOUT, st->path);
        if (p == -RV9_IOE_NOTFOUND) {
            m_say(env, RV9_STDOUT, ": nothing publishes that\n");
        } else {
            m_say(env, RV9_STDOUT, ": cannot open\n");
        }
        return -4;
    }

    m_say(env, RV9_STDOUT, "seq    age_ms values...\n");

    /*
     * Start from the sequence as it stands rather than from zero, so the
     * first line is the next change and not a replay of what is already
     * there. Starting from zero would be equally defensible; this makes
     * `watch` a change log, which is what it is for.
     */
    rv9_pub_wait_t w = { .seq = 0, .timeout_ms = 0 };
    env->getstat(p, RV9_PUB_GS_WAIT, &w);

    uint32_t seen = 0;
    bool faulted = false;
    for (;;) {
        if (env->signals_take() & RV9_SIG_STOP) break;

        w.timeout_ms = TICK_MS;
        int r = env->getstat(p, RV9_PUB_GS_WAIT, &w);
        if (r == -RV9_IOE_TIMEOUT) continue;    /* nothing new; look again */
        if (r < 0) break;

        /*
         * A change that is not a new value: the publisher stopped, and the
         * cell says why. Said once, then keep watching -- a component that
         * is restarted publishes again, and that is worth seeing too.
         */
        rv9_pub_info_t info;
        if (env->getstat(p, RV9_PUB_GS_INFO, &info) == 0) {
            if (info.fault && !faulted) {
                m_say(env, RV9_STDOUT, "  publisher stopped: ");
                m_say(env, RV9_STDOUT, fault_word(info.fault));
                m_say(env, RV9_STDOUT, " (the value above was its last)\n");
                faulted = true;
                continue;
            }
            if (!info.fault) faulted = false;
        }

        int got = env->read(p, st->snap, sizeof(st->snap));
        if (got == -RV9_IOE_WOULDBLOCK) {
            /* Outrun: the publisher moved on before the snapshot was
               taken. Reported rather than hidden -- 'pubs' counts them. */
            m_say(env, RV9_STDOUT, "  (missed one)\n");
            continue;
        }
        if (got < (int)sizeof(rv9_pub_t)) break;

        show(env, (const rv9_pub_t *)st->snap,
             (const int32_t *)(st->snap + sizeof(rv9_pub_t)),
             env->time_us());

        if (want && ++seen >= want) break;
    }

    env->close(p);
    return 0;
}
