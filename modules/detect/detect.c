/*
 * detect -- find the people in front of the camera.
 *
 *   detect [count]             that many pictures, default 1
 *   detect -f <file> [count]   a picture `snap -r` saved, instead
 *   detect -v ...              and where the time went, by kind of layer
 *   detect [-v] -c ...         plain C even on a CPU with a vector unit
 *
 * For each picture, one line per person found, best first:
 *
 *   person 0.88 143,186 251,463
 *
 * -- the score, then the box's corners in the camera's 800x640 pixels --
 * and then how long it took. Nothing found is said, so a reader never
 * waits for a line that is not coming.
 *
 * The engine is pico.c: Espressif's pedestrian detector (esp-dl, MIT),
 * rewritten in integer C and checked byte for byte against ESP-DL on this
 * board. Its model is a file, /sd0/pico.rv9m, made by tools/mkpico.py and
 * read into this program's arena when it starts; nothing of it is in the
 * firmware.
 *
 * It yields between layers. RV-9 switches threads only at system calls,
 * and a picture is a long time to keep the shell waiting.
 */
#include "modlib.h"
#include "pico.h"

#define MODEL   "/sd0/pico.rv9m"
#define CW      800
#define CH      640
#define FRAME   (CW * CH * 2)
#define CHUNK   (64 * 1024)

typedef struct {
    char        word[48];
    char        file[48];
    pico_t      p;
    pico_box_t  box[10];
    rv9_cam_frame_t f;
    uint32_t    us[6];          /* -v: 1x1 conv, larger conv, depthwise, rest */
    rv9_sys_limits_t lim;
} detect_statics_t;

static int read_all(const rv9_mod_env_t *env, int path, uint8_t *dst,
                    uint32_t n)
{
    uint32_t got = 0;
    while (got < n) {
        uint32_t want = n - got < CHUNK ? n - got : CHUNK;
        int k = env->read(path, dst + got, want);
        if (k <= 0) return -1;
        got += (uint32_t)k;
    }
    return 0;
}

static void ms(const rv9_mod_env_t *env, const char *what, uint32_t us)
{
    m_say(env, RV9_STDOUT, what);
    m_num(env, RV9_STDOUT, (int32_t)(us / 1000));
    m_say(env, RV9_STDOUT, " ms");
}

static void hex(const rv9_mod_env_t *env, uint32_t v)
{
    char b[9];
    for (int i = 0; i < 8; i++) b[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[8] = '\0';
    m_say(env, RV9_STDOUT, b);
}

static void score(const rv9_mod_env_t *env, uint32_t s)
{
    uint32_t c = (s * 100 + 32768) >> 16;       /* hundredths */
    m_num(env, RV9_STDOUT, (int32_t)(c / 100));
    m_say(env, RV9_STDOUT, c % 100 < 10 ? ".0" : ".");
    m_num(env, RV9_STDOUT, (int32_t)(c % 100));
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == 0 || env->abi_version < 15) return -1;
    detect_statics_t *st = (detect_statics_t *)env->statics;
    if (st == 0 || env->statics_size < sizeof(*st)) return -2;

    const char *rest = env->arg ? m_word(env->arg, st->word, sizeof(st->word)) : "";
    int verbose = m_eq(st->word, "-v");
    if (verbose) rest = m_word(rest, st->word, sizeof(st->word));
    int plain = m_eq(st->word, "-c");
    if (plain) rest = m_word(rest, st->word, sizeof(st->word));
    int from_file = m_eq(st->word, "-f");
    if (from_file) {
        rest = m_word(rest, st->file, sizeof(st->file));
        m_word(rest, st->word, sizeof(st->word));
    }
    uint32_t count = st->word[0] ? m_num_parse(st->word, 0) : 1;
    if (count == 0 || (from_file && !st->file[0])) {
        m_say(env, RV9_STDERR, "usage: detect [count] | detect -f <file> [count]\n");
        return 1;
    }

    /* The model, read whole into the front of the arena. */
    int mf = env->open(MODEL, RV9_MODE_READ);
    uint64_t msize = 0;
    if (mf < 0 || env->getstat(mf, RV9_GS_SIZE, &msize) < 0 || msize == 0) {
        m_say(env, RV9_STDERR, "detect: cannot read " MODEL "\n");
        if (mf >= 0) env->close(mf);
        return 2;
    }
    /* Sixteen-byte aligned, for the vector unit's loads. */
    uint8_t *heap = (uint8_t *)(((uintptr_t)env->heap + 15) & ~(uintptr_t)15);
    uint32_t mlen = (uint32_t)msize, at = (mlen + 15) & ~15u;
    if (env->heap == 0 || at + FRAME + 16 > env->heap_size ||
        read_all(env, mf, heap, mlen) < 0) {
        m_say(env, RV9_STDERR, "detect: no room for the model\n");
        env->close(mf);
        return 2;
    }
    env->close(mf);
    if (pico_open(&st->p, heap, mlen) < 0 ||
        at + pico_arena_bytes(&st->p) + FRAME + 16 > env->heap_size) {
        m_say(env, RV9_STDERR, "detect: " MODEL " is not a model this reads\n");
        return 2;
    }
    uint8_t *arena = heap + at;

    /* The vector unit, if this CPU has one. */
    if (env->sysinfo(RV9_SYS_LIMITS, &st->lim, sizeof(st->lim)) > 0 &&
        (st->lim.isa & RV9_ISA_PIE)) {
        st->p.pie = 1;
    }
    if (plain) st->p.pie = 0;
    uint16_t *frame = (uint16_t *)(arena + ((pico_arena_bytes(&st->p) + 15) & ~15u));

    int src = env->open(st->file[0] ? st->file : "/cam0", RV9_MODE_READ);
    if (src < 0) {
        m_say(env, RV9_STDERR, "detect: cannot open the picture\n");
        return 3;
    }
    if (!st->file[0]) {
        uint32_t fmt = RV9_CAM_FMT_RGB565;
        env->setstat(src, RV9_CAM_SS_FORMAT, &fmt);
    }

    int rc = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t t0 = env->time_us();
        if (st->file[0] && i > 0) env->seek(src, 0, RV9_SEEK_SET);
        if (read_all(env, src, (uint8_t *)frame, FRAME) < 0) {
            m_say(env, RV9_STDERR, "detect: no 800x640 picture to read\n");
            rc = 4;
            break;
        }
        uint64_t t1 = env->time_us();
        pico_input(&st->p, arena, frame, CW, CH, PICO_RGB565);
        for (uint32_t k = 0; k < st->p.n_nodes; k++) {
            uint64_t a = env->time_us();
            pico_node(&st->p, arena, k);
            if (verbose) {
                const uint8_t *nd = st->p.nodes + k * 32;
                int kind = nd[0] == 1 ? (nd[2] == 1 ? 0 : 1) : nd[0] == 2 ? 2 : 3;
                st->us[kind] += (uint32_t)(env->time_us() - a);
            }
            env->yield();
        }
        int n = pico_post(&st->p, arena, CW, CH, st->box, 10);
        uint64_t t2 = env->time_us();

        for (int j = 0; j < n; j++) {
            pico_box_t *b = &st->box[j];
            m_say(env, RV9_STDOUT, "person ");
            score(env, b->score);
            m_say(env, RV9_STDOUT, " ");
            m_num(env, RV9_STDOUT, b->x1);
            m_say(env, RV9_STDOUT, ",");
            m_num(env, RV9_STDOUT, b->y1);
            m_say(env, RV9_STDOUT, " ");
            m_num(env, RV9_STDOUT, b->x2);
            m_say(env, RV9_STDOUT, ",");
            m_num(env, RV9_STDOUT, b->y2);
            m_say(env, RV9_STDOUT, "\n");
        }
        if (n == 0) m_say(env, RV9_STDOUT, "nobody\n");
        ms(env, "(", (uint32_t)(t1 - t0));
        ms(env, " for the picture, ", (uint32_t)(t2 - t1));
        m_say(env, RV9_STDOUT, " to look at it)\n");
        if (verbose) {
            m_say(env, RV9_STDOUT, "  outputs hash ");
            hex(env, pico_hash(&st->p, arena));
            m_say(env, RV9_STDOUT, st->p.pie ? "; vector unit:" : "; plain C:");
            ms(env, " 1x1 conv ", st->us[0]);
            ms(env, ", other conv ", st->us[1]);
            ms(env, ", depthwise ", st->us[2]);
            ms(env, ", the rest ", st->us[3]);
            m_say(env, RV9_STDOUT, "\n");
            for (int j = 0; j < 4; j++) st->us[j] = 0;
        }
    }
    env->close(src);
    return rc;
}
