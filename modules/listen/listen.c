/*
 * listen -- hear something said, and write down what it was.
 *
 *   listen [seconds]       default 4, at most 16
 *   listen -c [seconds]    again and again, until it is stopped (kill)
 *   listen -f <file>       a recording instead: raw 16 kHz, one channel,
 *                          at most 16 seconds of it
 *
 * Whistle's engine is in this module, not in the firmware: the model, the
 * audio and the engine's work are this process's arena, and all of it is
 * given back when it ends. The price is reading the 17 MB model at every
 * start, about five seconds from the card -- done before the beep, so that
 * nobody speaks into it -- and -c is for paying it once.
 *
 * Records MIC2 from /au0: the recogniser wants one channel, and on this
 * board MIC2 is the more sensitive capsule of the two -- about twice MIC1's
 * level for the same sound. It beeps when it starts listening; without that
 * the first live test lost the front of its first word.
 */
#include "modlib.h"
#include "ndrt.h"
#include "nd_whistle.h"

#define RATE       16000
#define MAX_SECS   16
#define CHUNK      640               /* stereo frames, 40 ms */
#define BEEP       (RATE * 3 / 20)   /* 150 ms */
#define DISCARD    (RATE * 3 / 10)   /* the beep, its echo, and the codecs settling */
#define DEPTH      4                 /* of 8 decoder layers: the measured setting */
#define MAX_TOKENS 96
#define TEXT_MAX   512
#define MODEL      "/sd0/whistle.cact"

typedef struct {
    nd_ctx_t    ctx;
    nd_whistle *w;
    int16_t     st[CHUNK * 2];
    uint32_t    ids[MAX_TOKENS];
    char        text[TEXT_MAX];
    char        name[48];
} listen_statics_t;

static void ms(const rv9_mod_env_t *env, const char *what, uint32_t v)
{
    m_say(env, RV9_STDOUT, what);
    m_num(env, RV9_STDOUT, (int32_t)v);
    m_say(env, RV9_STDOUT, " ms");
}

static int read_all(const rv9_mod_env_t *env, int f, uint8_t *to, uint32_t n)
{
    for (uint32_t have = 0; have < n; ) {
        uint32_t want = n - have < 65536 ? n - have : 65536;
        int got = env->read(f, to + have, want);
        if (got <= 0) return -1;
        have += (uint32_t)got;
    }
    return 0;
}

static int load_model(const rv9_mod_env_t *env, listen_statics_t *st)
{
    int f = env->open(MODEL, RV9_MODE_READ);
    uint64_t size = 0;
    if (f < 0 || env->getstat(f, RV9_GS_SIZE, &size) < 0 || size == 0) {
        m_say(env, RV9_STDERR, "listen: cannot read " MODEL "\n");
        if (f >= 0) env->close(f);
        return -1;
    }
    uint8_t *blob = nd_malloc((size_t)size);
    st->w = nd_calloc(1, sizeof(*st->w));
    if (blob == 0 || st->w == 0) {
        m_say(env, RV9_STDERR, "listen: no room for the model\n");
        env->close(f);
        return -1;
    }
    if (read_all(env, f, blob, (uint32_t)size) < 0) {
        m_say(env, RV9_STDERR, "listen: the model would not read\n");
        env->close(f);
        return -1;
    }
    env->close(f);
    if (nd_whistle_open(st->w, blob, (size_t)size) != 0) {
        m_say(env, RV9_STDERR, "listen: " MODEL " is not a model this reads\n");
        return -1;
    }
    /* Not nd_whistle_cache_head: it copies the decoder's output head out
       of flash-mapped weights into PSRAM, and these are in PSRAM already. */
    nd_whistle_set_depth(st->w, DEPTH);
    return 0;
}

/* The beep, then `secs` of MIC2 into pcm. Returns the samples. */
static uint32_t record(const rv9_mod_env_t *env, listen_statics_t *st,
                       int16_t *pcm, uint32_t secs)
{
    int au = env->open("/au0", RV9_MODE_RW);
    if (au < 0) {
        m_say(env, RV9_STDERR, "listen: /au0: cannot open\n");
        return 0;
    }
    /* 880 Hz, a square wave at an eighth of full scale: short and
       unmistakable. Written first, so it is playing while the input
       below is thrown away. */
    for (uint32_t i = 0; i < CHUNK; i++) {
        int16_t v = ((i / 9) & 1) ? 4096 : -4096;
        st->st[2 * i] = v;
        st->st[2 * i + 1] = v;
    }
    for (uint32_t done = 0; done < BEEP; done += CHUNK) {
        uint32_t k = BEEP - done < CHUNK ? BEEP - done : CHUNK;
        env->write(au, st->st, k * 4);
    }
    for (uint32_t got = 0; got < DISCARD * 4; ) {
        uint32_t want = DISCARD * 4 - got;
        if (want > sizeof(st->st)) want = sizeof(st->st);
        int n = env->read(au, st->st, want);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    m_say(env, RV9_STDOUT, "listening...\n");
    uint32_t n = 0, frames = secs * RATE;
    while (n < frames) {
        uint32_t want = frames - n < CHUNK ? frames - n : CHUNK;
        int got = env->read(au, st->st, want * 4);
        if (got <= 0) break;
        uint32_t k = (uint32_t)got / 4;
        for (uint32_t i = 0; i < k; i++) pcm[n + i] = st->st[2 * i + 1];
        n += k;
    }
    env->close(au);
    return n;
}

static uint32_t from_file(const rv9_mod_env_t *env, const char *path,
                          int16_t *pcm, uint32_t cap)
{
    int f = env->open(path, RV9_MODE_READ);
    if (f < 0) {
        m_say(env, RV9_STDERR, "listen: cannot open the recording\n");
        return 0;
    }
    uint32_t n = 0;
    while (n < cap) {
        uint32_t want = (cap - n) * 2 < 65536 ? (cap - n) * 2 : 65536;
        int got = env->read(f, pcm + n, want);
        if (got <= 0) break;
        n += (uint32_t)got / 2;
    }
    env->close(f);
    return n;
}

/* One utterance, written down. 0, or the exit status to leave with. */
static int recognise(const rv9_mod_env_t *env, listen_statics_t *st,
                     const int16_t *pcm, uint32_t n, uint32_t load_ms)
{
    if (n < RATE / 10) {
        m_say(env, RV9_STDERR, "listen: nothing to recognise\n");
        return 5;
    }
    m_say(env, RV9_STDOUT, "thinking...\n");

    nd_whistle *w = st->w;
    uint32_t T   = nd_whistle_frames(w, n);
    float   *x   = nd_malloc((size_t)n * sizeof(float));
    float   *enc = nd_malloc((size_t)T * w->d * sizeof(float));
    if (x == 0 || enc == 0) {
        nd_free(x);
        m_say(env, RV9_STDERR, "listen: no room to work in\n");
        return 7;
    }
    for (uint32_t i = 0; i < n; i++) x[i] = pcm[i] / 32768.0f;

    st->ctx.last_us = env->time_us();
    uint64_t t1 = env->time_ms();
    int rc = nd_whistle_encode(w, x, n, enc);
    nd_free(x);
    if (rc != 0) {
        nd_free(enc);
        m_say(env, RV9_STDERR, "listen: the encoder failed\n");
        return 8;
    }
    uint64_t t2 = env->time_ms();
    int nt = nd_whistle_decode(w, enc, T, -1, st->ids, MAX_TOKENS);
    uint64_t t3 = env->time_ms();
    nd_free(enc);
    if (nt < 0) {
        m_say(env, RV9_STDERR, "listen: the decoder failed\n");
        return 8;
    }
    nd_whistle_detok(w, st->ids, (uint32_t)nt, st->text, TEXT_MAX - 1);
    st->text[TEXT_MAX - 1] = '\0';
    m_say(env, RV9_STDOUT, st->text);
    m_say(env, RV9_STDOUT, "\n");

    ms(env, "(", n / (RATE / 1000));
    ms(env, " of audio: ", (uint32_t)(t3 - t1));
    m_say(env, RV9_STDOUT, " to recognise");
    if (load_ms) ms(env, "; reading the model took ", load_ms);
    m_say(env, RV9_STDOUT, ")\n");
    return 0;
}

/* Everything that touches a float, kept out of the entry point, which
   runs before anybody has asked whether there is an FPU. */
__attribute__((noinline))
static int run(const rv9_mod_env_t *env, listen_statics_t *st, int file,
               int again, uint32_t secs)
{
    /* The same 16 seconds from a file: the engine's work grows with the
       length, about 430 KB a second over a 20 MB base (the model is 17),
       and 16 seconds is 26.6 MB. */
    uint32_t cap = (file ? MAX_SECS : secs) * RATE;
    int16_t *pcm = nd_malloc((size_t)cap * 2);
    if (pcm == 0) {
        m_say(env, RV9_STDERR, "listen: no room for the audio\n");
        return 7;
    }

    uint32_t n = 0;
    if (file && (n = from_file(env, st->name, pcm, cap)) == 0) return 4;

    m_say(env, RV9_STDOUT, "reading the model...\n");
    uint64_t t0 = env->time_ms();
    if (load_model(env, st) < 0) return 6;
    uint32_t load_ms = (uint32_t)(env->time_ms() - t0);

    if (file) return recognise(env, st, pcm, n, load_ms);

    for (;;) {
        if ((n = record(env, st, pcm, secs)) == 0) return 4;
        int rc = recognise(env, st, pcm, n, load_ms);
        if (!again) return rc;
        load_ms = 0;
        if (env->signals_take() & RV9_SIG_STOP) return 0;
    }
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == 0 || env->abi_version < 8) return -1;

    listen_statics_t *st = (listen_statics_t *)env->statics;
    if (st == 0 || env->statics_size < sizeof(*st)) return -2;

    const char *rest = env->arg ? m_word(env->arg, st->name, sizeof(st->name))
                                : "";
    int file = m_eq(st->name, "-f"), again = m_eq(st->name, "-c");
    if (file || again) rest = m_word(rest, st->name, sizeof(st->name));
    uint32_t secs = 4;
    if (!file && st->name[0]) secs = m_num_parse(st->name, 0);
    if (secs == 0 || secs > MAX_SECS || (file && st->name[0] == '\0')) {
        m_say(env, RV9_STDERR, "usage: listen [-c] [seconds] | "
                               "listen -f <file>\n");
        return 1;
    }

    rv9_sys_limits_t lim;
    memset(&lim, 0, sizeof(lim));
    env->sysinfo(RV9_SYS_LIMITS, &lim, sizeof(lim));
    if ((lim.isa & (RV9_ISA_PIE | RV9_ISA_FPU)) != (RV9_ISA_PIE | RV9_ISA_FPU)) {
        m_say(env, RV9_STDERR, "listen: needs a vector unit and an FPU "
                               "(the P4's)\n");
        return 2;
    }
    if (env->heap == 0) {
        m_say(env, RV9_STDERR, "listen: no arena\n");
        return 3;
    }

    st->ctx.env = env;
    nd_pool_init(&st->ctx.heap, env->heap, env->heap_size);

    /* s11 is the firmware's while this is not running: kept, and given
       back however this ends. */
    nd_ctx_t *theirs = nd_ctx_reg;
    nd_ctx_reg = &st->ctx;
    int rc = run(env, st, file, again, secs);
    nd_ctx_reg = theirs;
    return rc;
}
