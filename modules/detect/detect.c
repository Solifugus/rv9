/*
 * detect -- find the people, or the faces, in front of the camera.
 *
 *   detect [count]                 people: that many pictures, default 1
 *   detect faces [count]           faces, and where their eyes, nose and
 *                                  mouth are
 *   detect who [count]             faces, and whose they are
 *   detect enroll <name> [count]   learn a face: that many pictures, 5 if
 *                                  not said, while it turns a little
 *   detect known                   who has been learnt
 *   detect forget <name>           and unlearn one
 *   detect [faces] -f <file> [n]   a picture `snap -r` saved, instead
 *   detect [faces] -v ...          and the time, and an outputs hash
 *   detect [faces] [-v] -c ...     plain C even on a CPU with a vector unit
 *
 * One line per find, best first:
 *
 *   person 0.88 143,186 251,463
 *   face 0.61 347,113 639,491 eyes 433,268 548,259 nose 494,298 mouth 449,397 533,394
 *   face matthew 0.83 347,113 639,491      (`who`: the name, how alike)
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
 * WHO. Espressif's face signature model (human_face_feat mfn, 1.2 MB, in
 * /sd0/feat.rv9m) turns a face, turned and scaled to where it expects by
 * its five points, into 512 numbers; two pictures of one person give
 * similar ones. `enroll` averages a few into one signature per name and
 * keeps it in /sd0/faces.db -- a name and 512 Q15 numbers, 1 KB each.
 * `who` calls a face somebody's when its signature's cosine with theirs is
 * above 0.5, ESP-DL's line, and otherwise unknown. Nothing leaves the
 * board.
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
#define DB      "/sd0/faces.db"
#define PEOPLE  64          /* the most `who` compares against */
#define NAME    32

typedef struct {
    char    name[NAME];
    int16_t sig[PICO_SIG];  /* unit length, Q15 */
} person_t;

typedef struct {
    char        word[48];
    char        file[48];
    pico_t      p, p2, p3;
    char        name[NAME];
    pico_face_t face[10];
    pico_box_t  box[10];
    uint32_t    us[4];          /* -v: 1x1 conv, larger conv, depthwise, rest */
    rv9_sys_limits_t lim;
    /* -v for faces: per stage, by kind of layer; see between(). */
    const rv9_mod_env_t *env;
    uint64_t    last;
    uint32_t    fus[3][7];      /* stage 1, stage 2, signature */
} detect_statics_t;

enum { K_CONV_VEC, K_CONV_C, K_DW_VEC, K_DW_C, K_PRELU, K_ADD, K_OTHER, K_KINDS };

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
    else if (nd[0] == 9) kind = K_ADD;
    st->fus[p->kind == PICO_KIND_FACE1 ? 0 : p->kind == PICO_KIND_FACE2 ? 1 : 2][kind]
        += (uint32_t)(now - st->last);
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
    case K_ADD:      return "add ";
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

/* The people learnt so far, into `db`; how many, or -1 if the file is
   not one this wrote. No file is nobody. */
static int db_load(const rv9_mod_env_t *env, person_t *db, int max)
{
    int f = env->open(DB, RV9_MODE_READ);
    if (f < 0) return 0;
    uint64_t size = 0;
    env->getstat(f, RV9_GS_SIZE, &size);
    int n = (int)(size / sizeof(person_t));
    if (size % sizeof(person_t) != 0 || n > max ||
        read_all(env, f, (uint8_t *)db, (uint32_t)size) < 0) {
        env->close(f);
        return -1;
    }
    env->close(f);
    return n;
}

static int db_save(const rv9_mod_env_t *env, const person_t *db, int n)
{
    int f = env->open(DB, RV9_MODE_WRITE | RV9_MODE_CREATE);
    if (f < 0) return -1;
    int ok = n == 0 || env->write(f, db, (uint32_t)(n * sizeof(person_t))) ==
                       (int)(n * sizeof(person_t));
    env->close(f);
    return ok ? 0 : -1;
}

static int same_name(const char *a, const char *b)
{
    for (int i = 0; i < NAME; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

/* The face with the largest box, as ESP-DL's recogniser takes. */
static int largest(const pico_face_t *f, int n)
{
    int best = 0;
    for (int i = 1; i < n; i++) {
        int32_t a = (f[i].box.x2 - f[i].box.x1) * (f[i].box.y2 - f[i].box.y1);
        int32_t b = (f[best].box.x2 - f[best].box.x1) * (f[best].box.y2 - f[best].box.y1);
        if (a > b) best = i;
    }
    return best;
}

static void q16(const rv9_mod_env_t *env, int32_t v)
{
    if (v < 0) { m_say(env, RV9_STDOUT, "-"); v = -v; }
    score(env, (uint32_t)(v > 65536 ? 65536 : v));
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == 0 || env->abi_version < 15) return -1;
    detect_statics_t *st = (detect_statics_t *)env->statics;
    if (st == 0 || env->statics_size < sizeof(*st)) return -2;

    const char *rest = env->arg ? m_word(env->arg, st->word, sizeof(st->word)) : "";
    int who = m_eq(st->word, "who"), enroll = m_eq(st->word, "enroll");
    int known = m_eq(st->word, "known"), forget = m_eq(st->word, "forget");
    int faces = who || enroll || m_eq(st->word, "faces");
    if (faces || known || forget || m_eq(st->word, "people"))
        rest = m_word(rest, st->word, sizeof(st->word));
    if (enroll || forget) {
        int k = 0;
        while (st->word[k] && k < NAME - 1) { st->name[k] = st->word[k]; k++; }
        st->name[k] = '\0';
        if (k == 0 || st->word[0] == '-') {
            m_say(env, RV9_STDERR, "usage: detect enroll|forget <name>\n");
            return 1;
        }
        rest = m_word(rest, st->word, sizeof(st->word));
    }
    int verbose = m_eq(st->word, "-v");
    if (verbose) rest = m_word(rest, st->word, sizeof(st->word));
    int plain = m_eq(st->word, "-c");
    if (plain) rest = m_word(rest, st->word, sizeof(st->word));
    int from_file = m_eq(st->word, "-f");
    if (from_file) {
        rest = m_word(rest, st->file, sizeof(st->file));
        m_word(rest, st->word, sizeof(st->word));
    }
    uint32_t count = st->word[0] ? m_num_parse(st->word, 0) : enroll ? 5 : 1;
    if (count == 0 || (from_file && !st->file[0])) {
        m_say(env, RV9_STDERR, "usage: detect [faces|people|who] [-v] [-c] [-f <file>] [count]\n"
                               "       detect enroll <name> [count] | known | forget <name>\n");
        return 1;
    }

    /* The people learnt, at the very end of the arena, out of the way. */
    uint8_t *heap = (uint8_t *)(((uintptr_t)env->heap + 15) & ~(uintptr_t)15);
    uint32_t room = env->heap ? env->heap_size - 16 : 0, at = 0;
    if (room < PEOPLE * sizeof(person_t) + 2 * PICO_SIG * 4) {
        m_say(env, RV9_STDERR, "detect: no memory to work in\n");
        return 2;
    }
    room -= PEOPLE * sizeof(person_t) + 2 * PICO_SIG * 4;
    person_t *db = (person_t *)(heap + room);
    int32_t *sum = (int32_t *)(db + PEOPLE);
    int16_t *unit = (int16_t *)(sum + PICO_SIG);
    int npeople = 0;
    if (who || enroll || known || forget) {
        npeople = db_load(env, db, PEOPLE);
        if (npeople < 0) {
            m_say(env, RV9_STDERR, "detect: " DB " is not a file of faces\n");
            return 2;
        }
    }
    if (known) {
        for (int i = 0; i < npeople; i++) {
            m_say(env, RV9_STDOUT, db[i].name);
            m_say(env, RV9_STDOUT, "\n");
        }
        if (npeople == 0) m_say(env, RV9_STDOUT, "nobody has been enrolled\n");
        return 0;
    }
    if (forget) {
        int k = 0;
        for (int i = 0; i < npeople; i++)
            if (!same_name(db[i].name, st->name)) db[k++] = db[i];
        if (k == npeople) {
            m_say(env, RV9_STDERR, "detect: nobody of that name\n");
            return 5;
        }
        if (db_save(env, db, k) < 0) {
            m_say(env, RV9_STDERR, "detect: could not write " DB "\n");
            return 5;
        }
        m_say(env, RV9_STDOUT, "forgotten\n");
        return 0;
    }

    /* The models at the front of the arena, then the activations, then
       the frame. Sixteen-byte aligned throughout, for the vector unit. */
    if (faces) {
        if (load(env, "/sd0/face1.rv9m", &st->p, heap, &at, room) < 0 ||
            load(env, "/sd0/face2.rv9m", &st->p2, heap, &at, room) < 0) {
            return 2;
        }
        if ((who || enroll) &&
            load(env, "/sd0/feat.rv9m", &st->p3, heap, &at, room) < 0) {
            return 2;
        }
    } else if (load(env, "/sd0/pico.rv9m", &st->p, heap, &at, room) < 0) {
        return 2;
    }
    uint32_t need = pico_arena_bytes(&st->p);
    if (faces && pico_arena_bytes(&st->p2) > need) need = pico_arena_bytes(&st->p2);
    if ((who || enroll) && pico_arena_bytes(&st->p3) > need) need = pico_arena_bytes(&st->p3);
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
    st->p.pie = st->p2.pie = st->p3.pie = pie;
    if (enroll) {
        for (int i = 0; i < PICO_SIG; i++) sum[i] = 0;
        m_say(env, RV9_STDOUT, "look at the camera, and turn your head a little\n");
    }
    uint32_t used = 0;

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
        if (enroll) {
            if (n == 0) {
                m_say(env, RV9_STDOUT, "no face in that one\n");
                continue;
            }
            int b = largest(st->face, n);
            int8_t sig[PICO_SIG];
            pico_signature(&st->p3, arena, frame, CW, CH, PICO_RGB565,
                           st->face[b].pt, sig, env_yield, st);
            int32_t v[PICO_SIG];
            for (int k = 0; k < PICO_SIG; k++) v[k] = sig[k];
            pico_unit(v, unit);
            for (int k = 0; k < PICO_SIG; k++) sum[k] += unit[k];
            used++;
            m_say(env, RV9_STDOUT, "got it\n");
            /* Time to move between pictures: five of one pose would be
               one picture five times. */
            if (!st->file[0] && i + 1 < count) env->sleep_ms(500);
            continue;
        }
        uint64_t t2 = env->time_us();

        for (int j = 0; j < n; j++) {
            if (who) {
                const pico_face_t *f = &st->face[j];
                int8_t sig[PICO_SIG];
                st->last = env->time_us();
                pico_signature(&st->p3, arena, frame, CW, CH, PICO_RGB565,
                               f->pt, sig, verbose ? between : env_yield, st);
                int32_t v[PICO_SIG];
                for (int k = 0; k < PICO_SIG; k++) v[k] = sig[k];
                pico_unit(v, unit);
                int best = -1;
                int32_t bs = -65536;
                for (int k = 0; k < npeople; k++) {
                    int32_t sm = pico_similar(unit, db[k].sig);
                    if (sm > bs) { bs = sm; best = k; }
                }
                m_say(env, RV9_STDOUT, "face ");
                m_say(env, RV9_STDOUT, best >= 0 && bs > 32768 ? db[best].name : "unknown");
                m_say(env, RV9_STDOUT, " ");
                if (best >= 0) q16(env, bs);
                else m_say(env, RV9_STDOUT, "-");
                point(env, " ", f->box.x1, f->box.y1);
                point(env, " ", f->box.x2, f->box.y2);
            } else if (faces) {
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
        uint64_t t3 = env->time_us();
        ms(env, "(", (uint32_t)(t1 - t0));
        ms(env, " for the picture, ", (uint32_t)(t2 - t1));
        m_say(env, RV9_STDOUT, " to look at it");
        if (who && n > 0) ms(env, ", ", (uint32_t)(t3 - t2));
        if (who && n > 0) m_say(env, RV9_STDOUT, " to know it");
        m_say(env, RV9_STDOUT, ")\n");
        if (verbose) {
            m_say(env, RV9_STDOUT, "  outputs hash ");
            hex(env, pico_hash(who ? &st->p3 : faces ? &st->p2 : &st->p, arena));
            m_say(env, RV9_STDOUT, pie ? "; vector unit" : "; plain C");
            if (faces) {
                for (int sg = 0; sg < 3; sg++) {
                    m_say(env, RV9_STDOUT, sg == 2 ? "\n  signature:" : sg ? "\n  stage 2:" : "\n  stage 1:");
                    for (int k = 0; k < K_KINDS; k++) {
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

    if (enroll && rc == 0) {
        if (used == 0) {
            m_say(env, RV9_STDERR, "detect: never saw a face; nothing learnt\n");
            return 6;
        }
        pico_unit(sum, unit);
        int k = 0;
        while (k < npeople && !same_name(db[k].name, st->name)) k++;
        if (k == PEOPLE) {
            m_say(env, RV9_STDERR, "detect: no room for anybody else\n");
            return 6;
        }
        for (int i = 0; i < NAME; i++) db[k].name[i] = st->name[i];
        for (int i = 0; i < PICO_SIG; i++) db[k].sig[i] = unit[i];
        int fresh = k == npeople;
        if (db_save(env, db, fresh ? npeople + 1 : npeople) < 0) {
            m_say(env, RV9_STDERR, "detect: could not write " DB "\n");
            return 6;
        }
        m_say(env, RV9_STDOUT, fresh ? "learnt " : "learnt again: ");
        m_say(env, RV9_STDOUT, st->name);
        m_say(env, RV9_STDOUT, ", from ");
        m_num(env, RV9_STDOUT, (int32_t)used);
        m_say(env, RV9_STDOUT, used == 1 ? " picture\n" : " pictures\n");
    }
    return rc;
}
