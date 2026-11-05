/*
 * mystify -- the old screen saver, in SVG.
 *
 *   mystify [seconds]      until the screen is touched, or that long
 *
 * Two four-cornered shapes bounce about the window, each trailing fading
 * copies of where it has been, their colours drifting round the wheel.
 * Every frame is one SVG document written to /w0 -- a black background and
 * a dozen polygons -- which is the whole demonstration: the program knows
 * nothing about pixels, and the same bytes would draw anywhere.
 *
 * No floating point, as in every module: positions are 8.8 fixed point and
 * the colour wheel is six straight segments.
 */
#include "modlib.h"

#define SHAPES   2
#define CORNERS  4
#define TRAIL    7          /* copies of each shape, newest first */
#define SPACING  2          /* frames between trail copies */
#define DOC_MAX  2400

typedef struct { int32_t x, y, dx, dy; } pt_t;

typedef struct {
    pt_t     p[SHAPES][CORNERS];
    int16_t  trail[SHAPES][TRAIL][CORNERS][2];
    uint16_t hue[SHAPES];
    int      trail_n;        /* experiment: copies drawn */
    uint32_t seed;
    char     doc[DOC_MAX];
    uint32_t len;
} mystify_statics_t;

static uint32_t rnd(mystify_statics_t *st)
{
    st->seed = st->seed * 1103515245u + 12345u;
    return st->seed >> 8;
}

static void add(mystify_statics_t *st, const char *s)
{
    while (*s && st->len < DOC_MAX - 1) st->doc[st->len++] = *s++;
}

static void add_num(mystify_statics_t *st, int32_t v)
{
    char b[12];
    int i = 11;
    b[i] = '\0';
    int neg = v < 0;
    uint32_t u = neg ? (uint32_t)-v : (uint32_t)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    add(st, &b[i]);
}

/* Hue 0..1535 round the wheel, at full saturation, dimmed by k/256. */
static void add_colour(mystify_statics_t *st, uint32_t hue, uint32_t k)
{
    uint32_t seg = hue / 256, f = hue % 256, r, g, b;
    switch (seg) {
    case 0:  r = 255;     g = f;       b = 0;       break;
    case 1:  r = 255 - f; g = 255;     b = 0;       break;
    case 2:  r = 0;       g = 255;     b = f;       break;
    case 3:  r = 0;       g = 255 - f; b = 255;     break;
    case 4:  r = f;       g = 0;       b = 255;     break;
    default: r = 255;     g = 0;       b = 255 - f; break;
    }
    r = r * k >> 8; g = g * k >> 8; b = b * k >> 8;
    static const char hex[] = "0123456789abcdef";
    char c[8] = { '#', hex[r >> 4], hex[r & 15], hex[g >> 4], hex[g & 15],
                  hex[b >> 4], hex[b & 15], '\0' };
    add(st, c);
}

static void step(mystify_statics_t *st, int w, int h)
{
    for (int s = 0; s < SHAPES; s++) {
        for (int c = 0; c < CORNERS; c++) {
            pt_t *p = &st->p[s][c];
            p->x += p->dx;
            p->y += p->dy;
            if (p->x < 0)          { p->x = 0;          p->dx = -p->dx; }
            if (p->x > (w - 1) << 8) { p->x = (w - 1) << 8; p->dx = -p->dx; }
            if (p->y < 0)          { p->y = 0;          p->dy = -p->dy; }
            if (p->y > (h - 1) << 8) { p->y = (h - 1) << 8; p->dy = -p->dy; }
        }
        st->hue[s] = (uint16_t)((st->hue[s] + 3) % 1536);
    }
}

/* The newest position into the trail, the oldest falling off the end. */
static void remember(mystify_statics_t *st)
{
    for (int s = 0; s < SHAPES; s++) {
        for (int t = TRAIL - 1; t > 0; t--)
            for (int c = 0; c < CORNERS; c++) {
                st->trail[s][t][c][0] = st->trail[s][t - 1][c][0];
                st->trail[s][t][c][1] = st->trail[s][t - 1][c][1];
            }
        for (int c = 0; c < CORNERS; c++) {
            st->trail[s][0][c][0] = (int16_t)(st->p[s][c].x >> 8);
            st->trail[s][0][c][1] = (int16_t)(st->p[s][c].y >> 8);
        }
    }
}

static void build(mystify_statics_t *st, int w, int h)
{
    (void)w; (void)h;
    /* No background: the window clears every band to its own colour, black
       on this board, before drawing -- a full-screen rect would paint all
       614,400 pixels again for nothing. */
    st->len = 0;
    add(st, "<svg>");
    /* Oldest first, so the newest is drawn on top. */
    for (int t = st->trail_n - 1; t >= 0; t--) {
        for (int s = 0; s < SHAPES; s++) {
            add(st, "<polygon fill=\"none\" stroke-width=\"");
            add(st, t == 0 ? "3" : "2");
            add(st, "\" stroke=\"");
            /* Fading: full for the newest, down to a fifth for the oldest,
               and the hue a little behind, so a trail is a small rainbow. */
            add_colour(st, (st->hue[s] + 1536 - t * 20) % 1536,
                       256 - t * 200 / TRAIL);
            add(st, "\" points=\"");
            for (int c = 0; c < CORNERS; c++) {
                add_num(st, st->trail[s][t][c][0]);
                add(st, ",");
                add_num(st, st->trail[s][t][c][1]);
                add(st, c < CORNERS - 1 ? " " : "");
            }
            add(st, "\"/>");
        }
    }
    add(st, "</svg>");
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == 0 || env->abi_version < 9) return -1;
    mystify_statics_t *st = (mystify_statics_t *)env->statics;
    if (st == 0 || env->statics_size < sizeof(*st)) return -2;

    char word[12];
    word[0] = '\0';
    const char *rest = env->arg ? m_word(env->arg, word, sizeof(word)) : "";
    uint32_t secs = word[0] ? m_num_parse(word, 0) : 0;
    m_word(rest, word, sizeof(word));
    st->trail_n = word[0] ? (int)m_num_parse(word, 0) : TRAIL;

    int w0 = env->open("/w0", RV9_MODE_WRITE);
    if (w0 < 0) {
        m_say(env, RV9_STDERR, "mystify: /w0: cannot open\n");
        return 2;
    }
    uint32_t size = 0;
    env->getstat(w0, RV9_GS_SIZE, &size);
    int w = (int)(size & 0xffff), h = (int)(size >> 16);
    if (w < 16 || h < 16) {
        m_say(env, RV9_STDERR, "mystify: the window has no size\n");
        env->close(w0);
        return 2;
    }

    st->seed = (uint32_t)env->time_us() | 1;
    for (int s = 0; s < SHAPES; s++) {
        st->hue[s] = (uint16_t)(s * 768);
        for (int c = 0; c < CORNERS; c++) {
            pt_t *p = &st->p[s][c];
            p->x = (int32_t)(rnd(st) % (uint32_t)w) << 8;
            p->y = (int32_t)(rnd(st) % (uint32_t)h) << 8;
            /* 2 to 7 pixels a frame, either way. */
            p->dx = (int32_t)(512 + rnd(st) % 1280) * (rnd(st) & 1 ? 1 : -1);
            p->dy = (int32_t)(512 + rnd(st) % 1280) * (rnd(st) & 1 ? 1 : -1);
        }
    }
    for (int t = 0; t < TRAIL; t++) remember(st);

    int tp = env->open("/touch", RV9_MODE_READ);
    if (tp < 0 && secs == 0) secs = 30;      /* nothing else would stop it */
    m_say(env, RV9_STDOUT, tp >= 0 ? "touch the screen to stop\n"
                                   : "no touch screen; stopping in 30 s\n");

    uint64_t t0 = env->time_us();
    uint64_t until = secs ? t0 + (uint64_t)secs * 1000000u : 0;
    uint32_t frames = 0;
    for (;;) {
        for (int k = 0; k < SPACING; k++) step(st, w, h);
        remember(st);
        build(st, w, h);
        if (env->write(w0, st->doc, st->len) < 0) break;
        frames++;
        if (until && env->time_us() >= until) break;
        uint32_t waiting = 0;
        if (tp >= 0 && env->getstat(tp, RV9_GS_READY, &waiting) == 0 &&
            waiting > 0) {
            break;
        }
        env->yield();
    }
    if (tp >= 0) env->close(tp);
    env->close(w0);

    uint32_t ms = (uint32_t)(env->time_us() - t0) / 1000;
    m_num(env, RV9_STDOUT, (int32_t)frames);
    m_say(env, RV9_STDOUT, " frames, ");
    uint32_t tenths = frames * 10000u / (ms ? ms : 1);
    m_num(env, RV9_STDOUT, (int32_t)(tenths / 10));
    m_say(env, RV9_STDOUT, ".");
    m_num(env, RV9_STDOUT, (int32_t)(tenths % 10));
    m_say(env, RV9_STDOUT, " a second\n");
    return 0;
}
