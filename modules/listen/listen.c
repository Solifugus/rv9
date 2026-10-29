/*
 * listen -- hear something said, and write down what it was.
 *
 *   listen [seconds]       default 4, at most 16
 *   listen -f <file>       a recording instead: raw 16 kHz, one channel
 *
 * Records from /au0, hands MIC2 to /stt0 and prints what Whistle heard,
 * then how long each part took. MIC2 rather than both: the recogniser
 * wants one channel, and on this board MIC2 is the more sensitive capsule
 * of the two -- about twice MIC1's level for the same sound.
 *
 * The first use after a restart also reads the 17 MB model, which takes a
 * while and is said.
 */
#include "modlib.h"

#define RATE   16000
#define CHUNK  640               /* stereo frames, 40 ms */

typedef struct {
    int16_t st[CHUNK * 2];
    int16_t mono[CHUNK];
    char    text[256];
    char    name[48];
    rv9_stt_timing_t t;
} listen_statics_t;

static void ms(const rv9_mod_env_t *env, const char *what, uint32_t v)
{
    m_say(env, RV9_STDOUT, what);
    m_num(env, RV9_STDOUT, (int32_t)v);
    m_say(env, RV9_STDOUT, " ms");
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == 0 || env->abi_version < 8) return -1;

    listen_statics_t *st = (listen_statics_t *)env->statics;
    if (st == 0 || env->statics_size < sizeof(*st)) return -2;

    const char *rest = env->arg ? m_word(env->arg, st->name, sizeof(st->name))
                                : "";
    int from_file = m_eq(st->name, "-f");
    if (from_file) rest = m_word(rest, st->name, sizeof(st->name));
    uint32_t secs = 4;
    if (!from_file && st->name[0]) secs = m_num_parse(st->name, 0);
    if (secs == 0 || secs > 16 || (from_file && st->name[0] == '\0')) {
        m_say(env, RV9_STDERR, "usage: listen [seconds] | listen -f <file>\n");
        return 1;
    }

    int stt = env->open("/stt0", RV9_MODE_RW);
    if (stt < 0) {
        m_say(env, RV9_STDERR, "listen: /stt0: cannot open\n");
        return 3;
    }

    if (from_file) {
        int f = env->open(st->name, RV9_MODE_READ);
        if (f < 0) {
            m_say(env, RV9_STDERR, "listen: cannot open the recording\n");
            env->close(stt);
            return 4;
        }
        for (;;) {
            int n = env->read(f, st->mono, sizeof(st->mono));
            if (n <= 0) break;
            if (env->write(stt, st->mono, (uint32_t)n) < n) break;
        }
        env->close(f);
    } else {
        int au = env->open("/au0", RV9_MODE_READ);
        if (au < 0) {
            m_say(env, RV9_STDERR, "listen: /au0: cannot open\n");
            env->close(stt);
            return 4;
        }
        /* The codecs settle for a tenth of a second after they start. */
        for (uint32_t got = 0; got < RATE / 10 * 4; ) {
            int n = env->read(au, st->st, RATE / 10 * 4 - got);
            if (n <= 0) break;
            got += (uint32_t)n;
        }
        m_say(env, RV9_STDOUT, "listening...\n");
        uint32_t frames = secs * RATE;
        while (frames > 0) {
            uint32_t want = frames < CHUNK ? frames : CHUNK;
            int n = env->read(au, st->st, want * 4);
            if (n <= 0) break;
            uint32_t k = (uint32_t)n / 4;
            for (uint32_t i = 0; i < k; i++) st->mono[i] = st->st[2 * i + 1];
            env->write(stt, st->mono, k * 2);
            frames -= k;
        }
        env->close(au);
    }

    if (env->setstat(stt, RV9_STT_SS_DONE, 0) < 0) {
        m_say(env, RV9_STDERR, "listen: nothing to recognise\n");
        env->close(stt);
        return 5;
    }
    m_say(env, RV9_STDOUT, "thinking...\n");

    int n = env->read(stt, st->text, sizeof(st->text) - 1);
    if (n < 0) {
        m_say(env, RV9_STDERR, "listen: the recogniser failed; is "
                               "/sd0/whistle.cact there?\n");
        env->close(stt);
        return 6;
    }
    st->text[n] = '\0';
    m_say(env, RV9_STDOUT, st->text);

    env->getstat(stt, RV9_STT_GS_TIMING, &st->t);
    env->close(stt);

    ms(env, "(", st->t.audio_ms);
    ms(env, " of audio: ", st->t.encode_ms + st->t.decode_ms);
    m_say(env, RV9_STDOUT, " to recognise");
    if (st->t.load_ms) ms(env, "; reading the model took ", st->t.load_ms);
    m_say(env, RV9_STDOUT, ")\n");
    return 0;
}
