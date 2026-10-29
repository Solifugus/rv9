/*
 * snap -- take a picture with the camera and save it.
 *
 *   snap <file>             a JPEG, quality 80
 *   snap -q <1..100> <file> a JPEG at that quality
 *   snap -r <file>          the pixels themselves: 800x640 RGB565,
 *                           little-endian, top row first, a megabyte
 *
 * Says how big the picture is and how long each part took. The first
 * picture after the camera was last closed waits about half a second more
 * while its exposure finds the light.
 */
#include "modlib.h"

#define CHUNK 4000

typedef struct {
    uint8_t buf[CHUNK];
    char    name[48];
    char    word[8];
    rv9_cam_frame_t f;
} snap_statics_t;

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

    snap_statics_t *st = (snap_statics_t *)env->statics;
    if (st == 0 || env->statics_size < sizeof(*st)) return -2;

    uint32_t format = RV9_CAM_FMT_JPEG, quality = 80;
    const char *rest = env->arg ? m_word(env->arg, st->name, sizeof(st->name))
                                : "";
    if (m_eq(st->name, "-r")) {
        format = RV9_CAM_FMT_RGB565;
        rest = m_word(rest, st->name, sizeof(st->name));
    } else if (m_eq(st->name, "-q")) {
        rest = m_word(rest, st->word, sizeof(st->word));
        quality = m_num_parse(st->word, 0);
        rest = m_word(rest, st->name, sizeof(st->name));
    }
    if (st->name[0] == '\0' || st->name[0] == '-' ||
        quality < 1 || quality > 100) {
        m_say(env, RV9_STDERR,
              "usage: snap [-q 1..100 | -r] <file>\n");
        return 1;
    }

    int cam = env->open("/cam0", RV9_MODE_READ);
    if (cam < 0) {
        m_say(env, RV9_STDERR, "snap: /cam0: cannot open\n");
        return 2;
    }
    env->setstat(cam, RV9_CAM_SS_FORMAT, &format);
    env->setstat(cam, RV9_CAM_SS_QUALITY, &quality);

    /* The first read takes the picture; only then is its size known. */
    int n = env->read(cam, st->buf, CHUNK);
    if (n <= 0 || env->getstat(cam, RV9_CAM_GS_FRAME, &st->f) < 0) {
        m_say(env, RV9_STDERR, "snap: the camera failed\n");
        env->close(cam);
        return 3;
    }

    int f = env->open(st->name, RV9_MODE_WRITE | RV9_MODE_CREATE);
    if (f < 0) {
        m_say(env, RV9_STDERR, "snap: cannot create the file\n");
        env->close(cam);
        return 4;
    }
    uint32_t have = 0;
    for (;;) {
        if (env->write(f, st->buf, (uint32_t)n) != n) {
            m_say(env, RV9_STDERR, "snap: the file would not take it\n");
            env->close(f);
            env->close(cam);
            return 5;
        }
        have += (uint32_t)n;
        if (have >= st->f.size) break;
        uint32_t want = st->f.size - have;
        n = env->read(cam, st->buf, want < CHUNK ? want : CHUNK);
        if (n <= 0) break;
    }
    env->close(f);
    env->close(cam);

    m_num(env, RV9_STDOUT, st->f.width);
    m_say(env, RV9_STDOUT, "x");
    m_num(env, RV9_STDOUT, st->f.height);
    if (st->f.format == RV9_CAM_FMT_JPEG) {
        m_say(env, RV9_STDOUT, " JPEG, quality ");
        m_num(env, RV9_STDOUT, st->f.quality);
    } else {
        m_say(env, RV9_STDOUT, " RGB565");
    }
    m_say(env, RV9_STDOUT, ", ");
    m_num(env, RV9_STDOUT, (int32_t)have);
    m_say(env, RV9_STDOUT, " bytes (");
    if (st->f.start_ms) ms(env, "starting the camera ", st->f.start_ms);
    if (st->f.start_ms) m_say(env, RV9_STDOUT, ", ");
    ms(env, "taking ", st->f.capture_ms);
    if (st->f.format == RV9_CAM_FMT_JPEG) ms(env, ", compressing ", st->f.encode_ms);
    m_say(env, RV9_STDOUT, ")\n");
    return have >= st->f.size ? 0 : 6;
}
