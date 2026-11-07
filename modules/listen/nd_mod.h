/*
 * What Whistle's engine needs to be a module, forced into every source
 * file with -include so the engine is compiled as written.
 *
 * A module links against nothing: no libc, no libm, no allocator. So the
 * C library the engine calls is in ndrt.c, its memory comes from the
 * module's arena, and the one place its context can be found from
 * anywhere is a register: s11, which -ffixed-s11 keeps the compiler away
 * from. s11 is callee-saved, so the firmware keeps it across every system
 * call, and RV-9's switch keeps it per thread.
 *
 * The standard headers come first, so that the renames below change the
 * engine's calls and not the library's declarations.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define RV9_MODULE 1
#define CONFIG_IDF_TARGET_ESP32P4 1     /* the vector path; asked first */

typedef struct nd_blk nd_blk_t;

typedef struct {
    uint8_t  *base;
    uint32_t  size;
    uint32_t  used, peak;       /* bytes in blocks handed out */
    nd_blk_t *avail;            /* free blocks, in address order, so neighbours merge */
} nd_pool_t;

/* Everything the engine would have kept in a global. */
typedef struct nd_ctx {
    const void *env;            /* the rv9_mod_env_t */
    nd_pool_t   heap;           /* the arena */
    uint64_t    last_us;        /* when the machine last had a turn */
    int32_t     perm1[512], perm2[512];   /* nd_whistle.c's s_perm1, s_perm2 */
} nd_ctx_t;

register nd_ctx_t *nd_ctx_reg __asm__("s11");

void *nd_malloc(size_t n);
void *nd_calloc(size_t n, size_t size);
void  nd_free(void *p);
void  nd_breathe(void);        /* a turn for the rest of the machine, if due */

#define malloc nd_malloc
#define calloc nd_calloc
#define free   nd_free

/* The engine asks for internal memory for the scratch every row of a
   layer reads. A module's arena is one kind of memory, and measured, the
   difference was 3% of a sentence for 96 KB of the chip's internal RAM. */
#define ND_ALLOC(n)      nd_malloc(n)
#define ND_FREE(p)       nd_free(p)
#define ND_ALLOC_FAST(n) nd_malloc(n)
#define ND_FREE_FAST(p)  nd_free(p)

/* The engine's only doubles are at load, where it computes its FFT's
   twiddles in double and keeps them as float. With no double-precision
   hardware that would need libgcc's soft float; computed in float, the
   tables differ in the last bit at most, and the transcripts not at all. */
#define cos(x)  cosf((float)(x))
#define sin(x)  sinf((float)(x))
#define sqrt(x) sqrtf((float)(x))
