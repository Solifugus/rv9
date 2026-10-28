/*
 * rec -- record the microphones to a file.
 *
 *   rec <file> [seconds]       default 3, as long as the arena allows
 *
 * The file is raw: 16 kHz frames of two 16-bit little-endian samples,
 * MIC1 then MIC2, exactly what /au0 gives. `play` plays it back, and on a
 * PC `aplay -f S16_LE -r 16000 -c 2` does.
 *
 * It records into its arena and writes the file afterwards. A flash write
 * can stall for longer than /au0 buffers -- erasing a sector is not
 * instant -- and a recorder that writes as it goes drops whatever arrived
 * during the stall, silently. Memory does not stall. That is what the
 * arena is for: 1 MB of external memory, which is 16 seconds, settled at
 * fork, so a recording that cannot fit is refused before it starts.
 *
 * The first tenth of a second is thrown away: the codecs are started by
 * the first read, and what they send while settling is not sound.
 *
 * Says how loud each microphone was, because a silent recording and a
 * broken microphone look the same until somebody plays it.
 */
#include "modlib.h"

#define RATE        16000
#define FRAME       4
#define SETTLE      (RATE / 10 * FRAME)
#define CHUNK       1280            /* 20 ms */

typedef struct { char name[48]; } rec_statics_t;

/* As a share of full scale, and the raw figure beside it: "0%" cannot
   tell a quiet room from a dead line, and the number can. */
static void pct(const rv9_mod_env_t *env, uint32_t peak)
{
    m_num(env, RV9_STDOUT, (int32_t)(peak * 100 / 32768));
    m_say(env, RV9_STDOUT, "% (");
    m_num(env, RV9_STDOUT, (int32_t)peak);
    m_say(env, RV9_STDOUT, ")");
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == 0 || env->abi_version < 15) return -1;

    rec_statics_t *st = (rec_statics_t *)env->statics;
    if (st == 0 || env->statics_size < sizeof(*st)) return -2;
    if (env->heap == 0) {
        m_say(env, RV9_STDERR, "rec: no arena; this board has no external "
                               "memory to record into\n");
        return 2;
    }

    const char *rest = env->arg ? m_word(env->arg, st->name, sizeof(st->name))
                                : "";
    if (st->name[0] == '\0') {
        m_say(env, RV9_STDERR, "usage: rec <file> [seconds]\n");
        return 1;
    }
    uint32_t secs = 3;
    while (*rest == ' ') rest++;
    if (*rest) secs = m_num_parse(rest, 0);

    uint32_t want = secs * RATE * FRAME;
    if (secs == 0 || want > env->heap_size) {
        m_say(env, RV9_STDERR, "rec: 1 to ");
        m_num(env, RV9_STDERR, (int32_t)(env->heap_size / (RATE * FRAME)));
        m_say(env, RV9_STDERR, " seconds\n");
        return 1;
    }

    int au = env->open("/au0", RV9_MODE_READ);
    if (au < 0) {
        m_say(env, RV9_STDERR, "rec: /au0: cannot open\n");
        return 3;
    }

    uint8_t *buf = (uint8_t *)env->heap;
    uint32_t skipped = 0;
    while (skipped < SETTLE) {
        int n = env->read(au, buf, SETTLE - skipped);
        if (n <= 0) break;
        skipped += (uint32_t)n;
    }

    m_say(env, RV9_STDOUT, "recording...\n");
    uint32_t have = 0;
    while (have < want) {
        uint32_t ask = want - have < CHUNK ? want - have : CHUNK;
        int n = env->read(au, buf + have, ask);
        if (n <= 0) break;
        have += (uint32_t)n;
    }
    env->close(au);

    uint32_t peak[2] = { 0, 0 };
    const int16_t *s = (const int16_t *)env->heap;
    for (uint32_t i = 0; i < have / 2; i++) {
        int32_t v = s[i];
        uint32_t m = (uint32_t)(v < 0 ? -v : v);
        if (m > peak[i & 1]) peak[i & 1] = m;
    }

    /*
     * Every sample zero is not a quiet room. A quiet room peaks at a few
     * hundred here; zero means the converter sent nothing at all, and it
     * happened once, on the first recording after the ES7210 was first
     * brought up from power-on, and not again in eleven cold starts since.
     * Saying so is cheap and the alternative is somebody talking at a
     * board that is not listening.
     */
    if (have > 0 && peak[0] == 0 && peak[1] == 0) {
        m_say(env, RV9_STDERR, "rec: every sample was zero: the microphones "
                               "sent nothing. Take the board's power away and "
                               "put it back.\n");
    }

    int f = env->open(st->name, RV9_MODE_WRITE | RV9_MODE_CREATE);
    if (f < 0) {
        m_say(env, RV9_STDERR, st->name);
        m_say(env, RV9_STDERR, ": cannot create\n");
        return 4;
    }
    uint32_t put = 0;
    while (put < have) {
        uint32_t n = have - put < 4096 ? have - put : 4096;
        int w = env->write(f, buf + put, n);
        if (w < (int)n) {
            m_say(env, RV9_STDERR, st->name);
            m_say(env, RV9_STDERR, ": ran out of room\n");
            env->close(f);
            return 5;
        }
        put += n;
    }
    env->close(f);

    m_num(env, RV9_STDOUT, (int32_t)(have / FRAME));
    m_say(env, RV9_STDOUT, " frames to ");
    m_say(env, RV9_STDOUT, st->name);
    m_say(env, RV9_STDOUT, "; peak MIC1 ");
    pct(env, peak[0]);
    m_say(env, RV9_STDOUT, ", MIC2 ");
    pct(env, peak[1]);
    m_say(env, RV9_STDOUT, "\n");
    return 0;
}
