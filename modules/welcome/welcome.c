/*
 * welcome -- say hello to the people the camera knows.
 *
 *   detect who -p &        what the camera sees, on /pub0/SEEN
 *   welcome [-u] [minutes]
 *
 * Watches /pub0/SEEN, and when a known face is the main one in two
 * pictures running, says "Hello, Matthew." through `say` -- once, and not
 * again for that person for ten minutes (or `minutes`). With -u it greets
 * a face nobody knows as well, after three pictures, at most once a
 * minute. Until stopped.
 *
 * It never looks at a picture: what detect publishes is all it needs, and
 * that is the point of publishing it. A program that steered towards a
 * face would read the same sixteen words.
 */
#include "modlib.h"

#define TICK_MS    500
#define REMEMBER   8              /* people greeted, and when */
#define NAME       32
#define SAME_KNOWN 2              /* pictures running before hello */
#define SAME_ANON  3
#define ANON_US    60000000ull

typedef struct {
    char     name[NAME];
    uint64_t at_us;
} greeted_t;

typedef struct {
    uint8_t   snap[sizeof(rv9_pub_t) + 16 * sizeof(int32_t)];
    greeted_t greeted[REMEMBER];
    char      last[NAME];         /* the main face in the last picture */
    char      text[64];
    char      word[16];
} welcome_statics_t;

static int same(const char *a, const char *b)
{
    for (int i = 0; i < NAME; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

static void copy(char *to, const char *from)
{
    int i = 0;
    for (; i < NAME - 1 && from[i]; i++) to[i] = from[i];
    to[i] = '\0';
}

/* By forking `say`, and waiting: the voice is say's to make. */
static void speak(const rv9_mod_env_t *env, const char *text)
{
    int pid = env->fork_arg("say", 8, text);
    if (pid < 0) {
        m_say(env, RV9_STDERR, "welcome: cannot start say\n");
        return;
    }
    int status = 0;
    env->wait(pid, &status, 30000);
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == 0 || env->abi_version < 8) return -1;
    welcome_statics_t *st = (welcome_statics_t *)env->statics;
    if (st == 0 || env->statics_size < sizeof(*st)) return -2;

    const char *rest = env->arg ? env->arg : "";
    rest = m_word(rest, st->word, sizeof(st->word));
    int strangers = m_eq(st->word, "-u");
    if (strangers) rest = m_word(rest, st->word, sizeof(st->word));
    uint32_t minutes = st->word[0] ? m_num_parse(st->word, 0) : 10;
    if (minutes == 0) {
        m_say(env, RV9_STDERR, "usage: welcome [-u] [minutes]\n");
        return 1;
    }

    int p = env->open("/pub0/SEEN", RV9_MODE_READ);
    if (p < 0) {
        m_say(env, RV9_STDERR, "welcome: /pub0/SEEN: cannot open\n");
        return 2;
    }
    m_say(env, RV9_STDOUT, "welcome: watching /pub0/SEEN (detect who -p)\n");

    rv9_pub_wait_t w = { .seq = 0, .timeout_ms = 0 };
    env->getstat(p, RV9_PUB_GS_WAIT, &w);

    int run = 0;                  /* pictures running with the same main face */
    uint64_t anon_at = 0;
    st->last[0] = '\0';

    for (;;) {
        if (env->signals_take() & RV9_SIG_STOP) break;
        w.timeout_ms = TICK_MS;
        int r = env->getstat(p, RV9_PUB_GS_WAIT, &w);
        if (r == -RV9_IOE_TIMEOUT) continue;
        if (r < 0) break;
        int got = env->read(p, st->snap, sizeof(st->snap));
        if (got < (int)sizeof(st->snap)) continue;     /* outrun, or short */

        const int32_t *v = (const int32_t *)(st->snap + sizeof(rv9_pub_t));
        if (v[0] != 2 /* faces */ || v[1] == 0) { run = 0; st->last[0] = '\0'; continue; }

        /* The name, or "" for a face nobody knows -- but only when detect
           was asked who (v[7] is -1 otherwise, and then there is no one
           to greet). */
        char name[NAME];
        const char *pub = (const char *)&v[8];
        int i = 0;
        for (; i < NAME - 1 && pub[i]; i++) name[i] = pub[i];
        name[i] = '\0';
        if (v[7] == -1) continue;

        run = same(name, st->last) ? run + 1 : 1;
        copy(st->last, name);
        uint64_t now = env->time_us();   /* microseconds: no 64-bit division here */

        if (name[0] && run >= SAME_KNOWN) {
            int k = 0, oldest = 0;
            for (; k < REMEMBER; k++) {
                if (same(st->greeted[k].name, name)) break;
                if (st->greeted[k].at_us < st->greeted[oldest].at_us) oldest = k;
            }
            if (k < REMEMBER &&
                now - st->greeted[k].at_us < (uint64_t)minutes * 60000000u) {
                continue;                 /* said hello already */
            }
            if (k == REMEMBER) k = oldest;
            copy(st->greeted[k].name, name);
            st->greeted[k].at_us = now;

            /* "Hello, Matthew." from "matthew". */
            const char *hi = "Hello, ";
            int n = 0;
            while (*hi) st->text[n++] = *hi++;
            for (int j = 0; name[j] && n < (int)sizeof(st->text) - 2; j++) {
                char c = name[j];
                if (j == 0 && c >= 'a' && c <= 'z') c = (char)(c - 32);
                st->text[n++] = c;
            }
            st->text[n++] = '.';
            st->text[n] = '\0';
            m_say(env, RV9_STDOUT, "welcome: ");
            m_say(env, RV9_STDOUT, st->text);
            m_say(env, RV9_STDOUT, "\n");
            speak(env, st->text);
            w.timeout_ms = 0;                       /* skip what came meanwhile */
            env->getstat(p, RV9_PUB_GS_WAIT, &w);
            run = 0;
        } else if (!name[0] && strangers && run >= SAME_ANON &&
                   (anon_at == 0 || now - anon_at >= ANON_US)) {
            anon_at = now;
            m_say(env, RV9_STDOUT, "welcome: somebody it does not know\n");
            speak(env, "Hello. I do not think we have met.");
            w.timeout_ms = 0;
            env->getstat(p, RV9_PUB_GS_WAIT, &w);
            run = 0;
        }
    }
    env->close(p);
    return 0;
}
