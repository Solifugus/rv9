/*
 * The kernel's allocator.
 *
 * A doubly-linked list of blocks in address order, first fit, splitting on
 * allocation and coalescing both ways on free. Unglamorous and entirely
 * adequate: the kernel allocates thread stacks and small objects, not a
 * workload that needs size classes.
 *
 * Blocks are linked physically rather than kept in a separate free list,
 * so coalescing is O(1) and needs no search. The cost is that allocation
 * walks every block, free or not. For a heap holding tens of objects that
 * is the right trade; if it ever holds thousands it is the wrong one, and
 * the fix is a free list threaded through the same blocks.
 */
#include "rv9/kernel.h"

#define ALIGN       8u
#define IN_USE      1u
#define MIN_PAYLOAD 16u

typedef struct block {
    struct block *prev;      /* physically previous */
    struct block *next;      /* physically next */
    uint32_t      size;      /* payload bytes */
    uint32_t      flags;
} block_t;

static block_t *s_first;
static size_t   s_total;

static size_t align_up(size_t n)
{
    return (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1);
}

static void *payload_of(block_t *b) { return (void *)(b + 1); }

static block_t *block_of(void *p)
{
    return ((block_t *)p) - 1;
}

void rv9k_heap_init(void *base, size_t bytes)
{
    if (base == NULL || bytes < sizeof(block_t) + MIN_PAYLOAD) {
        s_first = NULL;
        s_total = 0;
        return;
    }

    /* Align the region itself; the caller may have handed us anything. */
    uintptr_t start = align_up((uintptr_t)base);
    size_t    lost  = (size_t)(start - (uintptr_t)base);
    if (lost >= bytes) { s_first = NULL; s_total = 0; return; }

    bytes -= lost;

    block_t *b = (block_t *)start;
    b->prev  = NULL;
    b->next  = NULL;
    b->size  = (uint32_t)(bytes - sizeof(block_t));
    b->flags = 0;

    s_first = b;
    s_total = bytes;
}

void *rv9k_alloc(size_t bytes)
{
    if (bytes == 0 || s_first == NULL) return NULL;

    size_t want = align_up(bytes);
    if (want < MIN_PAYLOAD) want = MIN_PAYLOAD;

    for (block_t *b = s_first; b != NULL; b = b->next) {
        if (b->flags & IN_USE) continue;
        if (b->size < want) continue;

        /* Split, but only if what is left can hold a block worth having.
           Otherwise hand over the whole thing and waste the remainder --
           a split that leaves an unusable sliver costs more than it saves. */
        size_t leftover = b->size - want;
        if (leftover >= sizeof(block_t) + MIN_PAYLOAD) {
            block_t *rest = (block_t *)((uint8_t *)payload_of(b) + want);
            rest->size  = (uint32_t)(leftover - sizeof(block_t));
            rest->flags = 0;
            rest->prev  = b;
            rest->next  = b->next;
            if (rest->next) rest->next->prev = rest;

            b->next = rest;
            b->size = (uint32_t)want;
        }

        b->flags |= IN_USE;
        return payload_of(b);
    }

    return NULL;
}

void *rv9k_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    if (count != 0 && total / count != size) return NULL;   /* overflow */

    uint8_t *p = (uint8_t *)rv9k_alloc(total);
    if (p == NULL) return NULL;

    for (size_t i = 0; i < total; i++) p[i] = 0;
    return p;
}

/* Merge b with the block after it, which must be free. */
static void merge_with_next(block_t *b)
{
    block_t *n = b->next;
    if (n == NULL || (n->flags & IN_USE)) return;

    b->size += (uint32_t)sizeof(block_t) + n->size;
    b->next = n->next;
    if (b->next) b->next->prev = b;
}

void rv9k_free(void *ptr)
{
    if (ptr == NULL) return;

    block_t *b = block_of(ptr);
    if (!(b->flags & IN_USE)) return;      /* double free; ignore it */

    b->flags &= ~IN_USE;

    /* Coalesce forwards, then backwards. Doing both is what keeps a heap
       from fragmenting into uselessness under alloc/free churn. */
    merge_with_next(b);
    if (b->prev && !(b->prev->flags & IN_USE)) merge_with_next(b->prev);
}

void rv9k_heap_stats(rv9k_heap_stats_t *out)
{
    if (out == NULL) return;

    out->total        = s_total;
    out->free_bytes   = 0;
    out->largest_free = 0;
    out->blocks       = 0;
    out->free_blocks  = 0;

    for (block_t *b = s_first; b != NULL; b = b->next) {
        out->blocks++;
        if (b->flags & IN_USE) continue;

        out->free_blocks++;
        out->free_bytes += b->size;
        if (b->size > out->largest_free) out->largest_free = b->size;
    }
}
