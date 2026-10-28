/*
 * play -- play a recording, or a tone, through the speaker.
 *
 *   play <file>            raw 16 kHz, two 16-bit channels: what `rec` makes
 *   play -1 <file>         one channel, as Whistle's clips and arecord -c 1
 *   play -v <dB> ...       speaker volume first, -95 to +32
 *   play beep [seconds]    440 Hz, a third of full scale; default one second
 *
 * /au0 takes two channels and plays the left, so a one-channel file is
 * doubled up here rather than in the driver: the driver says what the
 * hardware does, and a program that has something else to say converts.
 */
#include "modlib.h"

#define RATE   16000
#define IN     1024

typedef struct {
    uint8_t in[IN];
    int16_t out[IN];            /* a mono chunk, doubled */
    char    name[48];
} play_statics_t;

static int put(const rv9_mod_env_t *env, int au, const void *b, uint32_t n)
{
    return env->write(au, b, n) == (int)n ? 0 : -1;
}

static int beep(const rv9_mod_env_t *env, play_statics_t *st, int au,
                uint32_t secs)
{
    /* A triangle: no table and no multiply-heavy sine, and soft enough. */
    uint32_t phase = 0, inc = (uint32_t)(440ull * 4294967296ull / RATE);
    uint32_t frames = secs * RATE;
    while (frames > 0) {
        uint32_t n = frames < IN / 2 ? frames : IN / 2;
        for (uint32_t i = 0; i < n; i++) {
            int32_t x = (int32_t)(phase >> 16);             /* 0..65535 */
            int32_t t = (x < 32768 ? x : 65535 - x) * 2 - 32768;
            int16_t v = (int16_t)(t / 3);
            st->out[2 * i] = v;
            st->out[2 * i + 1] = v;
            phase += inc;
        }
        if (put(env, au, st->out, n * 4) < 0) return 5;
        frames -= n;
    }
    return 0;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == 0 || env->abi_version < 8) return -1;

    play_statics_t *st = (play_statics_t *)env->statics;
    if (st == 0 || env->statics_size < sizeof(*st)) return -2;

    const char *rest = env->arg ? env->arg : "";
    int mono = 0, have_vol = 0;
    int32_t vol = 0;
    for (;;) {
        rest = m_word(rest, st->name, sizeof(st->name));
        if (m_eq(st->name, "-1")) { mono = 1; continue; }
        if (m_eq(st->name, "-v")) {
            char num[12];
            rest = m_word(rest, num, sizeof(num));
            int neg = num[0] == '-';
            vol = (int32_t)m_num_parse(num + neg, 0);
            if (neg) vol = -vol;
            have_vol = 1;
            continue;
        }
        break;
    }
    if (st->name[0] == '\0') {
        m_say(env, RV9_STDERR, "usage: play [-1] [-v dB] <file> | beep [s]\n");
        return 1;
    }

    int au = env->open("/au0", RV9_MODE_WRITE);
    if (au < 0) {
        m_say(env, RV9_STDERR, "play: /au0: cannot open\n");
        return 3;
    }
    if (have_vol) env->setstat(au, RV9_AU_SS_VOLUME, &vol);

    int rc = 0;
    if (m_eq(st->name, "beep")) {
        while (*rest == ' ') rest++;
        uint32_t secs = *rest ? m_num_parse(rest, 0) : 1;
        rc = beep(env, st, au, secs ? secs : 1);
        env->close(au);
        return rc;
    }

    int f = env->open(st->name, RV9_MODE_READ);
    if (f < 0) {
        m_say(env, RV9_STDERR, st->name);
        m_say(env, RV9_STDERR, ": cannot open\n");
        env->close(au);
        return 4;
    }

    uint32_t chunk = mono ? IN / 2 : IN;
    for (;;) {
        int n = env->read(f, st->in, chunk);
        if (n <= 0) break;
        if (mono) {
            const int16_t *s = (const int16_t *)st->in;
            uint32_t k = (uint32_t)n / 2;
            for (uint32_t i = 0; i < k; i++) {
                st->out[2 * i] = s[i];
                st->out[2 * i + 1] = s[i];
            }
            if (put(env, au, st->out, k * 4) < 0) { rc = 5; break; }
        } else if (put(env, au, st->in, (uint32_t)n & ~3u) < 0) {
            rc = 5;
            break;
        }
    }

    env->close(f);
    env->close(au);
    return rc;
}
