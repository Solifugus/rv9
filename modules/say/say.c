/*
 * say -- speak, through the speaker.
 *
 *   say <words>            say them
 *   say                    say what comes in on standard input: date | say
 *   say -v <dB> ...        speaker volume first, -95 to +32
 *   say -o <file> ...      the speech into a file instead, raw 16 kHz mono
 *                          16-bit, as `play -1` and `listen -f` take
 *   say -t ...             and then how long it took
 *
 * The voice is /tts0's, and the samples come out of it as they are made,
 * so the first sentence is heard while the rest is still being worked
 * out. Like play, a one-channel voice is doubled up here for /au0's two.
 *
 * And made 3 dB louder: slt peaks 3.5 to 5 dB below full scale, so there
 * is room, and this leaves the speaker's volume -- which play and every
 * other program share -- where it was. Saturated, in case a voice has
 * less room than slt.
 */
#include "modlib.h"

#define TEXT_MAX 1024
#define IN       1024               /* bytes of mono at a time */

#define LOUDER 181                  /* x 181/128: +3 dB */

static int16_t louder(int16_t v)
{
    int32_t x = (int32_t)v * LOUDER / 128;
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

typedef struct {
    char             text[TEXT_MAX];
    int16_t          in[IN / 2];
    int16_t          out[IN];        /* the same, doubled */
    char             word[48];
    char             file[48];
    rv9_tts_timing_t t;
} say_statics_t;

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

    say_statics_t *st = (say_statics_t *)env->statics;
    if (st == 0 || env->statics_size < sizeof(*st)) return -2;

    const char *rest = env->arg ? env->arg : "";
    int have_vol = 0, timing = 0;
    int32_t vol = 0;
    st->file[0] = '\0';
    for (;;) {
        const char *next = m_word(rest, st->word, sizeof(st->word));
        if (m_eq(st->word, "-t")) { timing = 1; rest = next; continue; }
        if (m_eq(st->word, "-o")) {
            rest = m_word(next, st->file, sizeof(st->file));
            continue;
        }
        if (m_eq(st->word, "-v")) {
            char num[12];
            rest = m_word(next, num, sizeof(num));
            int neg = num[0] == '-';
            vol = (int32_t)m_num_parse(num + neg, 0);
            if (neg) vol = -vol;
            have_vol = 1;
            continue;
        }
        break;
    }

    /* The words, or standard input to its end. */
    uint32_t len = 0;
    if (*rest) {
        while (rest[len] && len < TEXT_MAX) { st->text[len] = rest[len]; len++; }
    } else {
        for (;;) {
            int n = env->read(RV9_STDIN, st->text + len, TEXT_MAX - len);
            if (n <= 0) break;
            len += (uint32_t)n;
            if (len == TEXT_MAX) break;
        }
    }
    if (len == 0) {
        m_say(env, RV9_STDERR,
              "usage: say [-v dB] [-o file] [-t] [words]\n");
        return 1;
    }

    int tts = env->open("/tts0", RV9_MODE_RW);
    if (tts < 0) {
        m_say(env, RV9_STDERR, "say: /tts0: cannot open\n");
        return 3;
    }
    if (env->write(tts, st->text, len) < (int)len ||
        env->setstat(tts, RV9_TTS_SS_DONE, 0) < 0) {
        m_say(env, RV9_STDERR, "say: /tts0 would not take it\n");
        env->close(tts);
        return 4;
    }

    int out;
    if (st->file[0]) {
        out = env->open(st->file, RV9_MODE_WRITE | RV9_MODE_CREATE);
    } else {
        out = env->open("/au0", RV9_MODE_WRITE);
        if (out >= 0 && have_vol) env->setstat(out, RV9_AU_SS_VOLUME, &vol);
    }
    if (out < 0) {
        m_say(env, RV9_STDERR, "say: ");
        m_say(env, RV9_STDERR, st->file[0] ? st->file : "/au0");
        m_say(env, RV9_STDERR, ": cannot open\n");
        env->close(tts);
        return 5;
    }

    int rc = 0;
    for (;;) {
        int n = env->read(tts, st->in, IN);
        if (n == -RV9_IOE_INVAL) break;            /* all of it */
        if (n < 0) {
            m_say(env, RV9_STDERR, "say: the voice failed; is "
                                   "/sd0/slt.flitevox there?\n");
            rc = 6;
            break;
        }
        uint32_t k = (uint32_t)n / 2;
        for (uint32_t i = 0; i < k; i++) st->in[i] = louder(st->in[i]);
        int w;
        if (st->file[0]) {
            w = env->write(out, st->in, k * 2);
            if (w < (int)(k * 2)) { m_say(env, RV9_STDERR, "say: out of room\n"); rc = 7; break; }
        } else {
            for (uint32_t i = 0; i < k; i++) {
                st->out[2 * i] = st->in[i];
                st->out[2 * i + 1] = st->in[i];
            }
            w = env->write(out, st->out, k * 4);
            if (w < (int)(k * 4)) { rc = 7; break; }
        }
    }

    if (timing && rc == 0 && env->getstat(tts, RV9_TTS_GS_TIMING, &st->t) >= 0) {
        ms(env, "(", st->t.audio_ms);
        ms(env, " of speech: the first sound after ", st->t.first_ms);
        ms(env, ", all of it in ", st->t.synth_ms);
        if (st->t.load_ms) ms(env, "; reading the voice took ", st->t.load_ms);
        m_say(env, RV9_STDOUT, ")\n");
    }
    env->close(out);
    env->close(tts);
    return rc;
}
