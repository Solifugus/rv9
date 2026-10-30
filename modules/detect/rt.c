/*
 * What the compiler calls when C asks for more than RV32IMAC does.
 *
 * pico.c reproduces a float32 multiply and a few divisions in 64-bit
 * integers, and a 32-bit RISC-V has no instructions for 64-bit shifts,
 * divisions or leading zeros: GCC calls libgcc for them. A module links
 * against nothing (that is the point of a module), so they are here,
 * written on 32-bit halves -- in 64-bit C they would call themselves.
 * Also memcpy and memset, which GCC emits for struct copies and an
 * initialised array whatever -fno-builtin says.
 *
 * Only for the module: on a PC, pico.c gets all of this from the compiler's
 * own library.
 */
#include <stddef.h>
#include <stdint.h>

typedef union { uint64_t u; int64_t s; struct { uint32_t lo, hi; } w; } dw_t;

uint64_t __ashldi3(uint64_t a, int b)
{
    dw_t x = { .u = a }, r;
    if (b == 0) return a;
    if (b >= 32) { r.w.lo = 0; r.w.hi = x.w.lo << (b - 32); }
    else { r.w.lo = x.w.lo << b; r.w.hi = x.w.hi << b | x.w.lo >> (32 - b); }
    return r.u;
}

uint64_t __lshrdi3(uint64_t a, int b)
{
    dw_t x = { .u = a }, r;
    if (b == 0) return a;
    if (b >= 32) { r.w.hi = 0; r.w.lo = x.w.hi >> (b - 32); }
    else { r.w.hi = x.w.hi >> b; r.w.lo = x.w.lo >> b | x.w.hi << (32 - b); }
    return r.u;
}

int64_t __ashrdi3(int64_t a, int b)
{
    dw_t x = { .s = a }, r;
    if (b == 0) return a;
    if (b >= 32) {
        r.w.hi = (uint32_t)((int32_t)x.w.hi >> 31);
        r.w.lo = (uint32_t)((int32_t)x.w.hi >> (b - 32));
    } else {
        r.w.hi = (uint32_t)((int32_t)x.w.hi >> b);
        r.w.lo = x.w.lo >> b | x.w.hi << (32 - b);
    }
    return r.s;
}

static int clz32(uint32_t v)
{
    int n = 0;
    if (!(v & 0xffff0000u)) { n += 16; v <<= 16; }
    if (!(v & 0xff000000u)) { n += 8;  v <<= 8; }
    if (!(v & 0xf0000000u)) { n += 4;  v <<= 4; }
    if (!(v & 0xc0000000u)) { n += 2;  v <<= 2; }
    if (!(v & 0x80000000u)) { n += 1; }
    return n;
}

int __clzdi2(uint64_t a)
{
    dw_t x = { .u = a };
    return x.w.hi ? clz32(x.w.hi) : 32 + clz32(x.w.lo);
}

/* Shift and subtract, a bit at a time: these run a few hundred times a
   picture, in the boxes, never in the layers. */
static uint64_t udivmod(uint64_t n, uint64_t d, uint64_t *rem)
{
    dw_t q = { .u = 0 }, r = { .u = 0 }, nn = { .u = n }, dd = { .u = d };
    for (int i = 63; i >= 0; i--) {
        /* r = r << 1 | bit i of n, on halves */
        r.w.hi = r.w.hi << 1 | r.w.lo >> 31;
        r.w.lo = r.w.lo << 1 |
                 ((i >= 32 ? nn.w.hi >> (i - 32) : nn.w.lo >> i) & 1);
        if (r.w.hi > dd.w.hi || (r.w.hi == dd.w.hi && r.w.lo >= dd.w.lo)) {
            uint32_t borrow = r.w.lo < dd.w.lo;
            r.w.lo -= dd.w.lo;
            r.w.hi -= dd.w.hi + borrow;
            if (i >= 32) q.w.hi |= 1u << (i - 32);
            else q.w.lo |= 1u << i;
        }
    }
    if (rem) *rem = r.u;
    return q.u;
}

uint64_t __udivdi3(uint64_t n, uint64_t d) { return udivmod(n, d, 0); }

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r;
    udivmod(n, d, &r);
    return r;
}

int64_t __divdi3(int64_t n, int64_t d)
{
    int neg = (n < 0) != (d < 0);
    dw_t a = { .s = n }, b = { .s = d };
    /* Negate on halves: two's complement is invert and add one. */
    if (n < 0) { a.w.lo = ~a.w.lo + 1; a.w.hi = ~a.w.hi + (a.w.lo == 0); }
    if (d < 0) { b.w.lo = ~b.w.lo + 1; b.w.hi = ~b.w.hi + (b.w.lo == 0); }
    dw_t q = { .u = udivmod(a.u, b.u, 0) };
    if (neg) { q.w.lo = ~q.w.lo + 1; q.w.hi = ~q.w.hi + (q.w.lo == 0); }
    return q.s;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}
