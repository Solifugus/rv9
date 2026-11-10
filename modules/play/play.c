/*
 * play -- play a recording, or a tone, through the speaker.
 *
 *   play <file>            raw 16 kHz, two 16-bit channels: what `rec` makes
 *   play -1 <file>         one channel, as Whistle's clips and arecord -c 1
 *   play -v <dB> ...       speaker volume first, -95 to +32
 *   play beep [seconds]    440 Hz, a third of full scale; default one second
 *   play tone <Hz> [ms]    any pitch, 20 to 7900 Hz; default half a second
 *   play notes <notes>     a tune, struck like a piano: C4 E4 G4 C5/2
 *
 * NOTES are read left to right, spaces and commas between them:
 *
 *   C D E F G A B   the note, then # or b, then its octave (4 is middle C's)
 *   /2 /4 /8 /16    how long: a half, quarter, eighth or sixteenth; /1 a whole
 *   .               after the length, half as long again (dotted)
 *   R               a rest, as long as a note
 *   T<bpm>          the tempo, quarter notes a minute (default 120)
 *
 * The octave and the length carry on from the note before, so
 * `C4/8 D E F G/4` is four eighths and a quarter in octave 4. Each note
 * is struck and dies away, a triangle with its octave above it, so a run
 * of them sounds like keys and not a buzzer.
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

/* Phase steps a sample at 16 kHz for octave 4, C to B: f * 2^32 / 16000,
   from frequencies in millihertz, worked out by the compiler. A module has
   no floating point library and no 64-bit division at run time. */
#define STEP(mhz) ((uint32_t)((mhz##ull << 32) / (RATE * 1000ull)))
static const uint32_t OCTAVE4[12] = {
    STEP(261626), STEP(277183), STEP(293665), STEP(311127), STEP(329628),
    STEP(349228), STEP(369994), STEP(391995), STEP(415305), STEP(440000),
    STEP(466164), STEP(493883),
};

static int32_t tri(uint32_t phase)
{
    int32_t x = (int32_t)(phase >> 16);                     /* 0..65535 */
    return (x < 32768 ? x : 65535 - x) * 2 - 32768;
}

/*
 * One note, `frames` long, at phase step `inc` (0 for silence). Struck,
 * it starts loud and falls away by a sixteenth every 256 samples (about
 * 16 ms), as a string does; held, it stays level. Either way it rises over
 * the first 2 ms and falls to nothing over the last 5, so no note clicks.
 */
static int voice(const rv9_mod_env_t *env, play_statics_t *st, int au,
                 uint32_t inc, uint32_t frames, int struck)
{
    uint32_t phase = 0, done = 0;
    int32_t amp = 32767;                                    /* Q15 */
    while (done < frames) {
        uint32_t n = frames - done < IN / 2 ? frames - done : IN / 2;
        for (uint32_t i = 0; i < n; i++, done++) {
            int32_t v = 0;
            if (inc) {
                int32_t e = amp;
                if (done < 32) e = e * (int32_t)done / 32;
                uint32_t left = frames - done;
                if (left < 80) e = e * (int32_t)left / 80;
                int32_t w = tri(phase) + (struck ? tri(phase * 2) / 3 : 0);
                v = (w / 3) * e / 32768;
                phase += inc;
                if (struck) amp -= amp >> 12;
            }
            st->out[2 * i] = (int16_t)v;
            st->out[2 * i + 1] = (int16_t)v;
        }
        if (put(env, au, st->out, n * 4) < 0) return 5;
    }
    return 0;
}

static int bad_note(const rv9_mod_env_t *env, const char *at)
{
    char c[2] = { *at, 0 };
    m_say(env, RV9_STDERR, "play: no such note at '");
    m_say(env, RV9_STDERR, c);
    m_say(env, RV9_STDERR, "' (C4 D#4 Eb4/8 G4/4. R T140)\n");
    return 1;
}

/* A tune: see NOTES above. */
static int notes(const rv9_mod_env_t *env, play_statics_t *st, int au, const char *s)
{
    static const uint8_t SEMI[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* A..G from C */
    uint32_t octave = 4, len = 4, tempo = 120, played = 0;
    while (*s) {
        if (*s == ' ' || *s == ',' || *s == '"' || *s == '\'') { s++; continue; }
        char c = *s >= 'a' && *s <= 'z' ? (char)(*s - 32) : *s;
        if (c == 'T') {
            tempo = m_num_parse(s + 1, &s);
            if (tempo < 20 || tempo > 400) return bad_note(env, s - 1);
            continue;
        }
        int semi = -1;
        if (c >= 'A' && c <= 'G') semi = SEMI[c - 'A'];
        else if (c != 'R') return bad_note(env, s);
        s++;
        int note = semi >= 0;                               /* else a rest */
        if (note && *s == '#') { semi++; s++; }
        else if (note && *s == 'b') { semi--; s++; }
        if (note && *s >= '0' && *s <= '8') octave = (uint32_t)(*s++ - '0');
        if (*s == '/') {
            len = m_num_parse(s + 1, &s);
            if (len == 0 || len > 64) return bad_note(env, s);
        }
        uint32_t ms = 240000 / (tempo * len);               /* a whole is four beats */
        if (*s == '.') { ms = ms * 3 / 2; s++; }

        uint32_t inc = 0;
        if (note) {
            int32_t o = (int32_t)octave;
            if (semi < 0) { semi += 12; o--; }              /* Cb */
            if (semi > 11) { semi -= 12; o++; }             /* B# */
            if (o < 0 || o > 8) return bad_note(env, s - 1);
            inc = OCTAVE4[semi];
            inc = o >= 4 ? inc << (o - 4) : inc >> (4 - o);
        }
        int rc = voice(env, st, au, inc, ms * (RATE / 1000), 1);
        if (rc) return rc;
        played++;
    }
    if (played == 0) {
        m_say(env, RV9_STDERR, "usage: play notes C4 E4 G4 C5/2\n");
        return 1;
    }
    return 0;
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
        m_say(env, RV9_STDERR, "usage: play [-1] [-v dB] <file> | beep [s] | "
                               "tone <Hz> [ms] | notes <notes>\n");
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

    if (m_eq(st->name, "tone")) {
        while (*rest == ' ') rest++;
        uint32_t hz = m_num_parse(rest, &rest);
        while (*rest == ' ') rest++;
        uint32_t ms = *rest ? m_num_parse(rest, 0) : 500;
        if (hz < 20 || hz > 7900 || ms == 0 || ms > 60000) {
            m_say(env, RV9_STDERR, "play: tone <Hz 20..7900> [ms]\n");
            env->close(au);
            return 1;
        }
        /* hz * 2^32 / 16000, in 32 bits: 268435.456 a hertz */
        uint32_t inc = hz * 268435u + hz * 456u / 1000u;
        rc = voice(env, st, au, inc, ms * (RATE / 1000), 0);
        env->close(au);
        return rc;
    }
    if (m_eq(st->name, "notes")) {
        rc = notes(env, st, au, rest);
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
