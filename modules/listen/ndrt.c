/*
 * The C library Whistle's engine calls, for a module that links against
 * nothing. See nd_mod.h for how the engine is pointed here.
 *
 * MEMORY
 *
 * The arena, as a first-fit free list in address order that merges
 * neighbours on free. The model is one block of it, read at start; the
 * rest comes and goes a layer at a time.
 *
 * TAKING TURNS
 *
 * RV-9 switches threads only when one asks for something, and a sentence
 * is seconds of arithmetic that asks for nothing. Every matrix product the
 * engine does goes through nd_parallel_rows, which here does the rows a
 * few at a time and offers the machine a turn between them: a millisecond's
 * sleep every BREATH_US, which lets the network stack and the host's own
 * tasks run, as well as RV-9's. Between rows no vector register is live,
 * so nothing the switch does not save is lost. The attention and the
 * log-mel front end, which are not matrix products, offer one per frame
 * (ND_BREATHE in nd_whistle.c), and so does every allocation. Measured,
 * the longest the engine then goes without one is 29 ms for 2.6 seconds
 * of speech and 107 ms for 10.
 *
 * MATHS
 *
 * expf, logf, sinf, cosf, powf: range reduction and a short polynomial,
 * to within an ulp or two of the library's, which is all a recogniser
 * whose weights are 2- and 4-bit can tell. sqrtf, fabsf and lrintf are
 * single instructions on the P4's FPU.
 */
#include "ndrt.h"
#include "nd_quant.h"

#define ND_CTX nd_ctx_reg

#define BREATH_US  20000        /* the most the engine runs before a turn */
#define ROW_CHUNK  8

/* ---- memory ---- */

struct nd_blk {
    uint32_t  size;             /* the whole block, header included */
    uint32_t  pad0;
    nd_blk_t *next;             /* while free */
    uint32_t  pad;
};
#define HDR   ((uint32_t)sizeof(nd_blk_t))     /* 16: keeps the vector loads aligned */
#define SPLIT 64

void nd_pool_init(nd_pool_t *p, void *base, uint32_t size)
{
    uintptr_t b = ((uintptr_t)base + 15) & ~(uintptr_t)15;
    size -= (uint32_t)(b - (uintptr_t)base);
    size &= ~15u;
    p->base = (uint8_t *)b;
    p->size = size;
    p->used = p->peak = 0;
    p->avail = 0;
    if (size >= HDR + SPLIT) {
        p->avail = (nd_blk_t *)b;
        p->avail->size = size;
        p->avail->next = 0;
    }
}

static void *pool_take(nd_pool_t *p, size_t n)
{
    if (n > 0x7FFFFFF0u) return 0;
    uint32_t want = ((uint32_t)n + HDR + 15) & ~15u;
    for (nd_blk_t **at = &p->avail; *at; at = &(*at)->next) {
        nd_blk_t *b = *at;
        if (b->size < want) continue;
        if (b->size - want >= HDR + SPLIT) {
            nd_blk_t *rest = (nd_blk_t *)((uint8_t *)b + want);
            rest->size = b->size - want;
            rest->next = b->next;
            *at = rest;
            b->size = want;
        } else {
            *at = b->next;
        }
        p->used += b->size;
        if (p->used > p->peak) p->peak = p->used;
        return (uint8_t *)b + HDR;
    }
    return 0;
}

static void pool_give(nd_pool_t *p, nd_blk_t *b)
{
    p->used -= b->size;
    nd_blk_t **at = &p->avail;
    while (*at && *at < b) at = &(*at)->next;
    b->next = *at;
    *at = b;
    if (b->next && (uint8_t *)b + b->size == (uint8_t *)b->next) {
        b->size += b->next->size;
        b->next = b->next->next;
    }
    /* and the one before, which the walk passed */
    for (nd_blk_t *q = p->avail; q && q != b; q = q->next) {
        if (q->next == b && (uint8_t *)q + q->size == (uint8_t *)b) {
            q->size += b->size;
            q->next = b->next;
            break;
        }
    }
}

void *nd_malloc(size_t n)
{
    nd_breathe();
    return pool_take(&ND_CTX->heap, n);
}

void *nd_calloc(size_t n, size_t size)
{
    if (size && n > 0x7FFFFFF0u / size) return 0;
    void *p = nd_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void nd_free(void *v)
{
    if (v == 0) return;
    nd_blk_t *b = (nd_blk_t *)((uint8_t *)v - HDR);
    pool_give(&ND_CTX->heap, b);
}

/* ---- taking turns ---- */

#define ENV(c) ((const rv9_mod_env_t *)(c)->env)

void nd_breathe(void)
{
    nd_ctx_t *c = ND_CTX;
    if (ENV(c)->time_us() - c->last_us < BREATH_US) return;
    ENV(c)->sleep_ms(1);
    c->last_us = ENV(c)->time_us();
}

void nd_parallel_rows(nd_row_fn fn, void *ctx, uint32_t nrows)
{
    for (uint32_t r = 0; r < nrows; r += ROW_CHUNK) {
        uint32_t e = nrows - r < ROW_CHUNK ? nrows : r + ROW_CHUNK;
        fn(ctx, r, e);
        nd_breathe();
    }
}

/* ---- the string functions GCC and the engine call ---- */

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if ((((uintptr_t)dp | (uintptr_t)sp) & 3) == 0) {
        for (; n >= 4; n -= 4, dp += 4, sp += 4)
            *(uint32_t *)(void *)dp = *(const uint32_t *)(const void *)sp;
    }
    while (n--) *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (dp <= sp || dp >= sp + n) return memcpy(d, s, n);
    while (n--) dp[n] = sp[n];
    return d;
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *dp = d;
    uint32_t w = (uint8_t)c * 0x01010101u;
    while (n && ((uintptr_t)dp & 3)) { *dp++ = (uint8_t)c; n--; }
    for (; n >= 4; n -= 4, dp += 4) *(uint32_t *)(void *)dp = w;
    while (n--) *dp++ = (uint8_t)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    const char *e = s;
    while (*e) e++;
    return (size_t)(e - s);
}

char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = '\0';
    return d;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (uint8_t)*a - (uint8_t)*b;
        if (*a == '\0') return 0;
    }
    return 0;
}

/* ---- maths ---- */

typedef union { float f; uint32_t u; int32_t i; } fu_t;

float sqrtf(float x)
{
    float r;
    __asm__("fsqrt.s %0, %1" : "=f"(r) : "f"(x));
    return r;
}

float fabsf(float x)
{
    fu_t v = { .f = x };
    v.u &= 0x7FFFFFFFu;
    return v.f;
}

long lrintf(float x)
{
    long r;
    __asm__("fcvt.w.s %0, %1, rne" : "=r"(r) : "f"(x));
    return r;
}

float floorf(float x)
{
    fu_t v = { .f = x };
    if ((v.u & 0x7F800000u) >= 0x4B000000u) return x;   /* integral, or not a number */
    float t = (float)(int32_t)x;
    return t > x ? t - 1.0f : t;
}

/* e^x: x = k ln2 + r, |r| <= ln2/2, ln2 in two parts so k ln2 is exact. */
float expf(float x)
{
    if (x != x) return x;
    if (x > 88.72283f) { fu_t v = { .u = 0x7F800000u }; return v.f; }
    if (x < -103.97208f) return 0.0f;

    float kf = floorf(x * 1.44269504f + 0.5f);
    int32_t k = (int32_t)kf;
    float r = (x - kf * 0.693145752f) - kf * 1.42860677e-6f;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.166666672f +
              r * (0.0416666418f + r * (0.00833336264f + r * 0.00138853584f)))));

    /* times 2^k, in two steps so a subnormal answer still comes out */
    if (k < -126) {
        fu_t s = { .u = (uint32_t)(k + 126 + 127) << 23 };
        fu_t t = { .u = 1u << 23 };              /* 2^-126 */
        return p * s.f * t.f;
    }
    if (k > 127) {
        fu_t s = { .u = (uint32_t)(k - 1 + 127) << 23 };
        return p * s.f * 2.0f;
    }
    fu_t s = { .u = (uint32_t)(k + 127) << 23 };
    return p * s.f;
}

/* ln x: x = 2^e m, m in [sqrt(1/2), sqrt 2); ln m = 2 atanh(s), s = (m-1)/(m+1). */
float logf(float x)
{
    fu_t v = { .f = x };
    if (x != x || v.i < 0) { fu_t n = { .u = 0x7FC00000u }; return n.f; }
    if (v.u == 0) { fu_t n = { .u = 0xFF800000u }; return n.f; }
    if (v.u >= 0x7F800000u) return x;

    int32_t e = 0;
    if (v.u < 0x00800000u) { v.f *= 16777216.0f; e = -24; }   /* subnormal */
    e += (int32_t)(v.u >> 23) - 127;
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;                  /* m in [1, 2) */
    if (v.f > 1.41421356f) { v.f *= 0.5f; e++; }

    float s  = (v.f - 1.0f) / (v.f + 1.0f);
    float s2 = s * s;
    float p  = s2 * (0.666666664f + s2 * (0.400000006f + s2 * (0.285714284f +
               s2 * (0.222221987f + s2 * 0.181887722f))));
    float lm = 2.0f * s + s * p;
    return lm + (float)e * 0.693147182f;
}

/* sin and cos of |r| <= pi/4 */
static float sin_k(float r)
{
    float r2 = r * r;
    return r + r * r2 * (-0.166666657f + r2 * (0.00833330024f +
               r2 * (-0.000198393349f + r2 * 2.7183114e-6f)));
}

static float cos_k(float r)
{
    float r2 = r * r;
    return 1.0f + r2 * (-0.5f + r2 * (0.0416666418f + r2 * (-0.00138867637f +
           r2 * 2.44331571e-5f)));
}

/* x = q pi/2 + r, pi/2 in three parts: good to |x| of a few thousand,
   which is more than a 30-second utterance's positions reach. */
static float reduce(float x, int32_t *q)
{
    float qf = floorf(x * 0.636619772f + 0.5f);
    *q = (int32_t)qf;
    return ((x - qf * 1.5703125f) - qf * 4.83751297e-4f) - qf * 7.54978942e-8f;
}

float sinf(float x)
{
    int32_t q;
    float r = reduce(x, &q);
    switch (q & 3) {
    case 0:  return sin_k(r);
    case 1:  return cos_k(r);
    case 2:  return -sin_k(r);
    default: return -cos_k(r);
    }
}

float cosf(float x)
{
    int32_t q;
    float r = reduce(x, &q);
    switch (q & 3) {
    case 0:  return cos_k(r);
    case 1:  return -sin_k(r);
    case 2:  return -cos_k(r);
    default: return sin_k(r);
    }
}

/* Only ever of a positive base in the engine: RoPE's frequencies and a
   threshold in decibels, both at load. */
float powf(float b, float e)
{
    if (e == 0.0f) return 1.0f;
    if (b <= 0.0f) return b == 0.0f ? 0.0f : 0.0f / 0.0f;
    return expf(e * logf(b));
}
