/*
 * The KAL's memory, and the floor underneath it.
 *
 * This file is built whichever kernel backs the KAL, because allocation on
 * this board does not come from the kernel: FreeRTOS and RV-9's own
 * scheduler both sit on ESP-IDF's heap. Two copies of it drifted apart
 * once already.
 *
 * ---- the floor ----
 *
 * The reserve is not for RV-9's benefit. It is for everything RV-9 cannot
 * ask.
 *
 * WiFi, the PHY, the SPI driver and ESP-IDF's own internals allocate
 * straight from the heap, and when they cannot get what they need they do
 * not return NULL -- they abort:
 *
 *     ESP_ERROR_CHECK failed: ESP_ERR_NO_MEM at phy_track_pll_init
 *     abort() was called
 *
 * That is a reboot, in a layer RV-9 does not own, caused by an unrelated
 * component's allocation failing. It happened here running the window, an
 * SSH session and a control loop together. On a vehicle it is the whole
 * system stopping because a display wanted a buffer.
 *
 * RV-9's own allocations all come through this file, and they *can* be
 * told no. So the last few kilobytes are simply never offered to them: a
 * request that would take free memory below the floor returns NULL, the
 * process manager reports "no memory to start it", and the machine stays
 * up. The floor does not make more memory exist. It decides which of two
 * failures happens -- a refusal RV-9 can report, or an abort it cannot
 * catch -- and only one of those is a system that is still running.
 *
 * It is a convention, not a wall. Nothing stops ESP-IDF spending the
 * reserve; the point is that RV-9 does not spend it first.
 */
#include "rv9/kal.h"

#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include <stdlib.h>

#ifndef CONFIG_RV9_HEAP_FLOOR
#define CONFIG_RV9_HEAP_FLOOR 0
#endif

static size_t   s_floor = CONFIG_RV9_HEAP_FLOOR;
static uint32_t s_refusals;

/*
 * Would this take us below the floor?
 *
 * Asked against the *default* heap for every allocation, including the DMA
 * and executable ones. On this board those are the same physical memory
 * viewed through different capability masks, so spending one spends the
 * others; treating them as separate pools would let the reserve be
 * consumed three times over.
 *
 * The size is not the whole cost -- the allocator has a header and rounds
 * up -- so the check is deliberately a little pessimistic rather than
 * exact. A floor accurate to the byte is a floor that has been crossed.
 */
static bool would_breach(size_t size)
{
    if (s_floor == 0) return false;

    size_t free_now = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (free_now < s_floor) return true;

    return size > free_now - s_floor;
}

static void *refuse(void)
{
    s_refusals++;
    return NULL;
}

void *rv9_alloc(size_t size)
{
    if (would_breach(size)) return refuse();
    return malloc(size);
}

void *rv9_calloc(size_t count, size_t size)
{
    /* The multiply is the allocator's business, but the floor check needs
       the product, and a product that wraps would ask for a small block
       and get a refusal it should have had anyway. */
    if (count != 0 && size > (size_t)-1 / count) return refuse();
    if (would_breach(count * size)) return refuse();
    return calloc(count, size);
}

void rv9_free(void *ptr) { free(ptr); }

void *rv9_alloc_dma(size_t size)
{
    if (would_breach(size)) return refuse();
    return heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}

void *rv9_alloc_exec(size_t size)
{
    if (would_breach(size)) return refuse();

#ifdef MALLOC_CAP_EXEC
    void *p = heap_caps_malloc(size, MALLOC_CAP_EXEC | MALLOC_CAP_8BIT);
    if (p != NULL) return p;
#endif

    /* Fall back to plain internal memory. On the C5, IRAM and DRAM are the
       same physical range (SOC_IRAM_LOW == SOC_DRAM_LOW), so internal RAM
       is executable whether or not the heap advertises MALLOC_CAP_EXEC --
       that flag only exists when ESP_SYSTEM_MEMPROT is off. Insist on
       internal: external PSRAM would not be executable on a future board. */
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void *rv9_alloc_internal(size_t size)
{
    if (would_breach(size)) return refuse();
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/*
 * Spend the reserve on purpose.
 *
 * There is exactly one thing worth spending it on: making the failure
 * legible. Refusing an allocation is only useful if somebody can be told,
 * and the telling may itself need a few bytes. Nothing that a module can
 * reach should call this.
 */
void *rv9_alloc_critical(size_t size) { return malloc(size); }

size_t rv9_heap_free(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
}

size_t rv9_heap_low_water(void)
{
    return heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
}

size_t rv9_heap_free_exec(void)
{
#ifdef MALLOC_CAP_EXEC
    return heap_caps_get_free_size(MALLOC_CAP_EXEC | MALLOC_CAP_8BIT);
#else
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
}

size_t rv9_heap_floor(void) { return s_floor; }

/*
 * Moving the floor starts the count again: a refusal is a statement about
 * where the floor was when it happened, so the two belong together. It
 * also keeps the self-test, which raises the floor to swallow the heap,
 * from leaving three refusals behind for `free` to report at boot.
 */
void rv9_heap_floor_set(size_t bytes)
{
    s_floor    = bytes;
    s_refusals = 0;
}

/*
 * What RV-9 may actually take, which is the number that decides whether
 * the next process starts. `rv9_heap_free` answers a different question --
 * how much exists -- and reporting that as though it were spendable is how
 * a system runs confidently into a wall.
 */
size_t rv9_heap_available(void)
{
    size_t free_now = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    return (free_now > s_floor) ? free_now - s_floor : 0;
}

uint32_t rv9_heap_refusals(void) { return s_refusals; }
