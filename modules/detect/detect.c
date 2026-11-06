/*
 * detect -- find the people, or the faces, in front of the camera.
 *
 *   detect [count]                 people: that many pictures, default 1
 *   detect faces [count]           faces, and where their eyes, nose and
 *                                  mouth are
 *   detect [faces] -f <file> [n]   a picture `snap -r` saved, instead
 *   detect [faces] -v ...          and the time, and an outputs hash
 *   detect [faces] [-v] -c ...     plain C even on a CPU with a vector unit
 *
 * One line per find, best first:
 *
 *   person 0.88 143,186 251,463
 *   face 0.61 347,113 639,491 eyes 433,268 548,259 nose 494,298 mouth 449,397 533,394
 *
 * -- the score, then the box's corners in the camera's 800x640 pixels,
 * and for a face five points, the picture's left first -- and then how
 * long it took. Nothing found is said, so a reader never waits for a line
 * that is not coming.
 *
 * The engine is pico.c: Espressif's pedestrian detector and two-stage face
 * detector (esp-dl, MIT), rewritten in integer C and checked byte for byte
 * against ESP-DL on this board. The models are files -- /sd0/pico.rv9m,
 * /sd0/face1.rv9m and /sd0/face2.rv9m, made by tools/mkpico.py -- read
 * into this program's arena when it starts; nothing of them is in the
 * firmware.
 *
 * It yields between layers. RV-9 switches threads only at system calls,
 * and a picture is a long time to keep the shell waiting.
 */
#include "modlib.h"
#include "pico.h"

#define CW      800
#define CH      640
#define FRAME   (CW * CH * 2)
#define CHUNK   (64 * 1024)

typedef struct {
    char        word[48];
    char        file[48];
    pico_t      p, p2;
    pico_face_t face[10];
    pico_box_t  box[10];
    uint32_t    us[4];          /* -v: 1x1 conv, larger conv, depthwise, rest */
    rv9_sys_limits_t lim;
    /* -v for faces: per stage, by kind of layer; see between(). */
    const rv9_mod_env_t *env;
    uint64_t    last;
    uint32_t    fus[2][6];
} detect_statics_t;

enum { K_CONV_VEC, K_CONV_C, K_DW_VEC, K_DW_C, K_PRELU, K_OTHER };

/* After each face layer: charge the time since the last one to its kind,
   then let the rest of the system run -- outside the measurement. */
static void between(void *ctx, const pico_t *p, uint32_t i)
{
    detect_statics_t *st = (detect_statics_t *)ctx;
    uint64_t now = st->env->time_us();
    const uint8_t *nd = p->nodes + i * 32;
    int kind = K_OTHER;
    if (nd[0] == 1) kind = p->pie ? K_CONV_VEC : K_CONV_C;
    else if (nd[0] == 2) {
        int c = pico_stride(p, (uint32_t)(nd[12] | nd[13] << 8));
        kind = (p->pie && (c & 15) == 0) ? K_DW_VEC : K_DW_C;
    } else if (nd[0] == 8) kind = K_PRELU;
    st->fus[p->kind == PICO_KIND_FACE1 ? 0 : 1][kind] += (uint32_t)(now - st->last);
    st->env->yield();
    st->last = st->env->time_us();
}

/* A switch, not a table of pointers: a module is position independent,
   and an array of string addresses would be absolute ones. */
static const char *kind_name(int k)
{
    switch (k) {
    case K_CONV_VEC: return "conv vector ";
    case K_CONV_C:   return "conv C ";
    case K_DW_VEC:   return "depthwise vector ";
    case K_DW_C:     return "depthwise C ";
    case K_PRELU:    return "PReLU ";
    default:         return "rest ";
    }
}

static void env_yield(void *ctx, const pico_t *p, uint32_t i)
{
    (void)p; (void)i;
    ((detect_statics_t *)ctx)->env->yield();
}

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

/*
 * A model file into memory at *at, which moves past it, sixteen-byte
 * aligned for the vector unit. 0, or a message has been said.
 */
static int load(const rv9_mod_env_t *env, const char *path, pico_t *p,
                uint8_t *heap, uint32_t *at, uint32_t room)
{
    int f = env->open(path, RV9_MODE_READ);
    uint64_t size = 0;
    if (f < 0 || env->getstat(f, RV9_GS_SIZE, &size) < 0 || size == 0) {
        m_say(env, RV9_STDERR, "detect: cannot read ");
        m_say(env, RV9_STDERR, path);
        m_say(env, RV9_STDERR, "\n");
        if (f >= 0) env->close(f);
        return -1;
    }
    uint32_t n = (uint32_t)size;
    if (*at + n > room || read_all(env, f, heap + *at, n) < 0) {
        m_say(env, RV9_STDERR, "detect: no room for the model\n");
        env->close(f);
        return -1;
    }
    env->close(f);
    if (pico_open(p, heap + *at, n) < 0) {
        m_say(env, RV9_STDERR, "detect: ");
        m_say(env, RV9_STDERR, path);
        m_say(env, RV9_STDERR, " is not a model this reads\n");
        return -1;
    }
    *at = (*at + n + 15) & ~15u;
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

static void point(const rv9_mod_env_t *env, const char *before, int32_t x,
                  int32_t y)
{
    m_say(env, RV9_STDOUT, before);
    m_num(env, RV9_STDOUT, x);
    m_say(env, RV9_STDOUT, ",");
    m_num(env, RV9_STDOUT, y);
}

static void found(const rv9_mod_env_t *env, const char *what,
                  const pico_box_t *b)
{
    m_say(env, RV9_STDOUT, what);
    score(env, b->score);
    point(env, " ", b->x1, b->y1);
    point(env, " ", b->x2, b->y2);
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == 0 || env->abi_version < 15) return -1;
    detect_statics_t *st = (detect_statics_t *)env->statics;
    if (st == 0 || env->statics_size < sizeof(*st)) return -2;

    const char *rest = env->arg ? m_word(env->arg, st->word, sizeof(st->word)) : "";
    int faces = m_eq(st->word, "faces");
    if (faces) rest = m_word(rest, st->word, sizeof(st->word));
    else if (m_eq(st->word, "people")) rest = m_word(rest, st->word, sizeof(st->word));
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
        m_say(env, RV9_STDERR, "usage: detect [faces|people] [-v] [-c] [-f <file>] [count]\n");
        return 1;
    }

    /* The models at the front of the arena, then the activations, then
       the frame. Sixteen-byte aligned throughout, for the vector unit. */
    uint8_t *heap = (uint8_t *)(((uintptr_t)env->heap + 15) & ~(uintptr_t)15);
    uint32_t room = env->heap ? env->heap_size - 16 : 0, at = 0;
    if (faces) {
        if (load(env, "/sd0/face1.rv9m", &st->p, heap, &at, room) < 0 ||
            load(env, "/sd0/face2.rv9m", &st->p2, heap, &at, room) < 0) {
            return 2;
        }
    } else if (load(env, "/sd0/pico.rv9m", &st->p, heap, &at, room) < 0) {
        return 2;
    }
    uint32_t need = pico_arena_bytes(&st->p);
    if (faces && pico_arena_bytes(&st->p2) > need) need = pico_arena_bytes(&st->p2);
    need = (need + 15) & ~15u;
    if (at + need + FRAME > room) {
        m_say(env, RV9_STDERR, "detect: no room to work in\n");
        return 2;
    }
    uint8_t *arena = heap + at;
    uint16_t *frame = (uint16_t *)(arena + need);

    /* The vector unit, if this CPU has one. */
    int pie = !plain && env->sysinfo(RV9_SYS_LIMITS, &st->lim, sizeof(st->lim)) > 0 &&
              (st->lim.isa & RV9_ISA_PIE);
    st->p.pie = st->p2.pie = pie;

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
        int n;
        if (faces) {
            st->env = env;
            st->last = env->time_us();
            n = pico_faces(&st->p, &st->p2, arena, frame, CW, CH, PICO_RGB565,
                           st->face, 10, verbose ? between : env_yield, st);
        } else {
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
            n = pico_post(&st->p, arena, CW, CH, st->box, 10);
        }
        uint64_t t2 = env->time_us();

        for (int j = 0; j < n; j++) {
            if (faces) {
                const pico_face_t *f = &st->face[j];
                found(env, "face ", &f->box);
                /* Stored left eye, left mouth, nose, right eye, right mouth. */
                point(env, " eyes ", f->pt[0], f->pt[1]);
                point(env, " ", f->pt[6], f->pt[7]);
                point(env, " nose ", f->pt[4], f->pt[5]);
                point(env, " mouth ", f->pt[2], f->pt[3]);
                point(env, " ", f->pt[8], f->pt[9]);
            } else {
                found(env, "person ", &st->box[j]);
            }
            m_say(env, RV9_STDOUT, "\n");
        }
        if (n == 0) m_say(env, RV9_STDOUT, faces ? "no faces\n" : "nobody\n");
        ms(env, "(", (uint32_t)(t1 - t0));
        ms(env, " for the picture, ", (uint32_t)(t2 - t1));
        m_say(env, RV9_STDOUT, " to look at it)\n");
        if (verbose) {
            m_say(env, RV9_STDOUT, "  outputs hash ");
            hex(env, pico_hash(faces ? &st->p2 : &st->p, arena));
            m_say(env, RV9_STDOUT, pie ? "; vector unit" : "; plain C");
            if (faces) {
                for (int sg = 0; sg < 2; sg++) {
                    m_say(env, RV9_STDOUT, sg ? "\n  stage 2:" : "\n  stage 1:");
                    for (int k = 0; k < 6; k++) {
                        if (st->fus[sg][k] == 0) continue;
                        m_say(env, RV9_STDOUT, " ");
                        m_say(env, RV9_STDOUT, kind_name(k));
                        m_num(env, RV9_STDOUT, (int32_t)(st->fus[sg][k] / 100) / 10);
                        m_say(env, RV9_STDOUT, ".");
                        m_num(env, RV9_STDOUT, (int32_t)(st->fus[sg][k] / 100) % 10);
                        m_say(env, RV9_STDOUT, ",");
                        st->fus[sg][k] = 0;
                    }
                }
            } else {
                ms(env, ": 1x1 conv ", st->us[0]);
                ms(env, ", other conv ", st->us[1]);
                ms(env, ", depthwise ", st->us[2]);
                ms(env, ", the rest ", st->us[3]);
                for (int j = 0; j < 4; j++) st->us[j] = 0;
            }
            m_say(env, RV9_STDOUT, "\n");
        }
    }
    env->close(src);
    return rc;
}
