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
#include "kal_internal.h"

#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>

#ifndef CONFIG_RV9_HEAP_FLOOR
#define CONFIG_RV9_HEAP_FLOOR 0
#endif

/*
 * Above the floor, for real-time work only. See RV9_MEM_* in rv9/kal.h.
 *
 * Sized for what admitting a loop actually costs rather than for comfort:
 * a control loop's stack and statics are two kilobytes, its set-up opens a
 * few hundred bytes more, and a failsafe's detached open is less than
 * that. Eight kilobytes admits a few loops with ordinary memory gone,
 * without taking so much from ordinary work that it runs short sooner.
 */
#define RV9_RT_RESERVE_BYTES 8192

static size_t   s_floor = CONFIG_RV9_HEAP_FLOOR;
static uint32_t s_refusals;

/* ---- classes ---- */

/*
 * Host tasks have no RV-9 thread to carry a class, and both thread-local
 * storage slots are taken (pthreads and lwIP own the first, RV-9's path
 * table cache the second). They are few, so they are a table: a host task
 * with no entry is SYSTEM. Real-time processes put themselves in it when
 * they start and are taken out when they end.
 */
#define MEM_HOST_TASKS 8

static struct {
    TaskHandle_t task;
    uint8_t      cls;
} s_host_class[MEM_HOST_TASKS];

static portMUX_TYPE s_class_guard = portMUX_INITIALIZER_UNLOCKED;

static int host_class(TaskHandle_t t)
{
    int cls = RV9_MEM_SYSTEM;
    portENTER_CRITICAL(&s_class_guard);
    for (int i = 0; i < MEM_HOST_TASKS; i++) {
        if (s_host_class[i].task == t) { cls = s_host_class[i].cls; break; }
    }
    portEXIT_CRITICAL(&s_class_guard);
    return cls;
}

int rv9_mem_class_get(void)
{
    rv9k_thread_t *th = rv9_kal_self_thread();
    if (th != NULL) return th->mem_class;
    return host_class(xTaskGetCurrentTaskHandle());
}

int rv9_mem_class_set(int cls)
{
    int prev = rv9_mem_class_get();

    rv9k_thread_t *th = rv9_kal_self_thread();
    if (th != NULL) {
        th->mem_class = (uint8_t)cls;
        return prev;
    }

    TaskHandle_t t = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_class_guard);
    int slot = -1, empty = -1;
    for (int i = 0; i < MEM_HOST_TASKS; i++) {
        if (s_host_class[i].task == t) { slot = i; break; }
        if (s_host_class[i].task == NULL && empty < 0) empty = i;
    }
    if (cls == RV9_MEM_SYSTEM) {
        if (slot >= 0) s_host_class[slot].task = NULL;   /* the default */
    } else {
        if (slot < 0) slot = empty;
        /* A full table leaves the task SYSTEM: more permissive than asked,
           never less, so nothing that should run is refused for it. */
        if (slot >= 0) {
            s_host_class[slot].task = t;
            s_host_class[slot].cls  = (uint8_t)cls;
        }
    }
    portEXIT_CRITICAL(&s_class_guard);
    return prev;
}

void rv9_mem_task_forget(rv9_task_t task)
{
    TaskHandle_t t = (TaskHandle_t)task;
    if (t == NULL) return;

    portENTER_CRITICAL(&s_class_guard);
    for (int i = 0; i < MEM_HOST_TASKS; i++) {
        if (s_host_class[i].task == t) s_host_class[i].task = NULL;
    }
    portEXIT_CRITICAL(&s_class_guard);
}

/* How far down this class may take free memory. */
static size_t floor_for(int cls)
{
    if (s_floor == 0) return 0;
    switch (cls) {
    case RV9_MEM_GENERAL:  return s_floor + RV9_RT_RESERVE_BYTES;
    case RV9_MEM_REALTIME: return s_floor;
    default:               return s_floor / 2;
    }
}

size_t rv9_heap_rt_reserve(void) { return RV9_RT_RESERVE_BYTES; }

size_t rv9_heap_available_for(int cls)
{
    size_t free_now = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t f = floor_for(cls);
    return (free_now > f) ? free_now - f : 0;
}

/*
 * Would this take us below the floor -- the caller's floor?
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

    size_t f = floor_for(rv9_mem_class_get());
    size_t free_now = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (free_now < f) return true;

    return size > free_now - f;
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

/*
 * The lowest free memory has ever been.
 *
 * Once the local monitor below is running, ESP-IDF's counter answers from
 * the re-base rather than from startup -- it is one counter, not two. So
 * the figure from before the re-base is kept here, and the all-time answer
 * is the lower of the two halves. Exact, because a minimum over a whole
 * run is the smaller of the minima over its parts.
 */
static size_t s_low_before_rebase;

size_t rv9_heap_low_water(void)
{
    size_t now = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    if (s_low_before_rebase != 0 && s_low_before_rebase < now) {
        return s_low_before_rebase;
    }
    return now;
}

/*
 * ESP-IDF can watch for a local low rather than the all-time one, which is
 * exactly what is wanted once the boot tests have finished spending memory
 * on purpose. While the monitor is running, heap_caps_get_minimum_free_size
 * answers from the moment it started; before that, both numbers agree.
 */
static bool s_low_rebased;

void rv9_heap_low_water_rebase(void)
{
    if (s_low_rebased) return;

    /* Taken before the re-base, or it is gone: the counter is shared. */
    size_t before = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);

    /* If the monitor refuses, both numbers keep answering since boot --
       equal figures say that plainly, and this file keeps no log of its
       own to say it in. */
    s_low_rebased = (heap_caps_monitor_local_minimum_free_size_start() == ESP_OK);
    if (s_low_rebased) s_low_before_rebase = before;
}

size_t rv9_heap_low_since_rebase(void)
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
