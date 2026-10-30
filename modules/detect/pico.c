/*
 * pico -- see pico.h. The arithmetic is ESP-DL's on the ESP32-P4, read from
 * its source (esp-dl 9c5fc6f); the places where that matters are said.
 *
 * ROUNDING. The P4 rounds every requantization half to even: ESP-DL sets
 * the vector unit's rounding mode to it before each kernel. Its plain C
 * path elsewhere rounds half up, which would be a different answer at
 * every exact half -- so it is half to even here, and that is not a detail.
 *
 * THE ONE FLOAT. Global average pooling multiplies the sum by a float32
 * scale, 2^(in-out) * (float)(1/n), and rounds that float to an integer.
 * 1/49 is not exact in float, so neither is the answer, and an exact
 * rational would differ at the edges. A module has no floating point, so
 * fmul() below does that one float32 multiply in integers, bit for bit.
 * The resize of the input picture uses the same, for the same reason.
 */
#include "pico.h"

enum { OP_CONV = 1, OP_DW, OP_GAP, OP_LUT, OP_MUL, OP_UP2, OP_CAT };
#define F_LANES  2      /* conv weights in ESP-DL's [cout/16][k][k][cin][16] */

#define HDR      56
#define TSZ      12
#define NSZ      32
#define IN_SIZE  224
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

/*
 * (int)((float)a * f), where f is a float32 given as its bits: the product
 * rounded to float32 as the FPU would, then to an integer -- half to even
 * (fcvt.w.s, ESP-DL's tool::round) or toward zero (a C cast). |a| < 2^24,
 * so (float)a is exact; f is positive and normal.
 */
static int32_t fmul(int32_t a, uint32_t f, int trunc)
{
    if (a == 0) return 0;
    int neg = a < 0;
    uint64_t p = (uint64_t)(neg ? -a : a) * ((f & 0x7fffff) | 0x800000);
    int e = (int)((f >> 23) & 0xff) - 127 - 23;
    int bits = 64 - __builtin_clzll(p);
    if (bits > 24) {                         /* round to float32 */
        p = rhe64(p, bits - 24);
        e += bits - 24;
    }
    if (e >= 0) {
        p <<= e;
    } else if (trunc) {
        p = -e >= 64 ? 0 : p >> -e;
    } else {
        p = rhe64(p, -e);
    }
    return neg ? -(int32_t)p : (int32_t)p;
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

/* ---- the model file ---- */

typedef struct { int h, w, c, exp; uint32_t off; } tinfo_t;

static tinfo_t tinfo(const pico_t *p, uint32_t t)
{
    const uint8_t *b = p->tensors + t * TSZ;
    tinfo_t r = { (int)rd16(b), (int)rd16(b + 2), (int)rd16(b + 4),
                  (int16_t)rd16(b + 6), rd32(b + 8) };
    return r;
}

int pico_open(pico_t *p, const void *model, uint32_t size)
{
    const uint8_t *m = (const uint8_t *)model;
    if (size < HDR || m[0] != 'R' || m[1] != 'V' || m[2] != '9' || m[3] != 'M')
        return -1;
    if (rd32(m + 4) != 1) return -2;
    p->m = m;
    p->pie = 0;
    p->n_tensors = rd32(m + 8);
    p->n_nodes   = rd32(m + 12);
    p->arena     = rd32(m + 44);
    p->tensors   = m + HDR;
    p->nodes     = p->tensors + p->n_tensors * TSZ;
    uint32_t blob = (uint32_t)(p->nodes - m) + p->n_nodes * NSZ;
    blob = (blob + 15) & ~15u;
    p->blob = m + blob;
    if (blob + rd32(m + 48) > size) return -3;
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
 * Stretched to 224x224 by nearest neighbour, red first, and halved into
 * int8: ESP-DL's ImagePreprocessor with mean 0 and std 1. The source
 * column is (int)(i * ((float)w / 224)) -- a float, truncated, which is
 * not always floor(i * w / 224) -- hence fdiv and fmul.
 */
void pico_input(const pico_t *p, void *arena, const void *px, int w, int h,
                int fmt)
{
    tinfo_t ti = tinfo(p, 0);
    int8_t *out = (int8_t *)arena + ti.off;
    uint32_t fx = fdiv((uint32_t)w, IN_SIZE), fy = fdiv((uint32_t)h, IN_SIZE);
    uint16_t col[IN_SIZE];           /* the same 224 columns every row */
    for (int x = 0; x < IN_SIZE; x++) {
        int sx = fmul(x, fx, 1);
        col[x] = (uint16_t)(sx > w - 1 ? w - 1 : sx);
    }
    for (int y = 0; y < IN_SIZE; y++) {
        int sy = fmul(y, fy, 1);
        if (sy > h - 1) sy = h - 1;
        for (int x = 0; x < IN_SIZE; x++) {
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
            for (int c = 0; c < 3; c++) {        /* v * 2^-1, half to even */
                uint32_t q = rgb[c] >> 1;
                if ((rgb[c] & 1) && (q & 1)) q++;
                *out++ = (int8_t)(q > 127 ? 127 : q);
            }
        }
    }
}

/* ---- the layers ---- */

static void conv(const int8_t *in, tinfo_t ti, int8_t *out, tinfo_t to,
                 const int8_t *wt, const uint8_t *bias, int k, int s, int pad,
                 int shift, int relu, int lanes)
{
    int cin = ti.c;
    for (int oy = 0; oy < to.h; oy++) {
        for (int ox = 0; ox < to.w; ox++) {
            for (int co = 0; co < to.c; co++) {
                int32_t acc = (int32_t)rd32(bias + 4 * co);
                /* Lanes: sixteen output channels side by side, so one
                   channel's weights are every sixteenth byte. */
                const int8_t *f = lanes ? wt + (co >> 4) * k * k * cin * 16 + (co & 15)
                                        : wt + co * k * k * cin;
                int step = lanes ? 16 : 1;
                for (int ky = 0; ky < k; ky++) {
                    int iy = oy * s - pad + ky;
                    if (iy < 0 || iy >= ti.h) continue;      /* zero padding */
                    for (int kx = 0; kx < k; kx++) {
                        int ix = ox * s - pad + kx;
                        if (ix < 0 || ix >= ti.w) continue;
                        const int8_t *a = in + (iy * ti.w + ix) * cin;
                        const int8_t *b = f + (ky * k + kx) * cin * step;
                        for (int ci = 0; ci < cin; ci++) acc += a[ci] * b[ci * step];
                    }
                }
                int8_t y = sat8(rhe(acc, shift));
                *out++ = relu && y < 0 ? 0 : y;
            }
        }
    }
}

static void dwconv(const int8_t *in, tinfo_t ti, int8_t *out, tinfo_t to,
                   const int8_t *wt, const uint8_t *bias, int k, int s, int pad,
                   int shift, int relu)
{
    int c = ti.c;
    for (int oy = 0; oy < to.h; oy++) {
        for (int ox = 0; ox < to.w; ox++) {
            for (int ch = 0; ch < c; ch++) {
                int32_t acc = (int32_t)rd32(bias + 4 * ch);
                for (int ky = 0; ky < k; ky++) {
                    int iy = oy * s - pad + ky;
                    if (iy < 0 || iy >= ti.h) continue;
                    for (int kx = 0; kx < k; kx++) {
                        int ix = ox * s - pad + kx;
                        if (ix < 0 || ix >= ti.w) continue;
                        acc += in[(iy * ti.w + ix) * c + ch] *
                               wt[(ky * k + kx) * c + ch];
                    }
                }
                int8_t y = sat8(rhe(acc, shift));
                *out++ = relu && y < 0 ? 0 : y;
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
 */
/*
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

static inline void pie_bias(const uint8_t *bias)
{
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
                     const int8_t *wt, const uint8_t *bias, int k, int s, int pad,
                     int shift, int relu)
{
    int cin = ti.c, groups = to.c / 16;
    pie_round_half_even();
    for (int oy = 0; oy < to.h; oy++) {
        for (int ox = 0; ox < to.w; ox++) {
            for (int g = 0; g < groups; g++) {
                pie_bias(bias + g * 64);
                const int8_t *f = wt + g * k * k * cin * 16;
                for (int ky = 0; ky < k; ky++) {
                    int iy = oy * s - pad + ky;
                    if (iy < 0 || iy >= ti.h) continue;
                    for (int kx = 0; kx < k; kx++) {
                        int ix = ox * s - pad + kx;
                        if (ix < 0 || ix >= ti.w) continue;
                        register const int8_t *a __asm__("a0") =
                            in + (iy * ti.w + ix) * cin;
                        register const int8_t *b __asm__("a1") =
                            f + (ky * k + kx) * cin * 16;
                        if ((cin & 15) == 0) {
                            /* Sixteen inputs at once, each times a vector
                               of sixteen output channels' weights. */
                            for (int c = 0; c < cin; c += 16) {
                                __asm__ volatile(
                                    "esp.vld.128.ip q0, %0, 16\n"
                                    VSM(0) VSM(1) VSM(2) VSM(3) VSM(4) VSM(5)
                                    VSM(6) VSM(7) VSM(8) VSM(9) VSM(10) VSM(11)
                                    VSM(12) VSM(13) VSM(14) VSM(15)
                                    : "+r"(a), "+r"(b) :: "memory");
                            }
                        } else {
                            /* The first layer's three colours: one input,
                               broadcast, at a time. */
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
                pie_out(out + (oy * to.w + ox) * to.c + g * 16, shift, relu);
            }
        }
    }
}

static void pie_dwconv(const int8_t *in, tinfo_t ti, int8_t *out, tinfo_t to,
                       const int8_t *wt, const uint8_t *bias, int k, int s,
                       int pad, int shift, int relu)
{
    int c = ti.c;
    pie_round_half_even();
    for (int oy = 0; oy < to.h; oy++) {
        for (int ox = 0; ox < to.w; ox++) {
            for (int g = 0; g < c; g += 16) {
                pie_bias(bias + g * 4);
                for (int ky = 0; ky < k; ky++) {
                    int iy = oy * s - pad + ky;
                    if (iy < 0 || iy >= ti.h) continue;
                    for (int kx = 0; kx < k; kx++) {
                        int ix = ox * s - pad + kx;
                        if (ix < 0 || ix >= ti.w) continue;
                        register const int8_t *a __asm__("a0") =
                            in + (iy * ti.w + ix) * c + g;
                        register const int8_t *b __asm__("a1") =
                            wt + (ky * k + kx) * c + g;
                        __asm__ volatile(
                            "esp.vld.128.ip q0, %0, 16\n"
                            "esp.vld.128.ip q1, %1, 16\n"
                            "esp.vmulas.s8.qacc q0, q1\n"
                            : "+r"(a), "+r"(b) :: "memory");
                    }
                }
                pie_out(out + (oy * to.w + ox) * c + g, shift, relu);
            }
        }
    }
}
#endif

void pico_node(const pico_t *p, void *arena, uint32_t i)
{
    const uint8_t *n = p->nodes + i * NSZ;
    int op = n[0], relu = n[1], k = n[2], s = n[3], pad = n[4];
    int shift = (int8_t)n[6];
    uint32_t i0 = rd16(n + 8), i1 = rd16(n + 10), o = rd16(n + 12);
    const uint8_t *w = p->blob + rd32(n + 16);
    const uint8_t *b = p->blob + rd32(n + 20);
    uint32_t aux = rd32(n + 24);

    int8_t *A = (int8_t *)arena;
    tinfo_t t0 = tinfo(p, i0), to = tinfo(p, o);
    const int8_t *x = A + t0.off;
    int8_t *y = A + to.off;
    int px = to.h * to.w;

    switch (op) {
    case OP_CONV:
#ifdef PICO_PIE
        if (p->pie && (n[5] & F_LANES)) {
            pie_conv(x, t0, y, to, (const int8_t *)w, b, k, s, pad, shift, relu);
            break;
        }
#endif
        conv(x, t0, y, to, (const int8_t *)w, b, k, s, pad, shift, relu,
             n[5] & F_LANES);
        break;
    case OP_DW:
#ifdef PICO_PIE
        if (p->pie) {
            pie_dwconv(x, t0, y, to, (const int8_t *)w, b, k, s, pad, shift, relu);
            break;
        }
#endif
        dwconv(x, t0, y, to, (const int8_t *)w, b, k, s, pad, shift, relu);
        break;
    case OP_GAP: {
        int n_ = t0.h * t0.w;
        for (int c = 0; c < t0.c; c++) {
            int32_t sum = 0;
            for (int j = 0; j < n_; j++) sum += x[j * t0.c + c];
            y[c] = sat8(fmul(sum, aux, 0));
        }
        break;
    }
    case OP_LUT:
        for (int j = 0; j < px * to.c; j++) y[j] = (int8_t)w[x[j] + 128];
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
                              * t0.c + (t0.c == 1 ? 0 : c)];
                    int bb = x1[((t1.h == 1 ? 0 : yy) * t1.w + (t1.w == 1 ? 0 : xx))
                                * t1.c + (t1.c == 1 ? 0 : c)];
                    *y++ = sat8(rhe(a * bb, shift));
                }
        break;
    }
    case OP_UP2:
        for (int yy = 0; yy < to.h; yy++)
            for (int xx = 0; xx < to.w; xx++)
                for (int c = 0; c < to.c; c++)
                    *y++ = x[((yy >> 1) * t0.w + (xx >> 1)) * t0.c + c];
        break;
    case OP_CAT: {
        tinfo_t t1 = tinfo(p, i1);
        const int8_t *x1 = A + t1.off;
        for (int j = 0; j < px; j++) {
            for (int c = 0; c < t0.c; c++) *y++ = x[j * t0.c + c];
            for (int c = 0; c < t1.c; c++) *y++ = x1[j * t1.c + c];
        }
        break;
    }
    }
}

/* ---- the boxes out ---- */

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

int pico_post(const pico_t *p, void *arena, int w, int h, pico_box_t *out,
              int max)
{
    pico_box_t cand[64];
    int nc = 0;
    for (int st = 0; st < 3; st++) {
        uint32_t ts = rd32(p->m + 20 + 8 * st), tb = rd32(p->m + 24 + 8 * st);
        int sh, sw, ses, bexp;
        const int8_t *S = pico_tensor(p, arena, ts, &sh, &sw, 0, &ses);
        const int8_t *B = pico_tensor(p, arena, tb, 0, 0, 0, &bexp);
        int stride = 8 << st, off = stride / 2;
        /* 0.7 * 0.7 in the score's own scale, rounded half to even. */
        uint32_t num = 49u << -ses, thr = num / 100, rem = num % 100;
        if (rem > 50 || (rem == 50 && (thr & 1))) thr++;
        for (int yy = 0; yy < sh; yy++) {
            for (int xx = 0; xx < sw; xx++) {
                int q = S[yy * sw + xx];
                if (q <= (int)thr) continue;
                const int8_t *bb = B + (yy * sw + xx) * 32;
                int64_t cx = (int64_t)(xx * stride + off) << 16;
                int64_t cy = (int64_t)(yy * stride + off) << 16;
                pico_box_t r;
                /* (int)((c -/+ dfl * stride) * size / 224): toward zero. */
                r.x1 = (int32_t)((cx - (int64_t)dfl(bb, bexp) * stride) * w
                                 / ((int64_t)IN_SIZE << 16));
                r.y1 = (int32_t)((cy - (int64_t)dfl(bb + 8, bexp) * stride) * h
                                 / ((int64_t)IN_SIZE << 16));
                r.x2 = (int32_t)((cx + (int64_t)dfl(bb + 16, bexp) * stride) * w
                                 / ((int64_t)IN_SIZE << 16));
                r.y2 = (int32_t)((cy + (int64_t)dfl(bb + 24, bexp) * stride) * h
                                 / ((int64_t)IN_SIZE << 16));
                r.score = isqrt64((uint64_t)q << (32 + ses));
                /* Best first; equal scores keep the order they came in. */
                int at = nc < 64 ? nc : 63;
                if (nc == 64 && cand[63].score >= r.score) continue;
                while (at > 0 && cand[at - 1].score < r.score) {
                    cand[at] = cand[at - 1];
                    at--;
                }
                cand[at] = r;
                if (nc < 64) nc++;
            }
        }
    }

    /* Greedy NMS: keep the best, drop what overlaps it by more than half. */
    int kept = 0;
    uint8_t gone[64] = {0};
    for (int i = 0; i < nc && kept < TOP_K && kept < max; i++) {
        if (gone[i]) continue;
        pico_box_t *a = &cand[i];
        int32_t aa = (a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1);
        for (int j = i + 1; j < nc; j++) {
            pico_box_t *b = &cand[j];
            int32_t iw = (a->x2 < b->x2 ? a->x2 : b->x2) -
                         (a->x1 > b->x1 ? a->x1 : b->x1) + 1;
            int32_t ih = (a->y2 < b->y2 ? a->y2 : b->y2) -
                         (a->y1 > b->y1 ? a->y1 : b->y1) + 1;
            if (iw <= 0 || ih <= 0) continue;
            int32_t in_ = iw * ih;
            int32_t ab = (b->x2 - b->x1 + 1) * (b->y2 - b->y1 + 1);
            if (2 * (int64_t)in_ > aa + ab - in_) gone[j] = 1;
        }
        pico_box_t r = *a;
        if (r.x1 < 0) r.x1 = 0;
        if (r.y1 < 0) r.y1 = 0;
        if (r.x2 > w - 1) r.x2 = w - 1;
        if (r.y2 > h - 1) r.y2 = h - 1;
        out[kept++] = r;
    }
    return kept;
}

int pico_run(const pico_t *p, void *arena, const void *px, int w, int h,
             int fmt, pico_box_t *out, int max)
{
    pico_input(p, arena, px, w, h, fmt);
    for (uint32_t i = 0; i < p->n_nodes; i++) pico_node(p, arena, i);
    return pico_post(p, arena, w, h, out, max);
}

uint32_t pico_hash(const pico_t *p, const void *arena)
{
    uint32_t h = 2166136261u;                /* FNV-1a */
    for (int k = 0; k < 6; k++) {
        int th, tw, tc;
        const int8_t *d = pico_tensor(p, arena, rd32(p->m + 20 + 4 * k),
                                      &th, &tw, &tc, 0);
        for (int i = 0; i < th * tw * tc; i++) {
            h ^= (uint8_t)d[i];
            h *= 16777619u;
        }
    }
    return h;
}
