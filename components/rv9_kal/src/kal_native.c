/*
 * RV-9 KAL -- native backend.
 *
 * The same contract five phases of RV-9 have been calling, implemented on
 * RV-9's own kernel. Selected by CONFIG_RV9_KERNEL_NATIVE; the FreeRTOS
 * backend is built otherwise, and both pass the same conformance suite.
 *
 * WHAT IS STILL BORROWED, AND WHY
 *
 * Memory comes from the host allocator, not the kernel's. Drivers need
 * DMA-capable and executable memory, which are properties of where the
 * memory is, not of who hands it out. The kernel's own heap is used for
 * the kernel's own objects; a general allocator that understands this
 * hardware's memory regions is a later job and pretending otherwise would
 * break the LCD and the module loader.
 *
 * Wall-clock microseconds come from the hardware counter rather than the
 * scheduler's tick. Reading a timer is not a scheduling service, and a
 * 1 ms tick is too coarse for code that measures short intervals.
 */
#include "rv9/kal.h"
#include "rv9/kernel.h"
#include "kal_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

/*
 * Hosting glue.
 *
 * RV-9's kernel runs inside one host task for now, so it needs a way to
 * give the CPU back when it has nothing to run, and a task to live in.
 * Both of those disappear at step 3, when the kernel owns the machine and
 * idling becomes a wait-for-interrupt.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rv9-kal";

/* The kernel's heap. Its own objects come from here; the host allocator
   still serves DMA and executable memory, which are properties of where
   the memory is rather than of who hands it out.
   
   Sized to what the kernel actually holds -- thread bookkeeping and small
   objects. Reserving more than that takes it away from the WiFi stack,
   which needs a great deal more than RV-9 does. */
#define KERNEL_HEAP_BYTES (32 * 1024)

static volatile uint32_t *s_tick_ref;

/*
 * IRAM, and it calls nothing. An interrupt here can arrive while the flash
 * cache is disabled -- the WiFi driver writes NVS -- and anything living
 * in flash is unreachable then.
 */
static void IRAM_ATTR tick_isr(void *arg)
{
    (void)arg;
    if (s_tick_ref) (*s_tick_ref)++;
}

/* Nothing to run: let the rest of the machine have the CPU. On bare metal
   this becomes a wait-for-interrupt. */
static void host_idle(void)
{
    vTaskDelay(1);
}

/* ---------------- interrupts ---------------- */

/*
 * Straight to the CSR. A critical section means interrupts off, and this
 * is the instruction that does it -- going through the host's macros for
 * it would be borrowing something we do not need to borrow.
 */
static inline uint32_t irq_save(void)
{
    uint32_t prev;
    __asm__ volatile ("csrrci %0, mstatus, 8" : "=r"(prev) :: "memory");
    return prev;
}

static inline void irq_restore(uint32_t prev)
{
    if (prev & 8) __asm__ volatile ("csrsi mstatus, 8" ::: "memory");
}

static uint32_t s_critical_state;
static uint32_t s_critical_depth;

const char *rv9_strerror(rv9_err_t err)
{
    switch (err) {
    case RV9_OK:              return "ok";
    case RV9_ERR_INVAL:       return "invalid argument";
    case RV9_ERR_NOMEM:       return "out of memory";
    case RV9_ERR_TIMEOUT:     return "timed out";
    case RV9_ERR_UNSUPPORTED: return "unsupported";
    case RV9_ERR_BUSY:        return "busy";
    default:                  return "unknown error";
    }
}

/* ---------------- time ---------------- */

RV9_RT_CODE uint64_t rv9_time_us(void) { return (uint64_t)esp_timer_get_time(); }
uint64_t rv9_time_ms(void) { return (uint64_t)esp_timer_get_time() / 1000ULL; }

uint32_t rv9_ms_to_ticks(uint32_t ms) { return RV9K_MS_TO_TICKS(ms); }

/* ---------------- starting the kernel ---------------- */

/* Thread stacks must be internal memory: they are written and executed
   from with the flash cache potentially disabled. Through the KAL, not
   straight to the heap, so that the floor applies -- a stack is the
   largest single thing a new process asks for, and it is exactly the
   request that should be refused rather than granted into the reserve. */
static void *task_stack_alloc(size_t n)
{
    return rv9_alloc_internal(n);
}


/* The one host task RV-9's threads run inside. Recorded so that anyone
   asking "am I an RV-9 thread?" can be told the truth; see kal_internal.h. */
static TaskHandle_t s_kernel_task;

static void kernel_host_task(void *arg)
{
    (void)arg;
    s_kernel_task = xTaskGetCurrentTaskHandle();
    rv9k_serve();        /* never returns */
}

RV9_RT_CODE rv9k_thread_t *rv9_kal_self_thread(void)
{
    if (xTaskGetCurrentTaskHandle() != s_kernel_task) return NULL;
    return rv9k_self();
}

rv9_err_t rv9_kal_start(rv9_task_fn fn, const char *name, size_t stack_bytes,
                        void *arg, int priority)
{
    rv9k_init();
    rv9k_set_allocators(task_stack_alloc, free);

    void *region = heap_caps_malloc(KERNEL_HEAP_BYTES,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (region == NULL) return RV9_ERR_NOMEM;
    rv9k_heap_init(region, KERNEL_HEAP_BYTES);

    s_tick_ref = rv9k_tick_ref();

    const esp_timer_create_args_t tick = {
        .callback        = tick_isr,
        .dispatch_method = ESP_TIMER_ISR,
        .name            = "rv9k-tick",
    };
    esp_timer_handle_t h = NULL;
    if (esp_timer_create(&tick, &h) != ESP_OK) return RV9_ERR_NOMEM;
    if (esp_timer_start_periodic(h, 1000000 / RV9K_TICK_HZ) != ESP_OK) {
        return RV9_ERR_NOMEM;
    }

    rv9k_set_idle_hook(host_idle);

    rv9_err_t err = rv9_task_create(fn, name, stack_bytes, arg, priority, NULL);
    if (err != RV9_OK) return err;

    ESP_LOGI(TAG, "RV-9 kernel: %d Hz tick, %d KB heap, serving",
             RV9K_TICK_HZ, KERNEL_HEAP_BYTES / 1024);

    /*
     * The kernel needs a stack to run its scheduler on and a CPU to be
     * scheduled onto. Its host task takes a high priority so RV-9's
     * threads are not starved by the host's own work.
     */
    if (xTaskCreate(kernel_host_task, "rv9-kernel", 8192, NULL,
                    configMAX_PRIORITIES - 3, NULL) != pdPASS) {
        return RV9_ERR_NOMEM;
    }

    return RV9_OK;
}

/* ---------------- tasks ---------------- */

rv9_err_t rv9_task_create(rv9_task_fn fn, const char *name, size_t stack_bytes,
                          void *arg, int priority, rv9_task_t *out_task)
{
    if (fn == NULL) return RV9_ERR_INVAL;
    if (stack_bytes == 0) stack_bytes = 4096;

    rv9k_thread_t *t = rv9k_thread_create((rv9k_entry_fn)fn, arg, name,
                                          stack_bytes, priority);
    if (t == NULL) return RV9_ERR_NOMEM;

    if (out_task) *out_task = (rv9_task_t)t;
    return RV9_OK;
}

/*
 * EVERY FUNCTION BELOW TAKES A HANDLE THAT MAY NOT BE OURS.
 *
 * A real-time process runs on the host's scheduler, so its handle is a
 * FreeRTOS TCB, not an rv9k_thread_t. Casting one to the other reads
 * whatever happens to sit at that offset -- which is how `stacks` reported
 * a 34-megabyte stack for `iolat`, and how a perfectly healthy real-time
 * process could have been read as dead and buried by the fault collector.
 *
 * rv9k_is_thread answers exactly, so each of these asks first and falls
 * back to what the host can tell us.
 */
rv9_err_t rv9_task_stack(rv9_task_t task, size_t *size, size_t *unused)
{
    if (task == NULL) return RV9_ERR_INVAL;

    if (!rv9k_is_thread(task)) {
        /* A host task. FreeRTOS keeps a high-water mark and not a size,
           so the size is reported as unknown rather than invented. */
        if (size)   *size   = 0;
        if (unused) *unused = uxTaskGetStackHighWaterMark((TaskHandle_t)task)
                              * sizeof(StackType_t);
        return RV9_OK;
    }

    const rv9k_thread_t *t = (const rv9k_thread_t *)task;
    if (size)   *size   = rv9k_stack_size(t);
    if (unused) *unused = rv9k_stack_unused(t);
    return RV9_OK;
}

int rv9_task_fault(rv9_task_t task)
{
    if (!rv9k_is_thread(task)) return RV9_TASK_FAULT_NONE;
    return rv9k_thread_fault((const rv9k_thread_t *)task);
}

bool rv9_task_alive(rv9_task_t task)
{
    /* A host task's liveness is the trampoline's business, as it is on
       the FreeRTOS backend. Saying "true" is a statement about what is
       known here, and claiming otherwise would bury the living. */
    if (!rv9k_is_thread(task)) return true;
    return rv9k_thread_alive((const rv9k_thread_t *)task);
}

void rv9_task_hold(void)
{
    rv9k_thread_t *t = rv9_kal_self_thread();
    if (t != NULL) t->holds++;
}

void rv9_task_unhold(void)
{
    rv9k_thread_t *t = rv9_kal_self_thread();
    if (t != NULL && t->holds > 0) t->holds--;
}

bool rv9_task_cancelled(void)
{
    rv9k_thread_t *t = rv9_kal_self_thread();
    return t != NULL && t->cancel;
}

void rv9_task_uncancel(rv9_task_t task)
{
    if (rv9k_is_thread(task)) ((rv9k_thread_t *)task)->cancel = false;
}

rv9_err_t rv9_task_kill(rv9_task_t task)
{
    /* Both ends must be threads. The thread table belongs to the host task
       the kernel runs in, and touching it from any other is a race. */
    if (!rv9k_is_thread(task) || rv9_kal_self_thread() == NULL) {
        return RV9_ERR_UNSUPPORTED;
    }

    switch (rv9k_thread_stop((rv9k_thread_t *)task)) {
    case 0:  return RV9_OK;
    case -2: return RV9_ERR_BUSY;
    default: return RV9_ERR_INVAL;
    }
}

void rv9_task_reap(rv9_task_t task)
{
    if (!rv9k_is_thread(task)) return;
    rv9k_thread_release((rv9k_thread_t *)task);
}

void rv9_task_delete(rv9_task_t task)
{
    if (task != NULL) {
        if (rv9k_is_thread(task)) rv9k_thread_kill((rv9k_thread_t *)task);
        else                      vTaskDelete((TaskHandle_t)task);
        return;
    }

    /*
     * Ending oneself, whichever scheduler one belongs to.
     *
     * A real-time process runs as a host task, and asking RV-9's kernel to
     * end it does nothing -- so the task function returned, which FreeRTOS
     * treats as a fatal error, correctly. The same oversight as delays:
     * not every caller of the KAL is an RV-9 thread.
     */
    if (rv9_kal_self_thread() != NULL) rv9k_exit();
    else                     vTaskDelete(NULL);
}

/*
 * Who is running, in whichever scheduler owns them.
 *
 * A real-time process is a host task, so returning only RV-9's notion of
 * "current thread" left it with no identity at all: the process manager
 * could not match it to a process, so it had no pid, no path table and no
 * output. It ran perfectly and silently into the void.
 *
 * Handles from the two schedulers are distinct objects, so one comparison
 * serves both.
 */
rv9_task_t rv9_task_self(void)
{
    rv9k_thread_t *t = rv9_kal_self_thread();
    if (t != NULL) return (rv9_task_t)t;
    return (rv9_task_t)xTaskGetCurrentTaskHandle();
}

/*
 * Not every caller is an RV-9 thread. Real-time processes and driver tasks
 * run on the host's scheduler, and asking RV-9's kernel to sleep them does
 * nothing at all -- which, at real-time priority, is a busy loop that stops
 * the machine. Fall back to the host for callers it does not own.
 */
void rv9_task_yield(void)
{
    if (rv9_kal_self_thread() != NULL) rv9k_yield();
    else                     taskYIELD();
}

void rv9_task_delay_ms(uint32_t ms)
{
    if (rv9_kal_self_thread() != NULL) {
        rv9k_sleep_ms(ms);
    } else {
        TickType_t t = pdMS_TO_TICKS(ms);
        vTaskDelay(t ? t : 1);
    }
}

rv9_err_t rv9_task_priority_set(rv9_task_t task, int priority)
{
    /* A real-time task's priority is not RV-9's to change: it sits above
       everything by definition, and the ager has no business there. */
    if (!rv9k_is_thread(task)) return RV9_ERR_UNSUPPORTED;

    rv9k_priority_set((rv9k_thread_t *)task, priority);
    return RV9_OK;
}

/* The kernel ages threads itself; nobody should be doing it from above. */
bool rv9_sched_ages(void) { return true; }

RV9_RT_CODE void rv9_preempt_point(void)
{
    /* Only a thread of RV-9's own can be preempted by RV-9. */
    if (rv9_kal_self_thread() != NULL) rv9k_preempt_point();
}

/* ---------------- task-local storage ---------------- */

/*
 * Index 1, not 0.
 *
 * ESP-IDF's pthread support owns index 0, and the default configuration
 * allocates exactly one slot -- so the obvious choice silently overwrote
 * somebody else's pointer. See sdkconfig.defaults.
 */
#define RV9_TLS_INDEX 1

/*
 * Whichever scheduler owns the caller holds the slot. An RV-9 thread
 * carries its own; a real-time process is a host task and uses the host's.
 */
RV9_RT_CODE void *rv9_task_local_get(void)
{
    rv9k_thread_t *t = rv9_kal_self_thread();
    if (t != NULL) return rv9k_thread_local_get(t);
    return pvTaskGetThreadLocalStoragePointer(NULL, RV9_TLS_INDEX);
}

RV9_RT_CODE void rv9_task_local_set(void *value)
{
    rv9k_thread_t *t = rv9_kal_self_thread();
    if (t != NULL) { rv9k_thread_local_set(t, value); return; }
    vTaskSetThreadLocalStoragePointer(NULL, RV9_TLS_INDEX, value);
}

/* ---------------- semaphores ---------------- */

rv9_err_t rv9_sem_create(uint32_t max_count, uint32_t initial_count,
                         rv9_sem_t *out_sem)
{
    if (out_sem == NULL || max_count == 0 || initial_count > max_count) {
        return RV9_ERR_INVAL;
    }
    rv9k_sem_t *s = (rv9k_sem_t *)malloc(sizeof(*s));
    if (s == NULL) return RV9_ERR_NOMEM;

    rv9k_sem_init(s, (int32_t)initial_count, (int32_t)max_count);
    *out_sem = (rv9_sem_t)s;
    return RV9_OK;
}

void rv9_sem_destroy(rv9_sem_t sem) { free(sem); }

rv9_err_t rv9_sem_take(rv9_sem_t sem, uint32_t timeout_ms)
{
    if (sem == NULL) return RV9_ERR_INVAL;
    return rv9k_sem_take((rv9k_sem_t *)sem, timeout_ms) ? RV9_OK
                                                        : RV9_ERR_TIMEOUT;
}

rv9_err_t rv9_sem_give(rv9_sem_t sem)
{
    if (sem == NULL) return RV9_ERR_INVAL;
    rv9k_sem_give((rv9k_sem_t *)sem);
    return RV9_OK;
}

rv9_err_t rv9_sem_give_from_isr(rv9_sem_t sem, bool *higher_prio_woken)
{
    if (sem == NULL) return RV9_ERR_INVAL;

    bool woken = false;
    bool ok = rv9k_sem_give_from_isr((rv9k_sem_t *)sem, &woken);

    if (higher_prio_woken) *higher_prio_woken = woken;
    return ok ? RV9_OK : RV9_ERR_INVAL;
}

/* ---------------- mutexes ---------------- */

rv9_err_t rv9_mutex_create(rv9_mutex_t *out_mutex)
{
    if (out_mutex == NULL) return RV9_ERR_INVAL;
    rv9k_mutex_t *m = (rv9k_mutex_t *)malloc(sizeof(*m));
    if (m == NULL) return RV9_ERR_NOMEM;

    rv9k_mutex_init(m);
    *out_mutex = (rv9_mutex_t)m;
    return RV9_OK;
}

/* The kernel's mutex counts its own nesting, so one kind serves both. */
rv9_err_t rv9_mutex_create_recursive(rv9_mutex_t *out_mutex)
{
    return rv9_mutex_create(out_mutex);
}

void rv9_mutex_destroy(rv9_mutex_t mutex) { free(mutex); }

rv9_err_t rv9_mutex_lock(rv9_mutex_t mutex, uint32_t timeout_ms)
{
    if (mutex == NULL) return RV9_ERR_INVAL;
    return rv9k_mutex_lock((rv9k_mutex_t *)mutex, timeout_ms) ? RV9_OK
                                                              : RV9_ERR_TIMEOUT;
}

rv9_err_t rv9_mutex_unlock(rv9_mutex_t mutex)
{
    if (mutex == NULL) return RV9_ERR_INVAL;
    rv9k_mutex_unlock((rv9k_mutex_t *)mutex);
    return RV9_OK;
}

/* ---------------- queues ---------------- */

typedef struct {
    rv9k_queue_t q;
    uint8_t      storage[];
} native_queue_t;

rv9_err_t rv9_queue_create(uint32_t length, size_t item_size,
                           rv9_queue_t *out_queue)
{
    if (out_queue == NULL || length == 0 || item_size == 0) {
        return RV9_ERR_INVAL;
    }
    native_queue_t *nq =
        (native_queue_t *)malloc(sizeof(*nq) + (size_t)length * item_size);
    if (nq == NULL) return RV9_ERR_NOMEM;

    rv9k_queue_init(&nq->q, nq->storage, length, (uint32_t)item_size);
    *out_queue = (rv9_queue_t)nq;
    return RV9_OK;
}

void rv9_queue_destroy(rv9_queue_t queue) { free(queue); }

rv9_err_t rv9_queue_send(rv9_queue_t queue, const void *item,
                         uint32_t timeout_ms)
{
    if (queue == NULL || item == NULL) return RV9_ERR_INVAL;
    return rv9k_queue_send(&((native_queue_t *)queue)->q, item, timeout_ms)
           ? RV9_OK : RV9_ERR_TIMEOUT;
}

rv9_err_t rv9_queue_recv(rv9_queue_t queue, void *item, uint32_t timeout_ms)
{
    if (queue == NULL || item == NULL) return RV9_ERR_INVAL;
    return rv9k_queue_recv(&((native_queue_t *)queue)->q, item, timeout_ms)
           ? RV9_OK : RV9_ERR_TIMEOUT;
}

rv9_err_t rv9_queue_send_from_isr(rv9_queue_t queue, const void *item,
                                  bool *higher_prio_woken)
{
    if (queue == NULL || item == NULL) return RV9_ERR_INVAL;

    bool woken = false;
    bool ok = rv9k_queue_send_from_isr(&((native_queue_t *)queue)->q, item, &woken);

    if (higher_prio_woken) *higher_prio_woken = woken;
    return ok ? RV9_OK : RV9_ERR_NOMEM;
}

uint32_t rv9_queue_count(rv9_queue_t queue)
{
    if (queue == NULL) return 0;
    return rv9k_queue_count(&((native_queue_t *)queue)->q);
}

/* Memory lives in kal_mem.c: allocation on this board comes from
   ESP-IDF whichever kernel backs the KAL, so there is one copy of it. */

/* ---------------- critical sections ---------------- */

void rv9_critical_enter(void)
{
    uint32_t prev = irq_save();
    if (s_critical_depth++ == 0) s_critical_state = prev;
}

void rv9_critical_exit(void)
{
    if (s_critical_depth == 0) return;
    if (--s_critical_depth == 0) irq_restore(s_critical_state);
}

/* ---------------- scheduler lock ---------------- */

void rv9_sched_lock(void)   { rv9k_sched_lock(); }
void rv9_sched_unlock(void) { rv9k_sched_unlock(); }

/* ---------------- instruction sync ---------------- */

void rv9_isync(void)
{
    __asm__ volatile ("fence.i" ::: "memory");
}
