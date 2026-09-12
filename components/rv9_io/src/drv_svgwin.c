/*
 * svgwin -- a graphics window you draw on by writing SVG to it.
 *
 *     cat picture.svg > /w0
 *     echo '<svg><circle cx="50" cy="50" r="40" fill="red"/></svg>' > /w0
 *
 * OS-9's window devices took drawing commands written to a path, and this
 * is the same idea with a format people already have. A picture is a
 * stream of bytes, a stream of bytes is what a path carries, so a window
 * is a device you write to and nothing above needs a graphics API at all.
 *
 * SVG, and deliberately not CSS: presentation attributes only -- fill,
 * stroke, stroke-width on the elements themselves. A style language would
 * mean a cascade, a selector engine and a box model, which is an enormous
 * amount of machinery to arrive back at "this shape is red".
 *
 * The document is held as text and re-read once per band (see raster.c).
 * So the SVG source *is* the display list: there is no compiled form to
 * build, size, or keep in step with the source it came from.
 */
#include "font.h"
#include "panel.h"
#include "raster.h"

#include "rv9/io.h"
#include "rv9/kal.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "rv9-svgwin";

/* Descriptor options, by index into rv9_devdesc_t.opt:
     opt[2]  0 = portrait, 1 = landscape (agree with /term: one panel)
     opt[3]  background colour, RGB565 */
#define OPT_ROTATE 2
#define OPT_BG     3

#define SRC_MAX    4096
#define BAND_ROWS  8
#define MAX_PTS    256
#define MAX_CONTOURS 16
#define MAX_DEPTH  8

typedef struct {
    int       w, h;
    uint16_t  bg;

    char     *src;        /* the document, as written */
    size_t    len;
    bool      overflow;

    uint16_t *band;       /* w * BAND_ROWS */
    uint16_t *cov;        /* w */
    int32_t  *pts;        /* MAX_PTS * 2, 8.8 fixed */

    rv9_lock_t lock;
} svgwin_t;

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool name_is(const char *p, size_t n, const char *want)
{
    size_t i = 0;
    while (i < n && want[i] && p[i] == want[i]) i++;
    return i == n && want[i] == '\0';
}

/*
 * A number, in 8.8 fixed point.
 *
 * SVG allows "12", "1.5", "-.5" and "1e2"; exponents are not accepted here
 * because nothing writes them by hand and supporting them invites the
 * question of what else from the grammar is missing.
 */
static int32_t parse_num(const char *p, const char *end, const char **out)
{
    while (p < end && (is_space(*p) || *p == ',')) p++;

    int sign = 1;
    if (p < end && (*p == '-' || *p == '+')) { if (*p == '-') sign = -1; p++; }

    int64_t whole = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p - '0');
        if (whole > 100000) whole = 100000;
        p++;
    }

    int32_t frac = 0;
    if (p < end && *p == '.') {
        p++;
        int32_t scale = 256;
        while (p < end && *p >= '0' && *p <= '9') {
            scale /= 10;
            if (scale > 0) frac += (*p - '0') * scale;
            p++;
        }
    }

    if (out) *out = p;
    return (int32_t)sign * ((int32_t)whole * 256 + frac);
}

/* The value of one attribute of the element starting at `tag`. */
static bool attr(const char *tag, const char *tend, const char *want,
                 const char **val, size_t *vlen)
{
    size_t wl = strlen(want);
    const char *p = tag;

    while (p < tend) {
        while (p < tend && is_space(*p)) p++;
        if (p >= tend) break;

        const char *ns = p;
        while (p < tend && *p != '=' && !is_space(*p) && *p != '>' && *p != '/') p++;
        size_t nl = (size_t)(p - ns);

        while (p < tend && is_space(*p)) p++;
        if (p >= tend || *p != '=') continue;
        p++;
        while (p < tend && is_space(*p)) p++;
        if (p >= tend || (*p != '"' && *p != '\'')) continue;

        char q = *p++;
        const char *vs = p;
        while (p < tend && *p != q) p++;

        if (nl == wl && memcmp(ns, want, wl) == 0) {
            *val = vs;
            *vlen = (size_t)(p - vs);
            return true;
        }
        if (p < tend) p++;
    }
    return false;
}

static int32_t attr_num(const char *tag, const char *tend, const char *want,
                        int32_t dflt)
{
    const char *v; size_t n;
    if (!attr(tag, tend, want, &v, &n)) return dflt;
    return parse_num(v, v + n, NULL);
}

/* ------------------------------------------------------------------ */
/* Colour                                                              */
/* ------------------------------------------------------------------ */

#define NO_PAINT 0xFFFFFFFFu

static uint32_t hex_digit(char c)
{
    if (c >= '0' && c <= '9') return (uint32_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint32_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint32_t)(c - 'A' + 10);
    return 16;
}

static uint32_t rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

/*
 * A colour, or NO_PAINT.
 *
 * Hex and a short list of names. The full SVG colour list is a hundred and
 * forty entries of which a drawing on a 1.47-inch panel will use eight,
 * and the table would be larger than the parser.
 */
static uint32_t parse_colour(const char *v, size_t n)
{
    while (n && is_space(*v)) { v++; n--; }
    while (n && is_space(v[n - 1])) n--;

    if (n == 0) return NO_PAINT;

    if (*v == '#') {
        v++; n--;
        if (n >= 6) {
            uint32_t r = hex_digit(v[0]) * 16 + hex_digit(v[1]);
            uint32_t g = hex_digit(v[2]) * 16 + hex_digit(v[3]);
            uint32_t b = hex_digit(v[4]) * 16 + hex_digit(v[5]);
            return rgb565(r, g, b);
        }
        if (n >= 3) {
            uint32_t r = hex_digit(v[0]) * 17;
            uint32_t g = hex_digit(v[1]) * 17;
            uint32_t b = hex_digit(v[2]) * 17;
            return rgb565(r, g, b);
        }
        return NO_PAINT;
    }

    static const struct { const char *name; uint8_t r, g, b; } named[] = {
        { "none",    0,   0,   0   },   /* handled below */
        { "black",   0,   0,   0   },
        { "white",   255, 255, 255 },
        { "red",     255, 0,   0   },
        { "green",   0,   128, 0   },
        { "lime",    0,   255, 0   },
        { "blue",    0,   0,   255 },
        { "yellow",  255, 255, 0   },
        { "cyan",    0,   255, 255 },
        { "magenta", 255, 0,   255 },
        { "orange",  255, 165, 0   },
        { "gray",    128, 128, 128 },
        { "grey",    128, 128, 128 },
        { "silver",  192, 192, 192 },
        { "navy",    0,   0,   128 },
        { "teal",    0,   128, 128 },
        { "purple",  128, 0,   128 },
    };

    if (name_is(v, n, "none")) return NO_PAINT;

    for (size_t i = 1; i < sizeof(named) / sizeof(named[0]); i++) {
        if (name_is(v, n, named[i].name)) {
            return rgb565(named[i].r, named[i].g, named[i].b);
        }
    }
    return NO_PAINT;
}

static uint32_t attr_colour(const char *tag, const char *tend,
                            const char *want, uint32_t dflt)
{
    const char *v; size_t n;
    if (!attr(tag, tend, want, &v, &n)) return dflt;
    return parse_colour(v, n);
}

/* ------------------------------------------------------------------ */
/* Inherited state                                                     */
/* ------------------------------------------------------------------ */

/*
 * What a <g> can pass down: paint, and a transform.
 *
 * The transform is scale-and-translate only. Rotation and skew would mean
 * carrying a full matrix through the point pipeline, and every drawing
 * this device is for -- a dial, a plot, a status panel -- is built from
 * translate and scale.
 */
typedef struct {
    bool     evenodd;          /* fill-rule */
    int32_t  font_size;        /* 8.8 user units, the cell height */
    uint8_t  anchor;           /* 0 start, 1 middle, 2 end */
    uint32_t fill, stroke;
    int32_t  stroke_w;         /* 8.8 */
    int32_t  sx, sy;           /* 8.8 scale */
    int32_t  tx, ty;           /* 8.8 translate, panel pixels */
} gstate_t;

static int32_t fmul(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 8);
}

static void apply_transform(const char *v, size_t n, gstate_t *g)
{
    const char *p = v, *end = v + n;

    while (p < end) {
        while (p < end && (is_space(*p) || *p == ',')) p++;
        if (p >= end) break;

        const char *ns = p;
        while (p < end && *p != '(') p++;
        size_t nl = (size_t)(p - ns);
        while (nl && is_space(ns[nl - 1])) nl--;
        if (p >= end) break;
        p++;

        const char *args = p;
        while (p < end && *p != ')') p++;
        const char *aend = p;
        if (p < end) p++;

        const char *q = args;
        if (name_is(ns, nl, "translate")) {
            int32_t x = parse_num(q, aend, &q);
            int32_t y = (q < aend) ? parse_num(q, aend, &q) : 0;
            g->tx += fmul(x, g->sx);
            g->ty += fmul(y, g->sy);
        } else if (name_is(ns, nl, "scale")) {
            int32_t x = parse_num(q, aend, &q);
            int32_t y = (q < aend) ? parse_num(q, aend, &q) : x;
            if (y == 0) y = x;
            g->sx = fmul(g->sx, x);
            g->sy = fmul(g->sy, y);
        }
    }
}

/* User units to panel pixels, 8.8 throughout. */
static void pt(const gstate_t *g, int32_t ux, int32_t uy, int32_t *ox, int32_t *oy)
{
    *ox = g->tx + fmul(ux, g->sx);
    *oy = g->ty + fmul(uy, g->sy);
}

/* ------------------------------------------------------------------ */
/* Shapes                                                              */
/* ------------------------------------------------------------------ */

static void paint(rband_t *b, const int32_t *pts, int n, bool closed,
                  const gstate_t *g)
{
    if (g->fill != NO_PAINT && n >= 3) {
        rv9_raster_fill(b, pts, n, false, (uint16_t)g->fill);
    }
    if (g->stroke != NO_PAINT && n >= 2) {
        int32_t w = fmul(g->stroke_w, g->sx);
        rv9_raster_stroke(b, pts, n, closed, w, (uint16_t)g->stroke);
    }
}

static void do_rect(rband_t *b, const char *t, const char *te,
                    const gstate_t *g, int32_t *pts)
{
    int32_t x = attr_num(t, te, "x", 0), y = attr_num(t, te, "y", 0);
    int32_t w = attr_num(t, te, "width", 0), h = attr_num(t, te, "height", 0);
    if (w <= 0 || h <= 0) return;

    pt(g, x,     y,     &pts[0], &pts[1]);
    pt(g, x + w, y,     &pts[2], &pts[3]);
    pt(g, x + w, y + h, &pts[4], &pts[5]);
    pt(g, x,     y + h, &pts[6], &pts[7]);

    paint(b, pts, 4, true, g);
}

/*
 * A circle or ellipse, as a polygon.
 *
 * The segment count follows the radius, so a small dot does not pay for
 * sixty-four edges and a large circle does not show its corners. The
 * sine table is sixteen entries of a quarter turn, which is all a
 * fixed-point renderer at this size can tell apart.
 */
static const int16_t SIN64[17] = {
    0, 25, 50, 74, 98, 121, 142, 162, 181, 198, 213, 226, 237, 245, 251, 255, 256
};

/*
 * Sine of a fraction of a turn, angle in 1024ths, result 8.8.
 *
 * The table is interpolated rather than indexed. Taking the nearest of
 * sixty-four entries is tempting and wrong: a circle drawn with, say,
 * twenty-three segments then has its vertices at uneven angles, and comes
 * out visibly lumpy rather than round. The error is not in the radius, so
 * it survives every check that looks at size.
 */
static int32_t isin_turn(int32_t a)
{
    a &= 1023;
    int quad = (int)(a >> 8);
    int t = (int)(a & 255);
    if (quad & 1) t = 256 - t;

    int idx  = t >> 4;
    int frac = t & 15;

    int32_t v0 = SIN64[idx];
    int32_t v1 = SIN64[idx < 16 ? idx + 1 : 16];
    int32_t v  = v0 + (v1 - v0) * frac / 16;

    return (quad >= 2) ? -v : v;
}

static int32_t isin(int step, int steps)
{
    return isin_turn((int32_t)step * 1024 / steps);
}

static int32_t icos(int step, int steps)
{
    return isin_turn((int32_t)step * 1024 / steps + 256);
}

static void do_ellipse(rband_t *b, const char *t, const char *te,
                       const gstate_t *g, int32_t *pts, bool circle)
{
    int32_t cx = attr_num(t, te, "cx", 0), cy = attr_num(t, te, "cy", 0);
    int32_t rx, ry;

    if (circle) {
        rx = ry = attr_num(t, te, "r", 0);
    } else {
        rx = attr_num(t, te, "rx", 0);
        ry = attr_num(t, te, "ry", 0);
    }
    if (rx <= 0 || ry <= 0) return;

    int32_t big = fmul(rx > ry ? rx : ry, g->sx);
    int n = 12 + (int)(big >> 8) / 2;
    if (n > 48) n = 48;
    if (n > MAX_PTS) n = MAX_PTS;

    for (int i = 0; i < n; i++) {
        int32_t ux = cx + fmul(rx, icos(i, n));
        int32_t uy = cy + fmul(ry, isin(i, n));
        pt(g, ux, uy, &pts[2 * i], &pts[2 * i + 1]);
    }

    paint(b, pts, n, true, g);
}

static void do_line(rband_t *b, const char *t, const char *te,
                    const gstate_t *g, int32_t *pts)
{
    pt(g, attr_num(t, te, "x1", 0), attr_num(t, te, "y1", 0), &pts[0], &pts[1]);
    pt(g, attr_num(t, te, "x2", 0), attr_num(t, te, "y2", 0), &pts[2], &pts[3]);

    if (g->stroke == NO_PAINT) return;
    rv9_raster_stroke(b, pts, 2, false, fmul(g->stroke_w, g->sx),
                      (uint16_t)g->stroke);
}

static void do_poly(rband_t *b, const char *t, const char *te,
                    const gstate_t *g, int32_t *pts, bool closed)
{
    const char *v; size_t vn;
    if (!attr(t, te, "points", &v, &vn)) return;

    const char *p = v, *end = v + vn;
    int n = 0;

    while (p < end && n < MAX_PTS) {
        const char *before = p;
        int32_t ux = parse_num(p, end, &p);
        if (p == before) break;
        int32_t uy = parse_num(p, end, &p);
        pt(g, ux, uy, &pts[2 * n], &pts[2 * n + 1]);
        n++;
        while (p < end && (is_space(*p) || *p == ',')) p++;
    }

    if (n < 2) return;

    if (closed) {
        paint(b, pts, n, true, g);
    } else {
        if (g->fill != NO_PAINT && n >= 3) {
            rv9_raster_fill(b, pts, n, false, (uint16_t)g->fill);
        }
        if (g->stroke != NO_PAINT) {
            rv9_raster_stroke(b, pts, n, false, fmul(g->stroke_w, g->sx),
                              (uint16_t)g->stroke);
        }
    }
}


/* ------------------------------------------------------------------ */
/* <path>                                                              */
/* ------------------------------------------------------------------ */

/*
 * The `d` attribute, flattened into contours.
 *
 * Curves become line segments here and the rasteriser never learns that
 * anything was curved -- which is why adding paths needed no change to it
 * beyond letting a shape have more than one contour.
 *
 * Everything is carried in panel coordinates rather than user units. The
 * transform is scale-and-translate, so a relative step scales and does not
 * translate, and working in the destination space means the segment count
 * for a curve can be chosen from how big it will actually be drawn.
 */
typedef struct {
    int32_t    *pts;
    int         max, n;
    rcontour_t *cs;
    int         max_c, nc;
    int         start;          /* first point index of the open contour */
    int32_t     cx, cy;         /* current point */
    int32_t     ox, oy;         /* where this subpath began */
    int32_t     kx, ky;         /* previous curve's trailing control point */
    bool        have_k;
} pathbuf_t;

static void emit(pathbuf_t *pb, int32_t x, int32_t y)
{
    if (pb->n >= pb->max) return;
    pb->pts[2 * pb->n] = x;
    pb->pts[2 * pb->n + 1] = y;
    pb->n++;
    pb->cx = x;
    pb->cy = y;
}

static void end_contour(pathbuf_t *pb, bool closed)
{
    int n = pb->n - pb->start;

    if (n >= 2 && pb->nc < pb->max_c) {
        pb->cs[pb->nc].n = n;
        pb->cs[pb->nc].closed = closed;
        pb->nc++;
        pb->start = pb->n;
        return;
    }

    /*
     * Too short to be a contour, so take its points back out.
     *
     * Leaving them would be worse than wasteful: contours are consecutive
     * runs of the shared array, so a point belonging to no contour shifts
     * every contour after it by one. "M262 76 m -40 0" -- which is how a
     * circle gets written -- left the centre point stranded, and the ring
     * that followed was drawn with a wedge cut out of it to that centre.
     */
    pb->n = pb->start;
}

/* How many segments a curve of this size deserves, from the length of its
   control polygon -- a cheap bound that is never shorter than the curve. */
static int curve_steps(int32_t poly_len)
{
    int px = (int)(poly_len >> 8);
    int n = px / 4;
    if (n < 3) n = 3;
    if (n > 24) n = 24;
    return n;
}

static int32_t adist(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    int32_t dx = ax > bx ? ax - bx : bx - ax;
    int32_t dy = ay > by ? ay - by : by - ay;
    return dx + dy;     /* a taxicab bound; only the step count uses it */
}

static void cubic_to(pathbuf_t *pb, int32_t x1, int32_t y1, int32_t x2,
                     int32_t y2, int32_t x3, int32_t y3)
{
    int32_t x0 = pb->cx, y0 = pb->cy;
    int n = curve_steps(adist(x0, y0, x1, y1) + adist(x1, y1, x2, y2) +
                        adist(x2, y2, x3, y3));

    for (int i = 1; i <= n; i++) {
        int64_t t = (int64_t)i * 256 / n;
        int64_t u = 256 - t;

        int64_t xx = u * u * u * x0 + 3 * u * u * t * x1 +
                     3 * u * t * t * x2 + t * t * t * x3;
        int64_t yy = u * u * u * y0 + 3 * u * u * t * y1 +
                     3 * u * t * t * y2 + t * t * t * y3;

        emit(pb, (int32_t)(xx >> 24), (int32_t)(yy >> 24));
    }
    pb->kx = x2; pb->ky = y2; pb->have_k = true;
}

static void quad_to(pathbuf_t *pb, int32_t x1, int32_t y1, int32_t x2,
                    int32_t y2)
{
    int32_t x0 = pb->cx, y0 = pb->cy;
    int n = curve_steps(adist(x0, y0, x1, y1) + adist(x1, y1, x2, y2));

    for (int i = 1; i <= n; i++) {
        int64_t t = (int64_t)i * 256 / n;
        int64_t u = 256 - t;

        int64_t xx = u * u * x0 + 2 * u * t * x1 + t * t * x2;
        int64_t yy = u * u * y0 + 2 * u * t * y1 + t * t * y2;

        emit(pb, (int32_t)(xx >> 16), (int32_t)(yy >> 16));
    }
    pb->kx = x1; pb->ky = y1; pb->have_k = true;
}


/*
 * Elliptical arcs.
 *
 * Done by bisection rather than trigonometry. Map the ellipse to a unit
 * circle by dividing out the radii, and the midpoint of a short arc
 * between two unit vectors is simply their sum, normalised -- so halving
 * four times gives sixteen points and never needs a sine, a cosine or an
 * arctangent. On a chip with no floating point that is the difference
 * between a page of fixed-point trigonometry and thirty lines.
 *
 * x-axis-rotation is parsed and ignored. A rotated ellipse needs the full
 * endpoint-to-centre conversion with a rotation matrix; nothing that draws
 * a pie chart or a map outline asks for one.
 */
static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 40;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* Scale a vector to length 1.0, in 8.8. */
static void unitise(int32_t *x, int32_t *y)
{
    uint64_t l = isqrt64((int64_t)(*x) * (*x) + (int64_t)(*y) * (*y));
    if (l == 0) { *x = 256; *y = 0; return; }
    *x = (int32_t)(((int64_t)*x * 256) / (int64_t)l);
    *y = (int32_t)(((int64_t)*y * 256) / (int64_t)l);
}

static void arc_seg(pathbuf_t *pb, int32_t cx, int32_t cy,
                    int32_t rx, int32_t ry,
                    int32_t ax, int32_t ay, int32_t bx, int32_t by,
                    bool major, int d, int depth)
{
    if (depth == 0) {
        emit(pb, cx + (int32_t)(((int64_t)bx * rx) >> 8),
                 cy + (int32_t)(((int64_t)by * ry) >> 8));
        return;
    }

    int32_t mx = ax + bx, my = ay + by;

    if (mx == 0 && my == 0) {
        /* Exactly half a turn: the two ends give no midpoint, so take the
           perpendicular on the side we are travelling. */
        mx = -ay * d;
        my =  ax * d;
        unitise(&mx, &my);
    } else {
        unitise(&mx, &my);
        if (major) { mx = -mx; my = -my; }
    }

    arc_seg(pb, cx, cy, rx, ry, ax, ay, mx, my, false, d, depth - 1);
    arc_seg(pb, cx, cy, rx, ry, mx, my, bx, by, false, d, depth - 1);
}

static void arc_to(pathbuf_t *pb, int32_t rx, int32_t ry, bool large,
                   bool sweep, int32_t x1, int32_t y1)
{
    int32_t x0 = pb->cx, y0 = pb->cy;

    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;
    if (rx == 0 || ry == 0 || (x0 == x1 && y0 == y1)) { emit(pb, x1, y1); return; }

    int32_t dx2 = (x0 - x1) / 2, dy2 = (y0 - y1) / 2;

    /* Work in units of the radii, where the ellipse is the unit circle. */
    int32_t a = (int32_t)(((int64_t)dx2 * 256) / rx);
    int32_t b = (int32_t)(((int64_t)dy2 * 256) / ry);

    int32_t a2 = (int32_t)(((int64_t)a * a) >> 8);
    int32_t b2 = (int32_t)(((int64_t)b * b) >> 8);

    /* Radii too small to reach: the spec says grow them until they do. */
    if (a2 + b2 > 256) {
        uint32_t sq = isqrt64(((uint64_t)(a2 + b2)) << 8);
        rx = (int32_t)(((int64_t)rx * sq) >> 8);
        ry = (int32_t)(((int64_t)ry * sq) >> 8);
        a = (int32_t)(((int64_t)dx2 * 256) / rx);
        b = (int32_t)(((int64_t)dy2 * 256) / ry);
        a2 = (int32_t)(((int64_t)a * a) >> 8);
        b2 = (int32_t)(((int64_t)b * b) >> 8);
    }

    int32_t den = a2 + b2;
    if (den <= 0) { emit(pb, x1, y1); return; }

    int32_t num = 256 - den;
    if (num < 0) num = 0;

    uint32_t coef = isqrt64((((uint64_t)num << 8) / (uint32_t)den) << 8);
    int sign = (large != sweep) ? 1 : -1;

    int32_t cx = (int32_t)(sign * (int64_t)coef * (((int64_t)rx * b) >> 8) >> 8)
                 + (x0 + x1) / 2;
    int32_t cy = (int32_t)(-sign * (int64_t)coef * (((int64_t)ry * a) >> 8) >> 8)
                 + (y0 + y1) / 2;

    int32_t v0x = (int32_t)(((int64_t)(x0 - cx) * 256) / rx);
    int32_t v0y = (int32_t)(((int64_t)(y0 - cy) * 256) / ry);
    int32_t v1x = (int32_t)(((int64_t)(x1 - cx) * 256) / rx);
    int32_t v1y = (int32_t)(((int64_t)(y1 - cy) * 256) / ry);
    unitise(&v0x, &v0y);
    unitise(&v1x, &v1y);

    int d = sweep ? 1 : -1;

    int32_t cross = (int32_t)((((int64_t)v0x * v1y) - ((int64_t)v0y * v1x)) >> 8);
    int turn = (cross > 0) ? 1 : (cross < 0 ? -1 : 0);

    /* Going our way round, is this the long arc or the short one? */
    bool major = (turn != 0) ? (turn != d) : large;

    int big = (int)((rx > ry ? rx : ry) >> 8);
    int depth = (big < 16) ? 3 : (big < 64 ? 4 : 5);

    arc_seg(pb, cx, cy, rx, ry, v0x, v0y, v1x, v1y, major, d, depth);
}

/* A path flag is a single character: "010" is three of them, not ten. */
static int parse_flag(const char *p, const char *end, const char **out)
{
    while (p < end && (is_space(*p) || *p == ',')) p++;
    int v = (p < end && *p == '1') ? 1 : 0;
    if (p < end) p++;
    if (out) *out = p;
    return v;
}

static int parse_path(const char *d, size_t dn, const gstate_t *g,
                      int32_t *pts, int max_pts, rcontour_t *cs, int max_c)
{
    pathbuf_t pb;
    memset(&pb, 0, sizeof(pb));
    pb.pts = pts; pb.max = max_pts; pb.cs = cs; pb.max_c = max_c;

    const char *p = d, *end = d + dn;
    char cmd = 0;

    /* A step in user units becomes a step in panel units by scaling only:
       the translation is already in the current point. */
    #define UX(v) fmul((v), g->sx)
    #define UY(v) fmul((v), g->sy)

    while (p < end) {
        while (p < end && (is_space(*p) || *p == ',')) p++;
        if (p >= end) break;

        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            cmd = *p++;
        } else if (cmd == 0) {
            break;
        } else if (cmd == 'M') {
            cmd = 'L';        /* extra pairs after a moveto are linetos */
        } else if (cmd == 'm') {
            cmd = 'l';
        }

        bool rel = (cmd >= 'a' && cmd <= 'z');
        char c = (char)(rel ? cmd - 32 : cmd);

        int32_t a, b2, c1x, c1y, c2x, c2y, x, y;

        switch (c) {
        case 'M':
            a = parse_num(p, end, &p);
            b2 = parse_num(p, end, &p);
            end_contour(&pb, false);
            if (rel) { x = pb.cx + UX(a); y = pb.cy + UY(b2); }
            else     { pt(g, a, b2, &x, &y); }
            emit(&pb, x, y);
            pb.ox = x; pb.oy = y;
            pb.have_k = false;
            break;

        case 'L':
            a = parse_num(p, end, &p);
            b2 = parse_num(p, end, &p);
            if (rel) { x = pb.cx + UX(a); y = pb.cy + UY(b2); }
            else     { pt(g, a, b2, &x, &y); }
            emit(&pb, x, y);
            pb.have_k = false;
            break;

        case 'H':
            a = parse_num(p, end, &p);
            if (rel) x = pb.cx + UX(a);
            else     { int32_t ty; pt(g, a, 0, &x, &ty); }
            emit(&pb, x, pb.cy);
            pb.have_k = false;
            break;

        case 'V':
            a = parse_num(p, end, &p);
            if (rel) y = pb.cy + UY(a);
            else     { int32_t tx; pt(g, 0, a, &tx, &y); }
            emit(&pb, pb.cx, y);
            pb.have_k = false;
            break;

        case 'C':
            c1x = parse_num(p, end, &p); c1y = parse_num(p, end, &p);
            c2x = parse_num(p, end, &p); c2y = parse_num(p, end, &p);
            a   = parse_num(p, end, &p); b2  = parse_num(p, end, &p);
            if (rel) {
                cubic_to(&pb, pb.cx + UX(c1x), pb.cy + UY(c1y),
                              pb.cx + UX(c2x), pb.cy + UY(c2y),
                              pb.cx + UX(a),   pb.cy + UY(b2));
            } else {
                int32_t q1x, q1y, q2x, q2y, q3x, q3y;
                pt(g, c1x, c1y, &q1x, &q1y);
                pt(g, c2x, c2y, &q2x, &q2y);
                pt(g, a, b2, &q3x, &q3y);
                cubic_to(&pb, q1x, q1y, q2x, q2y, q3x, q3y);
            }
            break;

        case 'S': {
            /* The missing control point is the previous one reflected, so
               a run of S commands stays smooth without restating it. */
            int32_t r1x = pb.have_k ? 2 * pb.cx - pb.kx : pb.cx;
            int32_t r1y = pb.have_k ? 2 * pb.cy - pb.ky : pb.cy;
            c2x = parse_num(p, end, &p); c2y = parse_num(p, end, &p);
            a   = parse_num(p, end, &p); b2  = parse_num(p, end, &p);
            if (rel) {
                cubic_to(&pb, r1x, r1y, pb.cx + UX(c2x), pb.cy + UY(c2y),
                              pb.cx + UX(a), pb.cy + UY(b2));
            } else {
                int32_t q2x, q2y, q3x, q3y;
                pt(g, c2x, c2y, &q2x, &q2y);
                pt(g, a, b2, &q3x, &q3y);
                cubic_to(&pb, r1x, r1y, q2x, q2y, q3x, q3y);
            }
            break;
        }

        case 'Q':
            c1x = parse_num(p, end, &p); c1y = parse_num(p, end, &p);
            a   = parse_num(p, end, &p); b2  = parse_num(p, end, &p);
            if (rel) {
                quad_to(&pb, pb.cx + UX(c1x), pb.cy + UY(c1y),
                             pb.cx + UX(a),   pb.cy + UY(b2));
            } else {
                int32_t q1x, q1y, q2x, q2y;
                pt(g, c1x, c1y, &q1x, &q1y);
                pt(g, a, b2, &q2x, &q2y);
                quad_to(&pb, q1x, q1y, q2x, q2y);
            }
            break;

        case 'T': {
            int32_t r1x = pb.have_k ? 2 * pb.cx - pb.kx : pb.cx;
            int32_t r1y = pb.have_k ? 2 * pb.cy - pb.ky : pb.cy;
            a = parse_num(p, end, &p); b2 = parse_num(p, end, &p);
            if (rel) {
                quad_to(&pb, r1x, r1y, pb.cx + UX(a), pb.cy + UY(b2));
            } else {
                int32_t q2x, q2y;
                pt(g, a, b2, &q2x, &q2y);
                quad_to(&pb, r1x, r1y, q2x, q2y);
            }
            break;
        }

        case 'A': {
            int32_t rx = parse_num(p, end, &p);
            int32_t ry = parse_num(p, end, &p);
            (void)parse_num(p, end, &p);          /* x-axis-rotation */
            int fa = parse_flag(p, end, &p);
            int fs = parse_flag(p, end, &p);
            a  = parse_num(p, end, &p);
            b2 = parse_num(p, end, &p);

            if (rel) { x = pb.cx + UX(a); y = pb.cy + UY(b2); }
            else     { pt(g, a, b2, &x, &y); }

            arc_to(&pb, UX(rx), UY(ry), fa != 0, fs != 0, x, y);
            pb.have_k = false;
            break;
        }

        case 'Z':
            end_contour(&pb, true);
            pb.cx = pb.ox; pb.cy = pb.oy;
            pb.have_k = false;
            break;

        default:
            /* Unknown command: stop rather than misread the rest as
               coordinates and draw something confidently wrong. */
            p = end;
            break;
        }

        if (pb.n >= pb.max) break;
    }

    #undef UX
    #undef UY

    end_contour(&pb, false);
    return pb.nc;
}

static void do_path(rband_t *b, const char *t, const char *te,
                    const gstate_t *g, svgwin_t *s)
{
    const char *v; size_t vn;
    if (!attr(t, te, "d", &v, &vn)) return;

    rcontour_t cs[MAX_CONTOURS];
    int nc = parse_path(v, vn, g, s->pts, MAX_PTS, cs, MAX_CONTOURS);
    if (nc <= 0) return;

    if (g->fill != NO_PAINT) {
        rv9_raster_fill_n(b, s->pts, cs, nc, g->evenodd, (uint16_t)g->fill);
    }
    if (g->stroke != NO_PAINT) {
        rv9_raster_stroke_n(b, s->pts, cs, nc, fmul(g->stroke_w, g->sx),
                            (uint16_t)g->stroke);
    }
}


/* ------------------------------------------------------------------ */
/* <text>                                                              */
/* ------------------------------------------------------------------ */

/*
 * Text, from the console's font.
 *
 * A chart without labels is a picture of a chart, so this is not a
 * flourish. The font is the one the console uses -- 10x20 cells with four
 * bits of coverage per pixel -- which means the glyphs already carry
 * anti-aliasing and need no scanline conversion: each destination pixel
 * samples the glyph and blends by what it finds.
 *
 * Labels are usually *smaller* than the font's native twenty rows, and
 * nearest-neighbour downscaling of an anti-aliased face looks like gravel.
 * So each destination pixel takes nine samples in a 3x3 grid and averages
 * them, which costs nothing at label sizes and keeps thin strokes grey
 * rather than missing.
 *
 * One font, one size, no families: `font-family` is accepted and ignored,
 * because there is exactly one face in the system and pretending otherwise
 * would be a lie told in a parser.
 */
static void do_text(rband_t *b, const char *t, const char *te,
                    const char *str, size_t n, const gstate_t *g)
{
    if (g->fill == NO_PAINT) return;

    while (n > 0 && is_space(*str)) { str++; n--; }
    while (n > 0 && is_space(str[n - 1])) n--;
    if (n == 0) return;

    /* font-size is the cell height, which is the whole em box including
       room for descenders -- not the cap height. */
    int32_t h = fmul(g->font_size, g->sy);
    int32_t w = (int32_t)(((int64_t)h * RV9_GLYPH_W) / RV9_GLYPH_H);
    if (h <= 0 || w <= 0) return;

    int32_t x, y;
    pt(g, attr_num(t, te, "x", 0), attr_num(t, te, "y", 0), &x, &y);

    if (g->anchor != 0) {
        int32_t total = (int32_t)n * w;
        x -= (g->anchor == 1) ? total / 2 : total;
    }

    /* SVG puts y on the baseline; the cell hangs above it. */
    int32_t top = y - (int32_t)(((int64_t)h * RV9_FONT_BASELINE) / RV9_GLYPH_H);

    int py0 = (int)(top >> 8);
    int py1 = (int)((top + h + 255) >> 8);
    if (py1 <= b->y0 || py0 >= b->y0 + b->rows) return;
    if (py0 < b->y0) py0 = b->y0;
    if (py1 > b->y0 + b->rows) py1 = b->y0 + b->rows;

    for (size_t i = 0; i < n; i++) {
        int ch = (unsigned char)str[i];
        if (ch == '\n' || ch == '\t') ch = ' ';

        int32_t cx = x + (int32_t)i * w;

        int px0 = (int)(cx >> 8);
        int px1 = (int)((cx + w + 255) >> 8);
        if (px1 <= 0 || px0 >= b->w) continue;
        if (px0 < 0) px0 = 0;
        if (px1 > b->w) px1 = b->w;

        for (int py = py0; py < py1; py++) {
            for (int pxi = px0; pxi < px1; pxi++) {
                int sum = 0;

                for (int k = 0; k < 3; k++) {
                    int32_t sy = ((int32_t)py << 8) + (256 * (2 * k + 1)) / 6
                                 - top;
                    int gy = (int)(((int64_t)sy * RV9_GLYPH_H) / h);

                    for (int l = 0; l < 3; l++) {
                        int32_t sx = ((int32_t)pxi << 8)
                                     + (256 * (2 * l + 1)) / 6 - cx;
                        int gx = (int)(((int64_t)sx * RV9_GLYPH_W) / w);
                        sum += rv9_glyph_cov(ch, gx, gy);
                    }
                }

                /* Nine samples of 0..15 become one alpha of 0..255. */
                if (sum > 0) {
                    rv9_raster_pixel(b, pxi, py, (uint16_t)g->fill,
                                     sum * 255 / (9 * 15));
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* One pass over the document, for one band                            */
/* ------------------------------------------------------------------ */

static void render_band(svgwin_t *s, rband_t *b)
{
    gstate_t stack[MAX_DEPTH];
    int depth = 0;

    gstate_t base = {
        .fill = 0x0000, .stroke = NO_PAINT, .stroke_w = RV9_FIX(1),
        .font_size = RV9_FIX(16), .anchor = 0,
        .sx = 256, .sy = 256, .tx = 0, .ty = 0,
    };
    stack[0] = base;

    const char *p = s->src, *end = s->src + s->len;

    while (p < end) {
        while (p < end && *p != '<') p++;
        if (p >= end) break;
        p++;

        if (p < end && *p == '/') {
            /* A close tag: pop if it is one we pushed. */
            p++;
            const char *ns = p;
            while (p < end && *p != '>' && !is_space(*p)) p++;
            if (name_is(ns, (size_t)(p - ns), "g") && depth > 0) depth--;
            continue;
        }
        if (p < end && (*p == '?' || *p == '!')) continue;

        const char *ns = p;
        while (p < end && *p != '>' && !is_space(*p) && *p != '/') p++;
        size_t nl = (size_t)(p - ns);

        const char *tag = p;
        while (p < end && *p != '>') p++;
        const char *tend = p;
        bool self_close = (tend > tag && tend[-1] == '/');
        if (self_close) tend--;
        if (p < end) p++;

        gstate_t g = stack[depth];

        const char *v; size_t vn;
        if (attr(tag, tend, "transform", &v, &vn)) apply_transform(v, vn, &g);
        g.fill     = attr_colour(tag, tend, "fill", g.fill);
        g.stroke   = attr_colour(tag, tend, "stroke", g.stroke);
        g.stroke_w = attr_num(tag, tend, "stroke-width", g.stroke_w);

        if (attr(tag, tend, "fill-rule", &v, &vn)) {
            g.evenodd = name_is(v, vn, "evenodd");
        }

        /*
         * Text properties inherit, which is the point of putting them on a
         * <g>: a whole axis of labels shares one size and one alignment.
         * Reading them only from the element they are used on looks like it
         * works -- the text still appears -- and quietly ignores every
         * group that was meant to style it.
         */
        g.font_size = attr_num(tag, tend, "font-size", g.font_size);

        if (attr(tag, tend, "text-anchor", &v, &vn)) {
            g.anchor = name_is(v, vn, "middle") ? 1 :
                       name_is(v, vn, "end")    ? 2 : 0;
        }

        if (name_is(ns, nl, "svg")) {
            /* viewBox maps user units onto the panel, which is what lets
               the same drawing suit a phone and a 320x172 strip. */
            if (attr(tag, tend, "viewBox", &v, &vn)) {
                const char *q = v, *qe = v + vn;
                int32_t vx = parse_num(q, qe, &q);
                int32_t vy = parse_num(q, qe, &q);
                int32_t vw = parse_num(q, qe, &q);
                int32_t vh = parse_num(q, qe, &q);
                if (vw > 0 && vh > 0) {
                    int32_t sx = (int32_t)(((int64_t)s->w << 16) / vw);
                    int32_t sy = (int32_t)(((int64_t)s->h << 16) / vh);
                    g.sx = sx; g.sy = sy;
                    g.tx = -fmul(vx, sx);
                    g.ty = -fmul(vy, sy);
                }
            }
            g.fill = (g.fill == 0x0000) ? 0x0000 : g.fill;
            stack[0] = g;
            continue;
        }

        if (name_is(ns, nl, "g")) {
            if (!self_close && depth + 1 < MAX_DEPTH) stack[++depth] = g;
            continue;
        }

        if (name_is(ns, nl, "text")) {
            /* The content is what follows the tag, up to the next one. */
            const char *cs = p;
            while (p < end && *p != '<') p++;
            do_text(b, tag, tend, cs, (size_t)(p - cs), &g);
            continue;
        }

        if (name_is(ns, nl, "rect"))          do_rect(b, tag, tend, &g, s->pts);
        else if (name_is(ns, nl, "circle"))   do_ellipse(b, tag, tend, &g, s->pts, true);
        else if (name_is(ns, nl, "ellipse"))  do_ellipse(b, tag, tend, &g, s->pts, false);
        else if (name_is(ns, nl, "line"))     do_line(b, tag, tend, &g, s->pts);
        else if (name_is(ns, nl, "polygon"))  do_poly(b, tag, tend, &g, s->pts, true);
        else if (name_is(ns, nl, "polyline")) do_poly(b, tag, tend, &g, s->pts, false);
        else if (name_is(ns, nl, "path"))     do_path(b, tag, tend, &g, s);
    }
}

static void render(svgwin_t *s)
{
    rband_t b = { .w = s->w, .rows = BAND_ROWS, .px = s->band, .cov = s->cov };

    uint64_t t0 = rv9_time_us();

    /* Ours now. The console will repaint itself in full whenever it next
       has something to say, which is what makes the picture stay up in
       the meantime rather than being eaten a row at a time. */
    rv9_panel_take(s);

    for (int y = 0; y < s->h; y += BAND_ROWS) {
        b.y0 = y;
        b.rows = (y + BAND_ROWS <= s->h) ? BAND_ROWS : s->h - y;

        rv9_raster_clear(&b, s->bg);
        render_band(s, &b);
        rv9_raster_to_panel(&b);

        rv9_panel_blit(0, y, s->w, y + b.rows, s->band);
    }

    ESP_LOGI(TAG, "drew %u bytes in %u ms", (unsigned)s->len,
             (unsigned)((rv9_time_us() - t0) / 1000));
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */

static rv9_io_err_t svgwin_init(rv9_dev_t *dev)
{
    svgwin_t *s = rv9_calloc(1, sizeof(*s));
    if (s == NULL) return RV9_IO_ERR_NOMEM;

    rv9_io_err_t err = rv9_panel_open(dev->opt[OPT_ROTATE] != 0, &s->w, &s->h);
    if (err != RV9_IO_OK) { rv9_free(s); return err; }

    s->bg = (uint16_t)dev->opt[OPT_BG];

    if (rv9_lock_create(&s->lock) != RV9_OK) { rv9_free(s); return RV9_IO_ERR_NOMEM; }

    dev->drv_state = s;
    ESP_LOGI(TAG, "window up: %dx%d", s->w, s->h);
    return RV9_IO_OK;
}

/*
 * The buffers live only while somebody has the window open.
 *
 * Between them they are about nine kilobytes on a board with forty free,
 * so holding them from boot to pay for a device that is used occasionally
 * would be the wrong trade. This is what the driver open and close hooks
 * were for.
 */
static rv9_io_err_t svgwin_open(rv9_dev_t *dev, uint32_t mode)
{
    (void)mode;
    svgwin_t *s = (svgwin_t *)dev->drv_state;
    if (s == NULL) return RV9_IO_ERR_IO;

    s->src  = rv9_alloc(SRC_MAX);
    s->band = rv9_alloc_dma((size_t)s->w * BAND_ROWS * sizeof(uint16_t));
    s->cov  = rv9_alloc((size_t)s->w * sizeof(uint16_t));
    s->pts  = rv9_alloc(MAX_PTS * 2 * sizeof(int32_t));

    if (s->src == NULL || s->band == NULL || s->cov == NULL || s->pts == NULL) {
        rv9_free(s->src);  s->src = NULL;
        rv9_free(s->band); s->band = NULL;
        rv9_free(s->cov);  s->cov = NULL;
        rv9_free(s->pts);  s->pts = NULL;
        return RV9_IO_ERR_NOMEM;
    }

    s->len = 0;
    s->overflow = false;
    return RV9_IO_OK;
}

static rv9_io_err_t svgwin_close(rv9_dev_t *dev)
{
    svgwin_t *s = (svgwin_t *)dev->drv_state;
    if (s == NULL) return RV9_IO_OK;

    rv9_free(s->src);  s->src = NULL;
    rv9_free(s->band); s->band = NULL;
    rv9_free(s->cov);  s->cov = NULL;
    rv9_free(s->pts);  s->pts = NULL;
    return RV9_IO_OK;
}

/*
 * Collect the document, and draw when it ends.
 *
 * "</svg>" is the signal rather than the close of the path, so a program
 * can hold the window open and send one picture after another -- which is
 * what anything animated wants, and costs nothing to allow.
 */
static rv9_io_err_t svgwin_write(rv9_dev_t *dev, const void *buf, size_t len,
                                 size_t *done)
{
    svgwin_t *s = (svgwin_t *)dev->drv_state;
    if (s == NULL || s->src == NULL) return RV9_IO_ERR_IO;

    const char *in = (const char *)buf;

    rv9_lock_acquire(s->lock);

    for (size_t i = 0; i < len; i++) {
        if (s->len < SRC_MAX) {
            s->src[s->len++] = in[i];
        } else {
            s->overflow = true;
        }

        if (s->len >= 6 && memcmp(&s->src[s->len - 6], "</svg>", 6) == 0) {
            if (s->overflow) {
                ESP_LOGW(TAG, "document over %d bytes; drew what fitted",
                         SRC_MAX);
            }
            render(s);
            s->len = 0;
            s->overflow = false;
        }
    }

    rv9_lock_release(s->lock);

    if (done) *done = len;
    return RV9_IO_OK;
}

static rv9_io_err_t svgwin_getstat(rv9_dev_t *dev, uint32_t code, void *arg)
{
    svgwin_t *s = (svgwin_t *)dev->drv_state;

    if (code == RV9_GS_SIZE && arg && s) {
        /* Pixels, not characters: this is not a console. */
        *(uint32_t *)arg = ((uint32_t)s->h << 16) | (uint32_t)s->w;
        return RV9_IO_OK;
    }
    return RV9_IO_ERR_UNSUPPORTED;
}

static const rv9_driver_t svgwin_driver = {
    .name    = "svgwin",
    .init    = svgwin_init,
    .open    = svgwin_open,
    .close   = svgwin_close,
    .write   = svgwin_write,
    .getstat = svgwin_getstat,
};

rv9_io_err_t rv9_drv_svgwin_register(void)
{
    return rv9_io_register_driver(&svgwin_driver);
}
