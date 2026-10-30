/*
 * view -- what the camera sees, on the screen.
 *
 *   view [seconds]     live, until the screen is touched (or that long)
 *   view -1            one picture, left up
 *   view <file>        a picture `snap -r` saved
 *
 * Reads RGB565 frames from /cam0 eight rows at a time and draws them on
 * /w0 with RV9_SVG_SS_PIXELS, scaled to fit -- on the P4's 1024x600 panel
 * an 800x640 frame becomes 750x600, centred, by dropping one row and one
 * column in sixteen. Nearest neighbour: no arithmetic on a pixel, which is
 * what keeps a frame cheap enough to repeat.
 *
 * Live, it says how many frames it drew and how fast when it stops. A
 * touch stops it because the screen is where the person looking is; the
 * terminal cannot be asked whether a key is waiting.
 */
#include "modlib.h"

#define CW      800             /* the camera's frame */
#define CH      640
#define IN_ROWS 8
#define OUT_ROWS 8
#define MAXW    1024            /* the widest window this draws on */

typedef struct {
    char     name[48];
    uint32_t ww, wh;            /* the window */
    uint32_t ow, oh, ox, oy;    /* the picture on it */
    rv9_cam_frame_t f;
    rv9_svg_pixels_t p;
} view_statics_t;

/* Fill the window black, a band at a time. */
static void clear(const rv9_mod_env_t *env, view_statics_t *st, int w0,
                  uint16_t *zero)
{
    for (uint32_t i = 0; i < st->ww * OUT_ROWS; i++) zero[i] = 0;
    for (uint32_t y = 0; y < st->wh; y += OUT_ROWS) {
        st->p.x = 0; st->p.y = (uint16_t)y;
        st->p.w = (uint16_t)st->ww;
        st->p.h = (uint16_t)(st->wh - y < OUT_ROWS ? st->wh - y : OUT_ROWS);
        st->p.px = (uint32_t)(uintptr_t)zero;
        env->setstat(w0, RV9_SVG_SS_PIXELS, &st->p);
    }
}

/*
 * One frame from `src`, which is at its first byte, onto the window.
 * Output row oy shows camera row oy*CH/oh, column ox likewise: the rows
 * arrive in order, so each is looked at as it goes past.
 */
static int frame(const rv9_mod_env_t *env, view_statics_t *st, int src,
                 int w0, uint16_t *in, uint16_t *out)
{
    uint32_t have = 0;          /* camera rows read so far */
    uint32_t oy = 0, band = 0;  /* next output row; rows in the band */
    while (oy < st->oh) {
        uint32_t sy = oy * CH / st->oh;
        if (sy >= have) {
            /* The next eight rows, whole: reads may come back short. */
            uint32_t want = (CH - have < IN_ROWS ? CH - have : IN_ROWS) * CW * 2;
            uint32_t got = 0;
            while (got < want) {
                int n = env->read(src, (uint8_t *)in + got, want - got);
                if (n <= 0) return -1;
                got += (uint32_t)n;
            }
            have += want / (CW * 2);
            continue;
        }
        const uint16_t *row = in + (sy % IN_ROWS) * CW;
        uint16_t *o = out + band * st->ow;
        for (uint32_t x = 0; x < st->ow; x++) o[x] = row[x * CW / st->ow];
        band++;
        oy++;
        if (band == OUT_ROWS || oy == st->oh) {
            st->p.x = (uint16_t)st->ox;
            st->p.y = (uint16_t)(st->oy + oy - band);
            st->p.w = (uint16_t)st->ow;
            st->p.h = (uint16_t)band;
            st->p.px = (uint32_t)(uintptr_t)out;
            env->setstat(w0, RV9_SVG_SS_PIXELS, &st->p);
            band = 0;
        }
    }
    /* Whatever of the frame is left, so the next one starts at its top. */
    while (have < CH) {
        uint32_t want = (CH - have < IN_ROWS ? CH - have : IN_ROWS) * CW * 2;
        uint32_t got = 0;
        while (got < want) {
            int n = env->read(src, (uint8_t *)in + got, want - got);
            if (n <= 0) return -1;
            got += (uint32_t)n;
        }
        have += want / (CW * 2);
    }
    return 0;
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == 0 || env->abi_version < 15) return -1;

    view_statics_t *st = (view_statics_t *)env->statics;
    if (st == 0 || env->statics_size < sizeof(*st)) return -2;

    uint16_t *in   = (uint16_t *)env->heap;
    uint16_t *out  = in + IN_ROWS * CW;
    if (env->heap == 0 ||
        env->heap_size < (IN_ROWS * CW + OUT_ROWS * MAXW) * 2) {
        m_say(env, RV9_STDERR, "view: no memory to work in\n");
        return 2;
    }

    if (env->arg) m_word(env->arg, st->name, sizeof(st->name));
    int once = m_eq(st->name, "-1");
    int from_file = st->name[0] == '/';
    uint32_t secs = (!once && !from_file && st->name[0])
                    ? m_num_parse(st->name, 0) : 0;
    if (!once && !from_file && st->name[0] && secs == 0) {
        m_say(env, RV9_STDERR, "usage: view [seconds] | view -1 | view <file>\n");
        return 1;
    }

    int w0 = env->open("/w0", RV9_MODE_WRITE);
    if (w0 < 0) {
        m_say(env, RV9_STDERR, "view: /w0: cannot open\n");
        return 3;
    }
    uint32_t size = 0;
    env->getstat(w0, RV9_GS_SIZE, &size);
    st->ww = size & 0xffff;
    st->wh = size >> 16;
    if (st->ww == 0 || st->wh == 0 || st->ww > MAXW) {
        m_say(env, RV9_STDERR, "view: the window is not a size this draws on\n");
        env->close(w0);
        return 3;
    }
    /* As large as fits, the camera's shape kept. */
    if (st->ww * CH >= st->wh * CW) {
        st->oh = st->wh;
        st->ow = CW * st->wh / CH;
    } else {
        st->ow = st->ww;
        st->oh = CH * st->ww / CW;
    }
    st->ox = (st->ww - st->ow) / 2;
    st->oy = (st->wh - st->oh) / 2;
    clear(env, st, w0, out);

    int src, rc = 0;
    if (from_file) {
        src = env->open(st->name, RV9_MODE_READ);
        if (src < 0) {
            m_say(env, RV9_STDERR, "view: cannot open the picture\n");
            env->close(w0);
            return 4;
        }
        if (frame(env, st, src, w0, in, out) < 0) {
            m_say(env, RV9_STDERR, "view: not an 800x640 RGB565 picture\n");
            rc = 5;
        }
        env->close(src);
        env->close(w0);
        return rc;
    }

    src = env->open("/cam0", RV9_MODE_READ);
    if (src < 0) {
        m_say(env, RV9_STDERR, "view: /cam0: cannot open\n");
        env->close(w0);
        return 4;
    }
    uint32_t fmt = RV9_CAM_FMT_RGB565;
    env->setstat(src, RV9_CAM_SS_FORMAT, &fmt);

    int tp = once ? -1 : env->open("/touch", RV9_MODE_READ);
    if (!once && tp < 0 && secs == 0) secs = 30;    /* nothing to stop it */
    if (!once) {
        m_say(env, RV9_STDOUT, tp >= 0 ? "touch the screen to stop\n"
                                       : "no touch screen; stopping in 30 s\n");
    }

    uint64_t t0 = env->time_us(), t1 = t0;
    uint64_t until = secs ? t0 + (uint64_t)secs * 1000000u : 0;
    uint32_t frames = 0;
    for (;;) {
        if (frame(env, st, src, w0, in, out) < 0) {
            m_say(env, RV9_STDERR, "view: the camera failed\n");
            rc = 5;
            break;
        }
        if (frames++ == 0) t1 = env->time_us();     /* after the warm-up */
        if (once) break;
        if (until && env->time_us() >= until) break;
        uint32_t waiting = 0;
        if (tp >= 0 && env->getstat(tp, RV9_GS_READY, &waiting) == 0 &&
            waiting > 0) {
            break;
        }
    }
    if (tp >= 0) env->close(tp);
    env->close(src);
    env->close(w0);

    if (!once && frames > 1) {
        /* 32 bits of microseconds is 71 minutes; no 64-bit division here. */
        uint32_t ms = (uint32_t)(env->time_us() - t1) / 1000;
        uint32_t tenths = (frames - 1) * 10000u / (ms ? ms : 1);
        m_num(env, RV9_STDOUT, (int32_t)frames);
        m_say(env, RV9_STDOUT, " frames, ");
        m_num(env, RV9_STDOUT, (int32_t)(tenths / 10));
        m_say(env, RV9_STDOUT, ".");
        m_num(env, RV9_STDOUT, (int32_t)(tenths % 10));
        m_say(env, RV9_STDOUT, " a second after the first\n");
    }
    return rc;
}
