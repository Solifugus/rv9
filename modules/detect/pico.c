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

enum { OP_CONV = 1, OP_DW, OP_GAP, OP_LUT, OP_MUL, OP_UP2, OP_CAT, OP_PRELU, OP_ADD };
#define F_LANES  2      /* conv weights in ESP-DL's [cout/16][k][k][cin][16] */
#define F_PRELU  4      /* and the PReLU after it done here, as it stores:
                           aux its alphas, byte 10 its shift down, 11 up */
#define F_PATCH  8      /* three channels in, k*k*3 <= 32, and 128 zero
                           bytes after the weights: see pie_conv */
#define FL_BGR   1      /* the model wants blue first */
#define FL_CENTRED 2    /* and its input as (v - 127.5) / 127.5 */
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

/* a / b, rounded once. The quotient carries six bits more than it keeps
   and a sticky bit for any remainder, which is enough to round right. */
static sf_t sf_div(sf_t a, sf_t b)
{
    if (a.m == 0 || b.m == 0) { sf_t z = { 0, 0, 0 }; return z; }
    uint64_t n = a.m << 30;
    uint64_t q = n / b.m;
    if (n % b.m) q |= 1;
    return sf_round(a.neg ^ b.neg, q, a.e - b.e - 30);
}

/* a * r + c with one rounding -- fmaf, which ESP-DL's -ffast-math build
   contracts this expression to. r is a small integer, so a * r is exact. */
static sf_t sf_fma_int(sf_t a, int r, sf_t c)
{
    sf_t p = { a.neg ^ (r < 0), a.m * (uint64_t)(r < 0 ? -r : r), a.e };
    if (p.m == 0) return c;
    if (c.m == 0) return sf_round(p.neg, p.m, p.e);
    return sf_add(p, c);
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
    /* 4 is 3 with F_PRELU; the engine reads both. */
    if (rd32(m + 4) != 3 && rd32(m + 4) != 4) return -2;
    p->m = m;
    p->pie = 0;
    p->n_tensors = rd32(m + 8);
    p->n_nodes   = rd32(m + 12);
    p->kind      = (int)rd32(m + 20);
    p->in_h      = (int)rd32(m + 24);
    p->in_w      = (int)rd32(m + 28);
    p->bgr       = (rd32(m + 32) & FL_BGR) != 0;
    p->centred   = (rd32(m + 32) & FL_CENTRED) != 0;
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
/*
 * A pixel's channel to the model's int8. Either v * 2^-1, half to even --
 * the detectors' -- or rne((v - 127.5) / 127.5 * 64) for the face
 * signature's, which over 0..255 is exactly the floor below; there are no
 * ties to round.
 */
static int8_t quant_in(const pico_t *p, uint32_t v)
{
    if (p->centred) return (int8_t)((2 * (128 * (int32_t)v - 16320) + 255 + 32640) / 510 - 64);
    uint32_t q = v >> 1;
    if ((v & 1) && (q & 1)) q++;
    return (int8_t)(q > 127 ? 127 : q);
}

/* One pixel of the picture as red, green, blue. */
static void pixel(const void *px, int w, int fmt, int x, int y, uint32_t rgb[3])
{
    if (fmt == PICO_RGB565) {
        uint32_t v = ((const uint16_t *)px)[y * w + x];
        rgb[0] = (v & 0xf800) >> 8;          /* zero filled, not replicated */
        rgb[1] = (v & 0x07e0) >> 3;
        rgb[2] = (v & 0x001f) << 3;
    } else {
        const uint8_t *s = (const uint8_t *)px + (y * w + x) * 3;
        rgb[0] = s[0]; rgb[1] = s[1]; rgb[2] = s[2];
    }
}

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
            uint32_t rgb[3];
            pixel(px, w, fmt, col[x], sy, rgb);
            uint32_t ch[3] = { rgb[r], rgb[1], rgb[b] };
            for (int c = 0; c < 3; c++) *out++ = quant_in(p, ch[c]);
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
    const int8_t *alpha;         /* a PReLU folded in (F_PRELU); NULL: none */
    int pdown, pup;              /* its shifts, as OP_PRELU's */
    int patch;                   /* F_PATCH */
    int lanes;                   /* F_LANES: a depthwise conv's weights are
                                    [c/16][k][k][16], not [k][k][c] */
} conv_t;

/* One value through a PReLU, exactly as OP_PRELU does it below. */
static int8_t prelu1(int v, int a, int down, int up)
{
    return v < 0 ? sat8(rhe(v * a, down)) : sat8(v << up);
}

/* What a layer stores: its own Relu, or the PReLU folded into it. */
static int8_t act(const conv_t *cv, int8_t y, int ch)
{
    if (cv->alpha) return prelu1(y, cv->alpha[ch], cv->pdown, cv->pup);
    return cv->relu && y < 0 ? 0 : y;
}

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
                o[co] = act(cv, sat8(rhe(acc, cv->shift)), co);
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
                        int t = ky * k + kx;
                        acc += in[(iy * ti.w + ix) * cs + ch] *
                               cv->w[cv->lanes ? ((ch >> 4) * k * k + t) * 16 + (ch & 15)
                                               : t * cs + ch];
                    }
                }
                o[ch] = act(cv, sat8(rhe(acc, cv->shift)), ch);
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

/*
 * The same, then the PReLU that followed the layer, on the sixteen values
 * before they are stored rather than on the whole tensor after: the
 * tensor's round trip through external memory was most of what PReLU
 * cost. The instructions are pie_prelu's, below, in the same order.
 */
static inline void pie_out_prelu(int8_t *dst, int shift, const int8_t *alpha,
                                 int down, int up)
{
    uint8_t scale = (uint8_t)(1 << up);
    register int8_t        *out __asm__("a0") = dst;
    register const int8_t  *al  __asm__("a1") = alpha;
    register const uint8_t *sc  __asm__("a5") = &scale;
    register int            sh  __asm__("a2") = shift;
    register int            sn  __asm__("a3") = down;
    register int            sp  __asm__("a4") = 0;
    __asm__ volatile(
        "esp.srcmb.s8.qacc q0, %3, 1\n"
        "esp.zero.q q2\n"
        "esp.vldbc.8.ip q3, %2, 0\n"
        "esp.vld.128.ip q1, %1, 16\n"
        "esp.vprelu.s8 q4, q0, q1, %4\n"
        "esp.vcmp.lt.s8 q6, q0, q2\n"
        "esp.andq q4, q4, q6\n"
        "esp.zero.qacc\n"
        "esp.vmulas.s8.qacc q0, q3\n"
        "esp.srcmb.s8.qacc q5, %5, 1\n"
        "esp.vcmp.gt.s8 q6, q0, q2\n"
        "esp.andq q5, q5, q6\n"
        "esp.vadd.s8 q4, q4, q5\n"
        "esp.vst.128.ip q4, %0, 16\n"
        : "+r"(out), "+r"(al), "+r"(sc)
        : "r"(sh), "r"(sn), "r"(sp) : "memory");
}

static inline void pie_store(int8_t *dst, const conv_t *cv, int ch)
{
    if (cv->alpha) pie_out_prelu(dst, cv->shift, cv->alpha + ch, cv->pdown, cv->pup);
    else pie_out(dst, cv->shift, cv->relu);
}

/* Sixteen inputs (q0), each times a row of sixteen output channels'
   weights: ESP-DL's 11c16 pattern, two weight registers taking turns, each
   multiply-accumulate loading the row after next as it goes -- one
   instruction per sixteen multiply-adds instead of two. */
#define VSMF(q, l) "esp.vsmulas.s8.qacc.ld.incp " q ", %1, " q ", q0, " #l "\n"
#define VSM16 \
    "esp.vld.128.ip q0, %0, 16\n" \
    "esp.vld.128.ip q1, %1, 16\n" \
    "esp.vld.128.ip q3, %1, 16\n" \
    VSMF("q1", 0) VSMF("q3", 1) VSMF("q1", 2) VSMF("q3", 3) \
    VSMF("q1", 4) VSMF("q3", 5) VSMF("q1", 6) VSMF("q3", 7) \
    VSMF("q1", 8) VSMF("q3", 9) VSMF("q1", 10) VSMF("q3", 11) \
    VSMF("q1", 12) VSMF("q3", 13) \
    "esp.vsmulas.s8.qacc q1, q0, 14\n" \
    "esp.vsmulas.s8.qacc q3, q0, 15\n"

/*
 * The first layer, over the picture: three channels a pixel, so the path
 * below would multiply one byte at a time, 27 times for a 3x3 patch, and
 * it was the slowest layer in the recognition model (18 ms). Instead the
 * patch is gathered once a pixel into 32 aligned bytes -- three runs of
 * nine, as the picture is stored, the edges zero -- and each group of
 * sixteen outputs is two VSM16s. The weights are already in that order,
 * [ky][kx][c][16], so the patch's lane i meets weight row i. Lanes 27..31
 * are zero, and the rows VSM16 reads for them, and two ahead, are the next
 * group's or the 128 zero bytes the converter put after the last: they
 * add nothing. The sums are the same integers in another order.
 */
static void pie_conv_patch(const int8_t *in, tinfo_t ti, int8_t *out,
                           tinfo_t to, const conv_t *cv)
{
    int k = cv->k, groups = (to.c + 15) / 16;
    int8_t patch[32] __attribute__((aligned(16)));
    for (int q = 0; q < 32; q++) patch[q] = 0;   /* past k*k*3 stays zero */
    pie_round_half_even();
    for (int oy = 0; oy < to.h; oy++) {
        for (int ox = 0; ox < to.w; ox++) {
            int n = 0;
            int iy0 = oy * cv->s - cv->pt, ix0 = ox * cv->s - cv->pl;
            if (k == 3 && iy0 >= 0 && iy0 + 2 < ti.h && ix0 >= 0 && ix0 + 2 < ti.w) {
                /* Inside the picture: three rows of nine bytes, no tests. */
                const int8_t *r = in + (iy0 * ti.w + ix0) * 3;
                for (int ky = 0; ky < 3; ky++, r += ti.w * 3) {
                    for (int q = 0; q < 9; q++) patch[n + q] = r[q];
                    n += 9;
                }
            } else for (int ky = 0; ky < k; ky++) {
                int iy = oy * cv->s - cv->pt + ky;
                for (int kx = 0; kx < k; kx++) {
                    int ix = ox * cv->s - cv->pl + kx;
                    if (iy < 0 || iy >= ti.h || ix < 0 || ix >= ti.w) {
                        patch[n] = patch[n + 1] = patch[n + 2] = 0;
                    } else {
                        const int8_t *a = in + (iy * ti.w + ix) * 3;
                        patch[n] = a[0]; patch[n + 1] = a[1]; patch[n + 2] = a[2];
                    }
                    n += 3;
                }
            }
            for (int g = 0; g < groups; g++) {
                pie_bias(cv->bias ? cv->bias + g * 64 : 0);
                register const int8_t *a __asm__("a0") = patch;
                register const int8_t *b __asm__("a1") = cv->w + g * k * k * 3 * 16;
                __asm__ volatile(VSM16 VSM16 : "+r"(a), "+r"(b) :: "memory");
                pie_store(out + (oy * to.w + ox) * to.s + g * 16, cv, g * 16);
            }
        }
    }
}

/*
 * A 1x1 conv, stride 1, no padding: every output pixel is its input pixel
 * times the weights, and the weights, [g][cin][16], run on from one group
 * of sixteen outputs to the next. ESP-DL's dl_esp32p4_s8_conv2d_11c16,
 * instruction for instruction: the hardware loop (esp.lp.setup) over all
 * but the last sixteen inputs, each multiply-add loading the weight row
 * after next, and the block's last one loading the next sixteen inputs.
 * `*w` is left at the next group's weights.
 */
static inline void pie_c11(const int8_t *x, const int8_t **w, int cdiv1)
{
    register const int8_t *a __asm__("a0") = x;
    register const int8_t *b __asm__("a1") = *w;
    register int           n __asm__("a2") = cdiv1;
#define VSL(q, l) "esp.vsmulas.s8.qacc.ld.incp " q ", %1, " q ", q0, " #l "\n"
    __asm__ volatile(
        "esp.vld.128.ip q0, %0, 16\n"
        "esp.vld.128.ip q1, %1, 16\n"
        "esp.vld.128.ip q3, %1, 16\n"
        "beqz %2, 1f\n"
        "esp.lp.setup 0, %2, 0f\n"
        VSL("q1", 0) VSL("q3", 1) VSL("q1", 2) VSL("q3", 3)
        VSL("q1", 4) VSL("q3", 5) VSL("q1", 6) VSL("q3", 7)
        VSL("q1", 8) VSL("q3", 9) VSL("q1", 10) VSL("q3", 11)
        VSL("q1", 12) VSL("q3", 13) VSL("q1", 14)
        "esp.vsmulas.s8.qacc.ld.incp q0, %0, q3, q0, 15\n"
        "0: esp.vld.128.ip q3, %1, 16\n"
        "1:\n"
        VSL("q1", 0) VSL("q3", 1) VSL("q1", 2) VSL("q3", 3)
        VSL("q1", 4) VSL("q3", 5) VSL("q1", 6) VSL("q3", 7)
        VSL("q1", 8) VSL("q3", 9) VSL("q1", 10) VSL("q3", 11)
        VSL("q1", 12) VSL("q3", 13)
        "esp.vsmulas.s8.qacc q1, q0, 14\n"
        "esp.vsmulas.s8.qacc q3, q0, 15\n"
        : "+r"(a), "+r"(b), "+r"(n) :: "memory");
#undef VSL
    *w = b;
}

static void pie_conv11(const int8_t *in, tinfo_t ti, int8_t *out, tinfo_t to,
                       const conv_t *cv)
{
    int cs = ti.s, groups = (to.c + 15) / 16, cdiv1 = cs / 16 - 1;
    int px = to.h * to.w;
    pie_round_half_even();
    for (int j = 0; j < px; j++) {
        const int8_t *x = in + j * cs, *w = cv->w;
        int8_t *o = out + j * to.s;
        for (int g = 0; g < groups; g++) {
            pie_bias(cv->bias ? cv->bias + g * 64 : 0);
            pie_c11(x, &w, cdiv1);
            pie_store(o + g * 16, cv, g * 16);
        }
    }
}

static void pie_conv(const int8_t *in, tinfo_t ti, int8_t *out, tinfo_t to,
                     const conv_t *cv)
{
    int cin = ti.c, cs = ti.s, k = cv->k, groups = (to.c + 15) / 16;
    if (cv->patch && cs == 3 && k * k * 3 <= 32) {
        pie_conv_patch(in, ti, out, to, cv);
        return;
    }
    if (k == 1 && cv->s == 1 && cv->pt == 0 && cv->pl == 0 &&
        (cs & 15) == 0 && ti.h == to.h && ti.w == to.w) {
        pie_conv11(in, ti, out, to, cv);
        return;
    }
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
                                __asm__ volatile(VSM16
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
                pie_store(out + (oy * to.w + ox) * to.s + g * 16, cv, g * 16);
            }
        }
    }
}

/*
 * PReLU, sixteen channels at a time, as ESP-DL's dl_esp32p4_s8_prelu_11c:
 * the negative lanes by esp.vprelu (times the channel's alpha, shifted,
 * rounded as CFG says -- half to even), the positive ones through QACC
 * times 2^up, the two kept by masks and added. Strides are sixteens.
 */
static void pie_prelu(const int8_t *in, tinfo_t ti, int8_t *out, tinfo_t to,
                      const int8_t *alpha, int down, int up)
{
    uint8_t scale = (uint8_t)(1 << up);
    pie_round_half_even();
    {
        register const uint8_t *sp __asm__("a0") = &scale;
        __asm__ volatile("esp.zero.q q2\n"
                         "esp.vldbc.8.ip q3, %0, 0\n" : "+r"(sp) :: "memory");
    }
    int px = to.h * to.w;
    for (int j = 0; j < px; j++) {
        for (int g = 0; g < ti.s; g += 16) {
            register const int8_t *x  __asm__("a0") = in + j * ti.s + g;
            register const int8_t *al __asm__("a1") = alpha + g;
            register int8_t       *y  __asm__("a2") = out + j * to.s + g;
            register int           sn __asm__("a3") = down;
            register int           sp __asm__("a4") = 0;
            __asm__ volatile(
                "esp.vld.128.ip q0, %0, 16\n"
                "esp.vld.128.ip q1, %1, 16\n"
                "esp.vprelu.s8 q4, q0, q1, %3\n"
                "esp.vcmp.lt.s8 q6, q0, q2\n"
                "esp.andq q4, q4, q6\n"
                "esp.zero.qacc\n"
                "esp.vmulas.s8.qacc q0, q3\n"
                "esp.srcmb.s8.qacc q5, %4, 1\n"
                "esp.vcmp.gt.s8 q6, q0, q2\n"
                "esp.andq q5, q5, q6\n"
                "esp.vadd.s8 q4, q4, q5\n"
                "esp.vst.128.ip q4, %2, 16\n"
                : "+r"(x), "+r"(al), "+r"(y)
                : "r"(sn), "r"(sp) : "memory");
        }
    }
}

/* Add, sixteen at a time: esp.vadd.s8, saturating, as ESP-DL's. When the
   three strides agree -- in these models they always do -- the tensors are
   one run of bytes each, and the hardware loop goes straight down them. */
static void pie_add(const int8_t *a, tinfo_t ta, const int8_t *b, tinfo_t tb,
                    int8_t *out, tinfo_t to)
{
    int px = to.h * to.w;
    if (ta.s == to.s && tb.s == to.s && px * to.s >= 16) {
        register const int8_t *x __asm__("a0") = a;
        register const int8_t *z __asm__("a1") = b;
        register int8_t       *y __asm__("a2") = out;
        register int           n __asm__("a3") = px * to.s / 16;
        __asm__ volatile(
            "esp.lp.setup 0, %3, 0f\n"
            "esp.vld.128.ip q0, %0, 16\n"
            "esp.vld.128.ip q1, %1, 16\n"
            "esp.vadd.s8 q2, q0, q1\n"
            "0: esp.vst.128.ip q2, %2, 16\n"
            : "+r"(x), "+r"(z), "+r"(y), "+r"(n) :: "memory");
        return;
    }
    for (int j = 0; j < px; j++) {
        for (int g = 0; g < to.s; g += 16) {
            register const int8_t *x __asm__("a0") = a + j * ta.s + g;
            register const int8_t *z __asm__("a1") = b + j * tb.s + g;
            register int8_t       *y __asm__("a2") = out + j * to.s + g;
            __asm__ volatile(
                "esp.vld.128.ip q0, %0, 16\n"
                "esp.vld.128.ip q1, %1, 16\n"
                "esp.vadd.s8 q2, q0, q1\n"
                "esp.vst.128.ip q2, %2, 16\n"
                : "+r"(x), "+r"(z), "+r"(y) :: "memory");
        }
    }
}

/*
 * A 3x3 depthwise conv's sixteen channels at a pixel whose nine taps are
 * all inside the picture: ESP-DL's dl_esp32p4_s8_depthwise_conv2d_33s1
 * sequence, each multiply-add loading the next tap's weights as it goes
 * and each input loaded two instructions before it is used. `x` is the
 * top-left tap; `dx` steps a pixel right, `dy` from a row's third tap to
 * the next row's first. The weights are this group's nine rows (F_LANES).
 */
static inline void pie_dw33(const int8_t *x, const int8_t *w, int dx, int dy)
{
    register const int8_t *a  __asm__("a0") = x;
    register const int8_t *b  __asm__("a1") = w;
    register int           sx __asm__("a2") = dx;
    register int           sy __asm__("a3") = dy;
    __asm__ volatile(
        "esp.vld.128.xp q0, %0, %2\n"
        "esp.vld.128.ip q1, %1, 16\n"
        "esp.vld.128.xp q2, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.xp q0, %0, %3\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vld.128.xp q2, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.xp q0, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vld.128.xp q2, %0, %3\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.xp q0, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vld.128.xp q2, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.ip q0, %0, 0\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vmulas.s8.qacc q0, q1\n"
        : "+r"(a), "+r"(b) : "r"(sx), "r"(sy) : "memory");
}

/* The same for 5x5, the people detector's: twenty-five taps, generated
   by the rule pie_dw33 follows (and checked to give pie_dw33 for 3x3). */
static inline void pie_dw55(const int8_t *x, const int8_t *w, int dx, int dy)
{
    register const int8_t *a  __asm__("a0") = x;
    register const int8_t *b  __asm__("a1") = w;
    register int           sx __asm__("a2") = dx;
    register int           sy __asm__("a3") = dy;
    __asm__ volatile(
        "esp.vld.128.xp q0, %0, %2\n"
        "esp.vld.128.ip q1, %1, 16\n"
        "esp.vld.128.xp q2, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.xp q0, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vld.128.xp q2, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.xp q0, %0, %3\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vld.128.xp q2, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.xp q0, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vld.128.xp q2, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.xp q0, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vld.128.xp q2, %0, %3\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.xp q0, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vld.128.xp q2, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.xp q0, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vld.128.xp q2, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.xp q0, %0, %3\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vld.128.xp q2, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.xp q0, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vld.128.xp q2, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.xp q0, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vld.128.xp q2, %0, %3\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.xp q0, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vld.128.xp q2, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.xp q0, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vld.128.xp q2, %0, %2\n"
        "esp.vmulas.s8.qacc.ld.ip q3, %1, 16, q0, q1\n"
        "esp.vld.128.ip q0, %0, 0\n"
        "esp.vmulas.s8.qacc.ld.ip q1, %1, 16, q2, q3\n"
        "esp.vmulas.s8.qacc q0, q1\n"
        : "+r"(a), "+r"(b) : "r"(sx), "r"(sy) : "memory");
}

static void pie_dwconv(const int8_t *in, tinfo_t ti, int8_t *out, tinfo_t to,
                       const conv_t *cv)
{
    int c = ti.s, k = cv->k;        /* the stride: padding goes along */
    int fast = cv->lanes && (k == 3 || k == 5);
    pie_round_half_even();
    for (int oy = 0; oy < to.h; oy++) {
        int iy0 = oy * cv->s - cv->pt;
        for (int ox = 0; ox < to.w; ox++) {
            int ix0 = ox * cv->s - cv->pl;
            if (fast && iy0 >= 0 && iy0 + k - 1 < ti.h && ix0 >= 0 &&
                ix0 + k - 1 < ti.w) {
                const int8_t *x = in + (iy0 * ti.w + ix0) * c;
                int dy = (ti.w - (k - 1)) * c;
                for (int g = 0; g < c; g += 16) {
                    pie_bias(cv->bias ? cv->bias + g * 4 : 0);
                    if (k == 3) pie_dw33(x + g, cv->w + g * 9, c, dy);
                    else        pie_dw55(x + g, cv->w + g * 25, c, dy);
                    pie_store(out + (oy * to.w + ox) * c + g, cv, g);
                }
                continue;
            }
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
                        register const int8_t *b __asm__("a1") = cv->lanes
                            ? cv->w + (g * k * k + (ky * k + kx) * 16)
                            : cv->w + (ky * k + kx) * c + g;
                        __asm__ volatile(
                            "esp.vld.128.ip q0, %0, 16\n"
                            "esp.vld.128.ip q1, %1, 16\n"
                            "esp.vmulas.s8.qacc q0, q1\n"
                            : "+r"(a), "+r"(b) :: "memory");
                    }
                }
                pie_store(out + (oy * to.w + ox) * c + g, cv, g);
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
                  (const int8_t *)w, boff == NO_BIAS ? 0 : p->blob + boff,
                  (n[8] & F_PRELU) ? (const int8_t *)p->blob + aux : 0,
                  (int8_t)n[10], n[11], (n[8] & F_PATCH) != 0,
                  (n[8] & F_LANES) != 0 };

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
#ifdef PICO_PIE
        if (p->pie && (t0.s & 15) == 0 && to.s == t0.s) {
            pie_prelu(x, t0, y, to, (const int8_t *)w, shift, (int)aux);
            break;
        }
#endif
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
    case OP_ADD: {                      /* same exponents: see mkpico.py */
        tinfo_t t1 = tinfo(p, i1);
        const int8_t *x1 = A + t1.off;
#ifdef PICO_PIE
        if (p->pie && ((t0.s | t1.s | to.s) & 15) == 0) {
            pie_add(x, t0, x1, t1, y, to);
            break;
        }
#endif
        for (int j = 0; j < px; j++)
            for (int c = 0; c < to.c; c++)
                y[j * to.s + c] = sat8(x[j * t0.s + c] + x1[j * t1.s + c]);
        break;
    }
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

/* ---- who: Espressif's face signature (human_face_feat mfn) ---- */

/* Where a face's five points go in the 112x112 picture the signature
   model was trained on: left eye, left mouth, nose, right eye, right
   mouth, as float32 -- ESP-DL's template, dl_feat_image_preprocessor.cpp. */
static const uint32_t s_template[10] = {
    0x42192dac, 0x424ec903, 0x4226327c, 0x42b8bb23, 0x426019ce,
    0x428f7924, 0x42931048, 0x424e016f, 0x428d75b5, 0x42b86880,
};

/*
 * The similarity transform -- turn, scale, shift -- taking the template's
 * points nearest the face's, by least squares, as a 2x3 matrix from the
 * template's coordinates to the picture's:
 *
 *   [ a  -b  tx ]      a = sum(s.d) / sum(s.s),  b = sum(s x d) / sum(s.s)
 *   [ b   a  ty ]      over the points less their means
 *
 * the closed form of the Umeyama fit ESP-DL computes through a 2x2 SVD.
 * Here in float32 throughout; ESP-DL mixes float, double and a float
 * Newton square root, and the two agree to about one part in 10^7 --
 * usually the same warp, now and then a pixel different.
 */
static void align(const int32_t pt[10], sf_t M[6])
{
    sf_t five = sf_int(5, 0), smx = sf_int(0, 0), smy = smx, dmx = smx, dmy = smx;
    for (int i = 0; i < 5; i++) {
        smx = sf_add(smx, sf_bits(s_template[2 * i]));
        smy = sf_add(smy, sf_bits(s_template[2 * i + 1]));
        dmx = sf_add(dmx, sf_int(pt[2 * i], 0));
        dmy = sf_add(dmy, sf_int(pt[2 * i + 1], 0));
    }
    smx = sf_div(smx, five); smy = sf_div(smy, five);
    dmx = sf_div(dmx, five); dmy = sf_div(dmy, five);

    sf_t na = sf_int(0, 0), nb = na, den = na;
    for (int i = 0; i < 5; i++) {
        sf_t sx = sf_add(sf_bits(s_template[2 * i]), (sf_t){ !smx.neg, smx.m, smx.e });
        sf_t sy = sf_add(sf_bits(s_template[2 * i + 1]), (sf_t){ !smy.neg, smy.m, smy.e });
        sf_t dx = sf_add(sf_int(pt[2 * i], 0), (sf_t){ !dmx.neg, dmx.m, dmx.e });
        sf_t dy = sf_add(sf_int(pt[2 * i + 1], 0), (sf_t){ !dmy.neg, dmy.m, dmy.e });
        na  = sf_add(na, sf_add(sf_mul(sx, dx), sf_mul(sy, dy)));
        sf_t c = sf_mul(sy, dx);
        nb  = sf_add(nb, sf_add(sf_mul(sx, dy), (sf_t){ !c.neg, c.m, c.e }));
        den = sf_add(den, sf_add(sf_mul(sx, sx), sf_mul(sy, sy)));
    }
    sf_t a = sf_div(na, den), b = sf_div(nb, den);
    sf_t nbb = { !b.neg, b.m, b.e };
    /* tx = dmx - (a smx - b smy), ty = dmy - (b smx + a smy) */
    sf_t u = sf_add(sf_mul(a, smx), sf_mul(nbb, smy));
    sf_t v = sf_add(sf_mul(b, smx), sf_mul(a, smy));
    M[0] = a;   M[1] = nbb; M[2] = sf_add(dmx, (sf_t){ !u.neg, u.m, u.e });
    M[3] = b;   M[4] = a;   M[5] = sf_add(dmy, (sf_t){ !v.neg, v.m, v.e });
}

void pico_align(const int32_t pt[10], int32_t m_q16[6])
{
    sf_t M[6];
    align(pt, M);
    for (int i = 0; i < 6; i++) m_q16[i] = sf_int_of(sf_mul(M[i], sf_int(65536, 0)), 0);
}

/*
 * The face into the model's 112x112: for each output pixel, the picture's
 * pixel M says, nearest -- ESP-DL's warp, in its Q10 fixed point. Each
 * column's and each row's share of the position is worked out once,
 * rounded half to even (lrintf), and a sample is their sum rounded:
 * (x1 + x2 + 512) >> 10. Outside the picture is black.
 */
static void warp_input(const pico_t *p, void *arena, const void *px, int w,
                       int h, int fmt, const sf_t M[6])
{
    tinfo_t ti = tinfo(p, 0);
    int8_t *out = (int8_t *)arena + ti.off;
    int n = ti.w;
    int32_t x1[128], y1[128], x2[128], y2[128];
    sf_t k = sf_int(1024, 0);
    for (int i = 0; i < n && i < 128; i++) {
        x1[i] = sf_int_of(sf_mul(sf_mul(M[0], sf_int(i, 0)), k), 0);
        y1[i] = sf_int_of(sf_mul(sf_mul(M[3], sf_int(i, 0)), k), 0);
        x2[i] = sf_int_of(sf_mul(sf_fma_int(M[1], i, M[2]), k), 0);
        y2[i] = sf_int_of(sf_mul(sf_fma_int(M[4], i, M[5]), k), 0);
    }
    int8_t black = quant_in(p, 0);
    int r = p->bgr ? 2 : 0, b = p->bgr ? 0 : 2;
    for (int row = 0; row < ti.h; row++) {
        for (int c = 0; c < n; c++) {
            int x = (x1[c] + x2[row] + 512) >> 10;
            int y = (y1[c] + y2[row] + 512) >> 10;
            if ((unsigned)x < (unsigned)w && (unsigned)y < (unsigned)h) {
                uint32_t rgb[3];
                pixel(px, w, fmt, x, y, rgb);
                *out++ = quant_in(p, rgb[r]);
                *out++ = quant_in(p, rgb[1]);
                *out++ = quant_in(p, rgb[b]);
            } else {
                *out++ = black; *out++ = black; *out++ = black;
            }
        }
    }
}

int pico_signature(const pico_t *p, void *arena, const void *px, int w, int h,
                   int fmt, const int32_t pt[10], int8_t sig[PICO_SIG],
                   pico_between_t between, void *ctx)
{
    sf_t M[6];
    align(pt, M);
    warp_input(p, arena, px, w, h, fmt, M);
    for (uint32_t i = 0; i < p->n_nodes; i++) {
        pico_node(p, arena, i);
        if (between) between(ctx, p, i);
    }
    int th, tw, tc;
    const int8_t *e = pico_tensor(p, arena, p->out[0], &th, &tw, &tc, 0);
    if (th * tw * tc != PICO_SIG) return -1;
    for (int i = 0; i < PICO_SIG; i++) sig[i] = e[i];
    return 0;
}

/* Unit length, in Q15: what is kept and compared. */
void pico_unit(const int32_t *v, int16_t out[PICO_SIG])
{
    uint64_t ss = 0;
    for (int i = 0; i < PICO_SIG; i++) ss += (uint64_t)((int64_t)v[i] * v[i]);
    uint32_t n = isqrt64(ss);
    for (int i = 0; i < PICO_SIG; i++)
        out[i] = n ? (int16_t)(((int64_t)v[i] * 32767 + (v[i] < 0 ? -(int64_t)n / 2 : (int64_t)n / 2)) / n) : 0;
}

/* Cosine of two unit signatures: 65536 is the same face exactly. */
int32_t pico_similar(const int16_t a[PICO_SIG], const int16_t b[PICO_SIG])
{
    int64_t d = 0;
    for (int i = 0; i < PICO_SIG; i++) d += (int32_t)a[i] * b[i];
    return (int32_t)(d >> 14);
}
