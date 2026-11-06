/*
 * pico -- see pico.h. The arithmetic is ESP-DL's on the ESP32-P4, read from
 * its source (esp-dl 9c5fc6f); the places where that matters are said.
 *
 * ROUNDING. The P4 rounds every requantization half to even: ESP-DL sets
 * the vector unit's rounding mode to it before each kernel. Its plain C
 * path elsewhere rounds half up, which would be a different answer at
 * every exact half -- so it is half to even here, and that is not a detail.
 *
 * FLOATS. A module has none, and ESP-DL uses a few where they decide an
 * answer: the average pool multiplies by a float32 scale and rounds; the
 * picture's resize truncates i * (float)(w / dst); the face boxes are
 * float sums truncated to pixels. Exact rationals would differ at the
 * edges, so those are done in sf_t below -- float32 in integers, rounded
 * to nearest even after every operation, as the FPU does.
 *
 * Three models share it: Espressif's pedestrian detector (KIND_PICO), and
 * the two stages of its face detector (KIND_MSR proposes, KIND_MNP looks
 * closer). The layers are the same machinery; what differs is how the
 * answer is read out, and that is the last third of this file.
 */
#include "pico.h"

enum { OP_CONV = 1, OP_DW, OP_GAP, OP_LUT, OP_MUL, OP_UP2, OP_CAT, OP_PRELU };
#define F_LANES  2      /* conv weights in ESP-DL's [cout/16][k][k][cin][16] */
#define FL_BGR   1      /* the model wants blue first */
#define NO_BIAS  0xffffffffu

#define HDR      80
#define TSZ      16
#define NSZ      32
#define TOP_K    10

static uint32_t rd16(const uint8_t *b) { return b[0] | b[1] << 8; }
static uint32_t rd32(const uint8_t *b)
{
    return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}

static int8_t sat8(int32_t v) { return v < -128 ? -128 : v > 127 ? 127 : (int8_t)v; }

/* v / 2^s, rounded half to even; s > 0. Arithmetic shift is floor, so the
   remainder is never negative and one rule serves both signs. */
static int32_t rhe(int32_t v, int s)
{
    int32_t q = v >> s, r = v & ((1 << s) - 1), h = 1 << (s - 1);
    if (r > h || (r == h && (q & 1))) q++;
    return q;
}

static uint64_t rhe64(uint64_t v, int s)
{
    if (s <= 0) return v;
    if (s >= 64) return 0;
    uint64_t q = v >> s, r = v & ((1ull << s) - 1), h = 1ull << (s - 1);
    if (r > h || (r == h && (q & 1))) q++;
    return q;
}

/* ---- float32, in integers ---- */

/* m * 2^e, m < 2^24 (or 0), and a sign. Every operation rounds the way
   the FPU does: to 24 significant bits, nearest, ties to even. */
typedef struct { int neg; uint64_t m; int e; } sf_t;

static int bitlen(uint64_t v)
{
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}

static sf_t sf_round(int neg, uint64_t m, int e)
{
    sf_t r = { neg, m, e };
    int b = bitlen(m);
    if (b > 0 && b < 24) {                   /* normalized: 2^23 <= m < 2^24 */
        r.m <<= 24 - b;
        r.e -= 24 - b;
    } else if (b > 24) {
        r.m = rhe64(m, b - 24);
        r.e = e + b - 24;
        if (r.m >> 24) { r.m >>= 1; r.e++; }   /* rounded up to 2^24 */
    }
    if (r.m == 0) r.neg = 0;
    return r;
}

/* Exact for |v| < 2^24, which every caller keeps to. */
static sf_t sf_int(int32_t v, int e)
{
    return sf_round(v < 0, (uint64_t)(v < 0 ? -(int64_t)v : v), e);
}

static sf_t sf_bits(uint32_t f)         /* positive, normal */
{
    sf_t r = { 0, (f & 0x7fffff) | 0x800000, (int)((f >> 23) & 0xff) - 127 - 23 };
    return r;
}

static sf_t sf_mul(sf_t a, sf_t b)
{
    return sf_round(a.neg ^ b.neg, a.m * b.m, a.e + b.e);
}

static sf_t sf_add(sf_t a, sf_t b)
{
    if (a.m == 0) return b;
    if (b.m == 0) return a;
    if (a.e < b.e) { sf_t t = a; a = b; b = t; }  /* a has the larger exponent */
    int d = a.e - b.e;
    /* Below half of a's last place cannot change it -- with a below 2^24
       and b too, a gap of 26 is past that. */
    if (d > 26) return a;
    int64_t x = (int64_t)(a.m << d), y = (int64_t)b.m;
    int64_t s = (a.neg ? -x : x) + (b.neg ? -y : y);
    return sf_round(s < 0, (uint64_t)(s < 0 ? -s : s), b.e);
}

/* To an integer: toward zero (a C cast) or half to even (fcvt.w.s). */
static int32_t sf_int_of(sf_t a, int trunc)
{
    uint64_t v;
    if (a.e >= 0) v = a.m << a.e;
    else if (trunc) v = -a.e >= 64 ? 0 : a.m >> -a.e;
    else v = rhe64(a.m, -a.e);
    return a.neg ? -(int32_t)v : (int32_t)v;
}

/* The float32 bits of a / b, rounded to nearest even; a, b > 0, small. */
static uint32_t fdiv(uint32_t a, uint32_t b)
{
    int k = 0;                               /* q = a * 2^k / b in [2^23, 2^24) */
    while (((uint64_t)a << k) / b < (1u << 23)) k++;
    while (((uint64_t)a << k) / b >= (1u << 24)) k--;
    uint64_t n = (uint64_t)a << k;
    uint32_t q = (uint32_t)(n / b), r = (uint32_t)(n % b);
    if (2 * (uint64_t)r > b || (2 * (uint64_t)r == b && (q & 1))) q++;
    if (q == (1u << 24)) { q >>= 1; k--; }
    return (uint32_t)(127 + 23 - k) << 23 | (q & 0x7fffff);
}

/* (int)((float)a * f): one float32 multiply and a conversion. */
static int32_t fmul(int32_t a, uint32_t f, int trunc)
{
    return sf_int_of(sf_mul(sf_int(a, 0), sf_bits(f)), trunc);
}

/* ---- the model file ---- */

/* `c` channels, stored `s` apart: c rounded up to sixteen, so the vector
   unit's loads are aligned -- see mkpico.py. The input picture alone has
   s == c == 3. */
typedef struct { int h, w, c, s, exp; uint32_t off; } tinfo_t;

static tinfo_t tinfo(const pico_t *p, uint32_t t)
{
    const uint8_t *b = p->tensors + t * TSZ;
    tinfo_t r = { (int)rd16(b), (int)rd16(b + 2), (int)rd16(b + 4),
                  (int)rd16(b + 6), (int16_t)rd16(b + 8), rd32(b + 12) };
    return r;
}

int pico_stride(const pico_t *p, uint32_t t) { return tinfo(p, t).s; }

int pico_open(pico_t *p, const void *model, uint32_t size)
{
    const uint8_t *m = (const uint8_t *)model;
    if (size < HDR || m[0] != 'R' || m[1] != 'V' || m[2] != '9' || m[3] != 'M')
        return -1;
    if (rd32(m + 4) != 3) return -2;
    p->m = m;
    p->pie = 0;
    p->n_tensors = rd32(m + 8);
    p->n_nodes   = rd32(m + 12);
    p->kind      = (int)rd32(m + 20);
    p->in_h      = (int)rd32(m + 24);
    p->in_w      = (int)rd32(m + 28);
    p->bgr       = (rd32(m + 32) & FL_BGR) != 0;
    p->n_out     = rd32(m + 36);
    for (uint32_t i = 0; i < 8; i++) p->out[i] = rd32(m + 40 + 4 * i);
    p->arena     = rd32(m + 72);
    p->tensors   = m + HDR;
    p->nodes     = p->tensors + p->n_tensors * TSZ;
    uint32_t blob = (uint32_t)(p->nodes - m) + p->n_nodes * NSZ;
    blob = (blob + 15) & ~15u;
    p->blob = m + blob;
    if (blob + rd32(m + 76) > size || p->n_out > 8) return -3;
    return 0;
}

const int8_t *pico_tensor(const pico_t *p, const void *arena, uint32_t t,
                          int *h, int *w, int *c, int *exp)
{
    tinfo_t ti = tinfo(p, t);
    if (h) *h = ti.h;
    if (w) *w = ti.w;
    if (c) *c = ti.c;
    if (exp) *exp = ti.exp;
    return (const int8_t *)arena + ti.off;
}

/* ---- the picture in ---- */

/*
 * A rectangle of the picture -- all of it, or a face's square -- stretched
 * to the model's input by nearest neighbour and halved into int8: ESP-DL's
 * ImagePreprocessor with mean 0 and std 1. The source column is
 * min((int)(i * ((float)(x2 - x1) / dst)) + x1, x2 - 1): a float,
 * truncated, which is not always floor(i * w / dst) -- hence fdiv/fmul.
 * The rectangle's right and bottom are exclusive.
 */
void pico_input_rect(const pico_t *p, void *arena, const void *px, int w, int h,
                     int fmt, int x1, int y1, int x2, int y2)
{
    (void)h;
    tinfo_t ti = tinfo(p, 0);
    int8_t *out = (int8_t *)arena + ti.off;
    int dw = ti.w, dh = ti.h;
    uint32_t fx = fdiv((uint32_t)(x2 - x1), (uint32_t)dw);
    uint32_t fy = fdiv((uint32_t)(y2 - y1), (uint32_t)dh);
    uint16_t col[256];                       /* the same columns every row */
    for (int x = 0; x < dw && x < 256; x++) {
        int sx = fmul(x, fx, 1) + x1;
        col[x] = (uint16_t)(sx > x2 - 1 ? x2 - 1 : sx);
    }
    int r = p->bgr ? 2 : 0, b = p->bgr ? 0 : 2;
    for (int y = 0; y < dh; y++) {
        int sy = fmul(y, fy, 1) + y1;
        if (sy > y2 - 1) sy = y2 - 1;
        for (int x = 0; x < dw; x++) {
            int sx = col[x];
            uint32_t rgb[3];
            if (fmt == PICO_RGB565) {
                uint32_t v = ((const uint16_t *)px)[sy * w + sx];
                rgb[0] = (v & 0xf800) >> 8;      /* zero filled, not replicated */
                rgb[1] = (v & 0x07e0) >> 3;
                rgb[2] = (v & 0x001f) << 3;
            } else {
                const uint8_t *s = (const uint8_t *)px + (sy * w + sx) * 3;
                rgb[0] = s[0]; rgb[1] = s[1]; rgb[2] = s[2];
            }
            uint32_t ch[3] = { rgb[r], rgb[1], rgb[b] };
            for (int c = 0; c < 3; c++) {        /* v * 2^-1, half to even */
                uint32_t q = ch[c] >> 1;
                if ((ch[c] & 1) && (q & 1)) q++;
                *out++ = (int8_t)(q > 127 ? 127 : q);
            }
        }
    }
}

void pico_input(const pico_t *p, void *arena, const void *px, int w, int h,
                int fmt)
{
    pico_input_rect(p, arena, px, w, h, fmt, 0, 0, w, h);
}

/* ---- the layers ---- */

typedef struct {
    int k, s, pt, pl, shift, relu;
    const int8_t *w;
    const uint8_t *bias;         /* NULL: none */
} conv_t;

static void conv(const int8_t *in, tinfo_t ti, int8_t *out, tinfo_t to,
                 const conv_t *cv)
{
    int cin = ti.c, cs = ti.s, k = cv->k;
    for (int oy = 0; oy < to.h; oy++) {
        for (int ox = 0; ox < to.w; ox++) {
            int8_t *o = out + (oy * to.w + ox) * to.s;
            for (int co = 0; co < to.c; co++) {
                int32_t acc = cv->bias ? (int32_t)rd32(cv->bias + 4 * co) : 0;
                /* Sixteen output channels side by side, so one channel's
                   weights are every sixteenth byte; a tap's are the input's
                   stride of them, padding included. */
                const int8_t *f = cv->w + (co >> 4) * k * k * cs * 16 + (co & 15);
                for (int ky = 0; ky < k; ky++) {
                    int iy = oy * cv->s - cv->pt + ky;
                    if (iy < 0 || iy >= ti.h) continue;      /* zero padding */
                    for (int kx = 0; kx < k; kx++) {
                        int ix = ox * cv->s - cv->pl + kx;
                        if (ix < 0 || ix >= ti.w) continue;
                        const int8_t *a = in + (iy * ti.w + ix) * cs;
                        const int8_t *b = f + (ky * k + kx) * cs * 16;
                        for (int ci = 0; ci < cin; ci++) acc += a[ci] * b[ci * 16];
                    }
                }
                int8_t y = sat8(rhe(acc, cv->shift));
                o[co] = cv->relu && y < 0 ? 0 : y;
            }
        }
    }
}

static void dwconv(const int8_t *in, tinfo_t ti, int8_t *out, tinfo_t to,
                   const conv_t *cv)
{
    int c = ti.c, cs = ti.s, k = cv->k;
    for (int oy = 0; oy < to.h; oy++) {
        for (int ox = 0; ox < to.w; ox++) {
            int8_t *o = out + (oy * to.w + ox) * to.s;
            for (int ch = 0; ch < c; ch++) {
                int32_t acc = cv->bias ? (int32_t)rd32(cv->bias + 4 * ch) : 0;
                for (int ky = 0; ky < k; ky++) {
                    int iy = oy * cv->s - cv->pt + ky;
                    if (iy < 0 || iy >= ti.h) continue;
                    for (int kx = 0; kx < k; kx++) {
                        int ix = ox * cv->s - cv->pl + kx;
                        if (ix < 0 || ix >= ti.w) continue;
                        acc += in[(iy * ti.w + ix) * cs + ch] *
                               cv->w[(ky * k + kx) * cs + ch];
                    }
                }
                int8_t y = sat8(rhe(acc, cv->shift));
                o[ch] = cv->relu && y < 0 ? 0 : y;
            }
        }
    }
}

#ifdef PICO_PIE
/*
 * The P4's vector unit, as ESP-DL drives it (esp-dl dl_esp32p4_s8*.S): the
 * accumulator QACC holds sixteen 32-bit lanes, one per channel; the bias
 * is loaded into it, sixteen int8 multiply-adds go in per instruction, and
 * esp.srcmb shifts, rounds and saturates all sixteen to int8 on the way
 * out -- rounding by the mode in the unit's CFG register, which is set to
 * half to even first, as ESP-DL sets it. So the answer is ESP-DL's by
 * construction rather than by imitation.
 *
 * Every 16-byte load here is aligned: channel counts are multiples of 16
 * on these paths, and the model and arena start on 16 bytes.
 *
 * Nothing in here calls the system, and RV-9 switches threads only at
 * system calls -- which matters, because the context switch does not save
 * the vector registers. The caller yields between layers.
 *
 * The vector instructions name their general registers in three bits, so
 * only x8..x15 will do; every operand here is pinned to one of those.
 */
static void pie_round_half_even(void)
{
    register uint32_t cfg __asm__("a2");
    __asm__ volatile("esp.movx.r.cfg %0" : "=r"(cfg));
    cfg = (cfg & ~0xf0u) | (6u << 4);
    __asm__ volatile("esp.movx.w.cfg %0" :: "r"(cfg));
}

/* Sixteen biases into QACC, or nothing. */
static inline void pie_bias(const uint8_t *bias)
{
    if (bias == 0) {
        __asm__ volatile("esp.zero.qacc");
        return;
    }
    register const uint8_t *b __asm__("a0") = bias;
    __asm__ volatile("esp.ld.qacc.l.l.128.ip %0, 16\n"
                     "esp.ld.qacc.l.h.128.ip %0, 16\n"
                     "esp.ld.qacc.h.l.128.ip %0, 16\n"
                     "esp.ld.qacc.h.h.128.ip %0, 16\n"
                     : "+r"(b) :: "memory");
}

/* QACC to sixteen int8 at `out`: shift, round, saturate, then Relu. */
static inline void pie_out(int8_t *dst, int shift, int relu)
{
    register int     sh   __asm__("a2") = shift;
    register int     zero __asm__("a3") = 0;
    register int8_t *out  __asm__("a0") = dst;
    __asm__ volatile("esp.srcmb.s8.qacc q2, %0, 1" :: "r"(sh));
    if (relu) __asm__ volatile("esp.vrelu.s8 q2, %0, %0" :: "r"(zero));
    __asm__ volatile("esp.vst.128.ip q2, %0, 16" : "+r"(out) :: "memory");
}

#define VSM(l) "esp.vld.128.ip q1, %1, 16\n esp.vsmulas.s8.qacc q1, q0, " #l "\n"

static void pie_conv(const int8_t *in, tinfo_t ti, int8_t *out, tinfo_t to,
                     const conv_t *cv)
{
    int cin = ti.c, cs = ti.s, k = cv->k, groups = (to.c + 15) / 16;
    pie_round_half_even();
    for (int oy = 0; oy < to.h; oy++) {
        for (int ox = 0; ox < to.w; ox++) {
            for (int g = 0; g < groups; g++) {
                pie_bias(cv->bias ? cv->bias + g * 64 : 0);
                const int8_t *f = cv->w + g * k * k * cs * 16;
                for (int ky = 0; ky < k; ky++) {
                    int iy = oy * cv->s - cv->pt + ky;
                    if (iy < 0 || iy >= ti.h) continue;
                    for (int kx = 0; kx < k; kx++) {
                        int ix = ox * cv->s - cv->pl + kx;
                        if (ix < 0 || ix >= ti.w) continue;
                        register const int8_t *a __asm__("a0") =
                            in + (iy * ti.w + ix) * cs;
                        register const int8_t *b __asm__("a1") =
                            f + (ky * k + kx) * cs * 16;
                        if ((cs & 15) == 0) {
                            /* Sixteen inputs at once, each times a vector
                               of sixteen output channels' weights. The
                               padding's weights are zero, so whatever its
                               bytes hold adds nothing. */
                            for (int c = 0; c < cs; c += 16) {
                                __asm__ volatile(
                                    "esp.vld.128.ip q0, %0, 16\n"
                                    VSM(0) VSM(1) VSM(2) VSM(3) VSM(4) VSM(5)
                                    VSM(6) VSM(7) VSM(8) VSM(9) VSM(10) VSM(11)
                                    VSM(12) VSM(13) VSM(14) VSM(15)
                                    : "+r"(a), "+r"(b) :: "memory");
                            }
                        } else {
                            /* The picture's three colours, unpadded: one
                               input, broadcast, at a time. */
                            for (int c = 0; c < cin; c++) {
                                __asm__ volatile(
                                    "esp.vldbc.8.ip q0, %0, 1\n"
                                    "esp.vld.128.ip q1, %1, 16\n"
                                    "esp.vmulas.s8.qacc q0, q1\n"
                                    : "+r"(a), "+r"(b) :: "memory");
                            }
                        }
                    }
                }
                pie_out(out + (oy * to.w + ox) * to.s + g * 16, cv->shift,
                        cv->relu);
            }
        }
    }
}

static void pie_dwconv(const int8_t *in, tinfo_t ti, int8_t *out, tinfo_t to,
                       const conv_t *cv)
{
    int c = ti.s, k = cv->k;        /* the stride: padding goes along */
    pie_round_half_even();
    for (int oy = 0; oy < to.h; oy++) {
        for (int ox = 0; ox < to.w; ox++) {
            for (int g = 0; g < c; g += 16) {
                pie_bias(cv->bias ? cv->bias + g * 4 : 0);
                for (int ky = 0; ky < k; ky++) {
                    int iy = oy * cv->s - cv->pt + ky;
                    if (iy < 0 || iy >= ti.h) continue;
                    for (int kx = 0; kx < k; kx++) {
                        int ix = ox * cv->s - cv->pl + kx;
                        if (ix < 0 || ix >= ti.w) continue;
                        register const int8_t *a __asm__("a0") =
                            in + (iy * ti.w + ix) * c + g;
                        register const int8_t *b __asm__("a1") =
                            cv->w + (ky * k + kx) * c + g;
                        __asm__ volatile(
                            "esp.vld.128.ip q0, %0, 16\n"
                            "esp.vld.128.ip q1, %1, 16\n"
                            "esp.vmulas.s8.qacc q0, q1\n"
                            : "+r"(a), "+r"(b) :: "memory");
                    }
                }
                pie_out(out + (oy * to.w + ox) * c + g, cv->shift, cv->relu);
            }
        }
    }
}
#endif

void pico_node(const pico_t *p, void *arena, uint32_t i)
{
    const uint8_t *n = p->nodes + i * NSZ;
    int op = n[0];
    uint32_t i0 = rd16(n + 12), i1 = rd16(n + 14), o = rd16(n + 16);
    uint32_t woff = rd32(n + 20), boff = rd32(n + 24), aux = rd32(n + 28);
    const uint8_t *w = p->blob + woff;
    int shift = (int8_t)n[9];

    int8_t *A = (int8_t *)arena;
    tinfo_t t0 = tinfo(p, i0), to = tinfo(p, o);
    const int8_t *x = A + t0.off;
    int8_t *y = A + to.off;
    int px = to.h * to.w;

    conv_t cv = { n[2], n[3], n[4], n[6], shift, n[1],
                  (const int8_t *)w, boff == NO_BIAS ? 0 : p->blob + boff };

    switch (op) {
    case OP_CONV:
#ifdef PICO_PIE
        if (p->pie) {
            pie_conv(x, t0, y, to, &cv);
            break;
        }
#endif
        conv(x, t0, y, to, &cv);
        break;
    case OP_DW:
#ifdef PICO_PIE
        if (p->pie && (t0.s & 15) == 0) {
            pie_dwconv(x, t0, y, to, &cv);
            break;
        }
#endif
        dwconv(x, t0, y, to, &cv);
        break;
    case OP_GAP: {
        int n_ = t0.h * t0.w;
        for (int c = 0; c < t0.c; c++) {
            int32_t sum = 0;
            for (int j = 0; j < n_; j++) sum += x[j * t0.s + c];
            y[c] = sat8(fmul(sum, aux, 0));
        }
        break;
    }
    case OP_LUT:
        for (int j = 0; j < px; j++)
            for (int c = 0; c < to.c; c++)
                y[j * to.s + c] = (int8_t)w[x[j * t0.s + c] + 128];
        break;
    case OP_PRELU:
        /* Negative: times the channel's alpha, shifted back, rounded half
           to even as the vector unit is set to round. Positive: shifted
           up to the output's scale. ESP-DL's esp.vprelu.s8 does the
           negative side; its rounding is what the board check settles. */
        for (int j = 0; j < px; j++)
            for (int c = 0; c < to.c; c++) {
                int v = x[j * t0.s + c];
                y[j * to.s + c] = v < 0 ? sat8(rhe(v * (int8_t)w[c], shift))
                                        : sat8(v << aux);
            }
        break;
    case OP_MUL: {
        /* Either operand may be broadcast: [1,1,C] over every pixel, or
           a single channel across all of them. */
        tinfo_t t1 = tinfo(p, i1);
        const int8_t *x1 = A + t1.off;
        for (int yy = 0; yy < to.h; yy++)
            for (int xx = 0; xx < to.w; xx++)
                for (int c = 0; c < to.c; c++) {
                    int a = x[((t0.h == 1 ? 0 : yy) * t0.w + (t0.w == 1 ? 0 : xx))
                              * t0.s + (t0.c == 1 ? 0 : c)];
                    int bb = x1[((t1.h == 1 ? 0 : yy) * t1.w + (t1.w == 1 ? 0 : xx))
                                * t1.s + (t1.c == 1 ? 0 : c)];
                    y[(yy * to.w + xx) * to.s + c] = sat8(rhe(a * bb, shift));
                }
        break;
    }
    case OP_UP2:
        for (int yy = 0; yy < to.h; yy++)
            for (int xx = 0; xx < to.w; xx++)
                for (int c = 0; c < to.c; c++)
                    y[(yy * to.w + xx) * to.s + c] =
                        x[((yy >> 1) * t0.w + (xx >> 1)) * t0.s + c];
        break;
    case OP_CAT: {
        tinfo_t t1 = tinfo(p, i1);
        const int8_t *x1 = A + t1.off;
        for (int j = 0; j < px; j++) {
            for (int c = 0; c < t0.c; c++) y[j * to.s + c] = x[j * t0.s + c];
            for (int c = 0; c < t1.c; c++) y[j * to.s + t0.c + c] = x1[j * t1.s + c];
        }
        break;
    }
    }
}

void pico_layers(const pico_t *p, void *arena)
{
    for (uint32_t i = 0; i < p->n_nodes; i++) pico_node(p, arena, i);
}

/* ---- reading the answer ---- */

/* 2^(-k/32), Q16, for k = 0..32: e^x is done as 2^(x log2 e). */
static const uint16_t exp2_tab[33] = {
    65535, 64132, 62757, 61413, 60097, 58809, 57549, 56316, 55109, 53928,
    52773, 51642, 50535, 49452, 48393, 47356, 46341, 45348, 44376, 43425,
    42495, 41584, 40693, 39821, 38968, 38133, 37316, 36516, 35734, 34968,
    34219, 33486, 32768,
};

/* e^(d * 2^exp) in Q16, for d <= 0. */
static uint32_t expq(int32_t d, int exp)
{
    /* -x * log2(e) in Q16; log2(e) = 94548 / 65536. */
    int64_t t = ((int64_t)-d * 94548) >> -exp;
    uint32_t n = (uint32_t)(t >> 16), f = (uint32_t)(t & 0xffff);
    if (n >= 16) return 0;
    uint32_t j = f >> 11, r = f & 0x7ff;            /* 32 steps, interpolated */
    uint32_t v = exp2_tab[j] - (((exp2_tab[j] - exp2_tab[j + 1]) * r) >> 11);
    return v >> n;
}

/* Distribution focal loss: the expected bin of a softmax over eight, Q16. */
static int32_t dfl(const int8_t *q, int exp)
{
    int32_t m = q[0];
    for (int i = 1; i < 8; i++) if (q[i] > m) m = q[i];
    uint64_t sum = 0, wsum = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t e = expq(q[i] - m, exp);
        sum += e;
        wsum += (uint64_t)e * i;
    }
    return (int32_t)((wsum << 16) / sum);
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

/*
 * Best first: insert by `key`, after any equal ones -- ESP-DL's upper_bound
 * -- so ties keep the order they came in. Keeps at most `cap`.
 */
static int insert(pico_face_t *list, int n, int cap, const pico_face_t *f)
{
    if (n == cap && list[cap - 1].key >= f->key) return n;
    int at = n < cap ? n : cap - 1;
    while (at > 0 && list[at - 1].key < f->key) {
        list[at] = list[at - 1];
        at--;
    }
    list[at] = *f;
    return n < cap ? n + 1 : n;
}

/* Greedy NMS: keep the best, drop what overlaps it by more than `num/den`
   of the union; at most TOP_K. Areas are ESP-DL's, with the +1. */
static int nms(pico_face_t *c, int n, pico_face_t *out, int max)
{
    int kept = 0;
    uint8_t gone[64];
    for (int i = 0; i < n; i++) gone[i] = 0;
    for (int i = 0; i < n && kept < TOP_K && kept < max; i++) {
        if (gone[i]) continue;
        pico_box_t *a = &c[i].box;
        int32_t aa = (a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1);
        for (int j = i + 1; j < n; j++) {
            pico_box_t *b = &c[j].box;
            int32_t iw = (a->x2 < b->x2 ? a->x2 : b->x2) -
                         (a->x1 > b->x1 ? a->x1 : b->x1) + 1;
            int32_t ih = (a->y2 < b->y2 ? a->y2 : b->y2) -
                         (a->y1 > b->y1 ? a->y1 : b->y1) + 1;
            if (iw <= 0 || ih <= 0) continue;
            int32_t in_ = iw * ih;
            int32_t ab = (b->x2 - b->x1 + 1) * (b->y2 - b->y1 + 1);
            if (2 * (int64_t)in_ > aa + ab - in_) gone[j] = 1;   /* IoU > 0.5 */
        }
        out[kept++] = c[i];
    }
    return kept;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void clip(pico_face_t *f, int w, int h, int points)
{
    f->box.x1 = clampi(f->box.x1, 0, w - 1);
    f->box.y1 = clampi(f->box.y1, 0, h - 1);
    f->box.x2 = clampi(f->box.x2, 0, w - 1);
    f->box.y2 = clampi(f->box.y2, 0, h - 1);
    for (int k = 0; k < points; k++) {
        f->pt[2 * k]     = clampi(f->pt[2 * k], 0, w - 1);
        f->pt[2 * k + 1] = clampi(f->pt[2 * k + 1], 0, h - 1);
    }
}

/* -- people: Espressif's PicoPostprocessor -- */

int pico_post(const pico_t *p, void *arena, int w, int h, pico_box_t *out,
              int max)
{
    pico_face_t cand[64], kept[TOP_K];
    int nc = 0;
    for (int st = 0; st < 3; st++) {
        int sh, sw, ses, bexp;
        const int8_t *S = pico_tensor(p, arena, p->out[2 * st], &sh, &sw, 0, &ses);
        const int8_t *B = pico_tensor(p, arena, p->out[2 * st + 1], 0, 0, 0, &bexp);
        int stride = 8 << st, off = stride / 2;
        int ss = pico_stride(p, p->out[2 * st]), bs = pico_stride(p, p->out[2 * st + 1]);
        /* 0.7 * 0.7 in the score's own scale, rounded half to even. */
        uint32_t num = 49u << -ses, thr = num / 100, rem = num % 100;
        if (rem > 50 || (rem == 50 && (thr & 1))) thr++;
        for (int yy = 0; yy < sh; yy++) {
            for (int xx = 0; xx < sw; xx++) {
                int q = S[(yy * sw + xx) * ss];
                if (q <= (int)thr) continue;
                const int8_t *bb = B + (yy * sw + xx) * bs;
                int64_t cx = (int64_t)(xx * stride + off) << 16;
                int64_t cy = (int64_t)(yy * stride + off) << 16;
                pico_face_t f;
                /* (int)((c -/+ dfl * stride) * size / 224): toward zero. */
                f.box.x1 = (int32_t)((cx - (int64_t)dfl(bb, bexp) * stride) * w
                                     / ((int64_t)p->in_w << 16));
                f.box.y1 = (int32_t)((cy - (int64_t)dfl(bb + 8, bexp) * stride) * h
                                     / ((int64_t)p->in_h << 16));
                f.box.x2 = (int32_t)((cx + (int64_t)dfl(bb + 16, bexp) * stride) * w
                                     / ((int64_t)p->in_w << 16));
                f.box.y2 = (int32_t)((cy + (int64_t)dfl(bb + 24, bexp) * stride) * h
                                     / ((int64_t)p->in_h << 16));
                f.box.score = isqrt64((uint64_t)q << (32 + ses));
                f.key = (int32_t)f.box.score;
                nc = insert(cand, nc, 64, &f);
            }
        }
    }
    int n = nms(cand, nc, kept, max);
    for (int i = 0; i < n; i++) {
        clip(&kept[i], w, h, 0);
        out[i] = kept[i].box;
    }
    return n;
}

int pico_run(const pico_t *p, void *arena, const void *px, int w, int h,
             int fmt, pico_box_t *out, int max)
{
    pico_input(p, arena, px, w, h, fmt);
    pico_layers(p, arena);
    return pico_post(p, arena, w, h, out, max);
}

/* -- faces, first stage: Espressif's MSRPostprocessor -- */

/*
 * Two strides, two square anchors each; a cell is kept when its logit is
 * above 0 (the 0.5 threshold, through an inverse sigmoid, quantizes to 0).
 * The logit itself orders them: the sigmoid ESP-DL puts it through first
 * does not change which is larger. Box edges are offsets from the anchor:
 *   (int)(((c -/+ a/2) + a * q * 2^-8) * ((float)size / input)).
 */
int pico_msr_post(const pico_t *p, void *arena, int w, int h,
                  pico_face_t *out, int max)
{
    static const int anchor[2][2] = { { 16, 32 }, { 64, 128 } };
    pico_face_t cand[64];
    int nc = 0;
    sf_t fx = sf_bits(fdiv((uint32_t)w, (uint32_t)p->in_w));
    sf_t fy = sf_bits(fdiv((uint32_t)h, (uint32_t)p->in_h));
    for (int st = 0; st < 2; st++) {
        int sh, sw;
        const int8_t *S = pico_tensor(p, arena, p->out[2 * st], &sh, &sw, 0, 0);
        const int8_t *B = pico_tensor(p, arena, p->out[2 * st + 1], 0, 0, 0, 0);
        int stride = 8 << st;
        int ss = pico_stride(p, p->out[2 * st]), bs = pico_stride(p, p->out[2 * st + 1]);
        for (int yy = 0; yy < sh; yy++) {
            for (int xx = 0; xx < sw; xx++) {
                for (int a = 0; a < 2; a++) {
                    int q = S[(yy * sw + xx) * ss + a];
                    if (q <= 0) continue;
                    const int8_t *bb = B + (yy * sw + xx) * bs + 4 * a;
                    int an = anchor[st][a], cx = xx * stride + 9, cy = yy * stride + 9;
                    /* The sum before the scale is exact in float: 8 bits
                       of fraction, so it is built as an integer * 2^-8. */
                    int lo_x = (cx - (an >> 1)) * 256, hi_x = (cx + an - (an >> 1)) * 256;
                    int lo_y = (cy - (an >> 1)) * 256, hi_y = (cy + an - (an >> 1)) * 256;
                    pico_face_t f;
                    f.box.x1 = sf_int_of(sf_mul(sf_int(lo_x + an * bb[0], -8), fx), 1);
                    f.box.y1 = sf_int_of(sf_mul(sf_int(lo_y + an * bb[1], -8), fy), 1);
                    f.box.x2 = sf_int_of(sf_mul(sf_int(hi_x + an * bb[2], -8), fx), 1);
                    f.box.y2 = sf_int_of(sf_mul(sf_int(hi_y + an * bb[3], -8), fy), 1);
                    /* sigmoid(q / 16), Q16, for reading only */
                    uint32_t e = expq(-q, -4);
                    f.box.score = (uint32_t)((65536ull << 16) / (65536 + e));
                    f.key = q;
                    nc = insert(cand, nc, 64, &f);
                }
            }
        }
    }
    int n = nms(cand, nc, out, max);
    for (int i = 0; i < n; i++) clip(&out[i], w, h, 0);
    return n;
}

/* -- faces, second stage: Espressif's MNPPostprocessor, one crop -- */

/*
 * The crop was the candidate made square about its centre and clipped;
 * the outputs are relative to it, in 48ths: a box edge is
 * (int)(48 * q * 2^-8 * scale + corner) -- the far edges with 48 added
 * before the scale -- and the five points the same with 2^-7. Each step
 * is a float32 operation, rounded, so sf_t does each.
 */
static void mnp_read(const pico_t *p, void *arena, int x1, int y1, int x2,
                     int y2, pico_face_t *f)
{
    int ss, sb, sl;
    const int8_t *S = pico_tensor(p, arena, p->out[0], 0, 0, 0, &ss);
    const int8_t *B = pico_tensor(p, arena, p->out[1], 0, 0, 0, &sb);
    const int8_t *L = pico_tensor(p, arena, p->out[2], 0, 0, 0, &sl);
    sf_t fx = sf_bits(fdiv((uint32_t)(x2 - x1), (uint32_t)p->in_w));
    sf_t fy = sf_bits(fdiv((uint32_t)(y2 - y1), (uint32_t)p->in_h));
    sf_t tx = sf_int(x1, 0), ty = sf_int(y1, 0), s48 = sf_int(48, 0);
    (void)ss;

    /* 48 * (q * 2^e) is exact: an integer * 2^e. */
    f->box.x1 = sf_int_of(sf_add(sf_mul(sf_int(48 * B[0], sb), fx), tx), 1);
    f->box.y1 = sf_int_of(sf_add(sf_mul(sf_int(48 * B[1], sb), fy), ty), 1);
    f->box.x2 = sf_int_of(sf_add(sf_mul(sf_add(sf_int(48 * B[2], sb), s48), fx), tx), 1);
    f->box.y2 = sf_int_of(sf_add(sf_mul(sf_add(sf_int(48 * B[3], sb), s48), fy), ty), 1);
    for (int k = 0; k < 5; k++) {
        f->pt[2 * k]     = sf_int_of(sf_add(sf_mul(sf_int(48 * L[2 * k], sl), fx), tx), 1);
        f->pt[2 * k + 1] = sf_int_of(sf_add(sf_mul(sf_int(48 * L[2 * k + 1], sl), fy), ty), 1);
    }

    /* The winning class's softmax probability over two logits of q / 16:
       1 / (1 + e^-(|q0 - q1| / 16)), Q16. ESP-DL keeps the crop when that
       is above 0.5 -- whichever class won -- which only a tie fails. */
    int d = S[0] - S[1];
    if (d < 0) d = -d;
    uint32_t e = expq(-d, -4);
    f->box.score = (uint32_t)((65536ull << 16) / (65536 + e));
    f->key = d == 0 ? -1 : (int32_t)f->box.score;
}

int pico_faces(const pico_t *msr, const pico_t *mnp, void *arena,
               const void *px, int w, int h, int fmt, pico_face_t *out,
               int max, pico_between_t between, void *ctx)
{
    pico_face_t cand[TOP_K], found[TOP_K];
    pico_input(msr, arena, px, w, h, fmt);
    for (uint32_t i = 0; i < msr->n_nodes; i++) {
        pico_node(msr, arena, i);
        if (between) between(ctx, msr, i);
    }
    int nc = pico_msr_post(msr, arena, w, h, cand, TOP_K);

    int nf = 0;
    for (int i = 0; i < nc; i++) {
        pico_box_t *b = &cand[i].box;
        int cx = (b->x1 + b->x2) >> 1, cy = (b->y1 + b->y2) >> 1;
        int side = b->x2 - b->x1 > b->y2 - b->y1 ? b->x2 - b->x1 : b->y2 - b->y1;
        int x1 = clampi(cx - (side >> 1), 0, w - 1);
        int y1 = clampi(cy - (side >> 1), 0, h - 1);
        int x2 = clampi(cx - (side >> 1) + side, 0, w - 1);
        int y2 = clampi(cy - (side >> 1) + side, 0, h - 1);
        if (x1 >= x2 || y1 >= y2) continue;      /* ESP-DL asserts here */

        pico_input_rect(mnp, arena, px, w, h, fmt, x1, y1, x2, y2);
        for (uint32_t k = 0; k < mnp->n_nodes; k++) {
            pico_node(mnp, arena, k);
            if (between) between(ctx, mnp, k);
        }
        pico_face_t f;
        mnp_read(mnp, arena, x1, y1, x2, y2, &f);
        if (f.key < 0) continue;
        nf = insert(found, nf, TOP_K, &f);
    }
    int n = nms(found, nf, out, max);
    for (int i = 0; i < n; i++) clip(&out[i], w, h, 5);
    return n;
}

uint32_t pico_hash(const pico_t *p, const void *arena)
{
    uint32_t h = 2166136261u;                /* FNV-1a */
    for (uint32_t k = 0; k < p->n_out; k++) {
        int th, tw, tc, ts = pico_stride(p, p->out[k]);
        const int8_t *d = pico_tensor(p, arena, p->out[k], &th, &tw, &tc, 0);
        for (int i = 0; i < th * tw; i++)           /* the channels, not padding */
            for (int c = 0; c < tc; c++) {
                h ^= (uint8_t)d[i * ts + c];
                h *= 16777619u;
            }
    }
    return h;
}
