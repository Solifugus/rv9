/*
 * RV-9 KAL -- FreeRTOS backend.
 *
 * Thin wrappers, deliberately. The value here is not the code, it is that
 * everything above stops depending on FreeRTOS. Resist adding cleverness:
 * every behaviour invented here becomes a requirement the native kernel
 * must reproduce in phase 7.
 *
 * This file is BELOW the seam and is the one place FreeRTOS headers are
 * expected.
 */
#include "rv9/kal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#define RV9_DEFAULT_STACK_BYTES 3072

/* RV-9 priorities are 0..RV9_PRIO_MAX and must land inside FreeRTOS's range
   with headroom left for the idle task. */
static inline UBaseType_t prio_to_native(int prio)
{
    if (prio < RV9_PRIO_IDLE) prio = RV9_PRIO_IDLE;
    if (prio > RV9_PRIO_MAX)  prio = RV9_PRIO_MAX;

    UBaseType_t native = (UBaseType_t)prio + 1; /* keep 0 for the idle task */
    if (native >= configMAX_PRIORITIES) {
        native = configMAX_PRIORITIES - 1;
    }
    return native;
}

static inline TickType_t timeout_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == RV9_WAIT_FOREVER) return portMAX_DELAY;
    if (timeout_ms == RV9_NO_WAIT)      return 0;
    return pdMS_TO_TICKS(timeout_ms);
}

const char *rv9_strerror(rv9_err_t err)
{
    switch (err) {
    case RV9_OK:              return "ok";
    case RV9_ERR_INVAL:       return "invalid argument";
    case RV9_ERR_NOMEM:       return "out of memory";
    case RV9_ERR_TIMEOUT:     return "timed out";
    case RV9_ERR_UNSUPPORTED: return "unsupported";
    default:                  return "unknown error";
    }
}

/* ---------------- time ---------------- */

uint64_t rv9_time_us(void) { return (uint64_t)esp_timer_get_time(); }
uint64_t rv9_time_ms(void) { return (uint64_t)esp_timer_get_time() / 1000ULL; }

uint32_t rv9_ms_to_ticks(uint32_t ms) { return (uint32_t)pdMS_TO_TICKS(ms); }

/* ---------------- tasks ---------------- */

rv9_err_t rv9_task_create(rv9_task_fn fn, const char *name, size_t stack_bytes,
                          void *arg, int priority, rv9_task_t *out_task)
{
    if (fn == NULL) return RV9_ERR_INVAL;
    if (stack_bytes == 0) stack_bytes = RV9_DEFAULT_STACK_BYTES;

    TaskHandle_t handle = NULL;
    BaseType_t ok = xTaskCreate((TaskFunction_t)fn,
                                name ? name : "rv9",
                                (uint32_t)stack_bytes,   /* ESP-IDF: bytes */
                                arg,
                                prio_to_native(priority),
                                &handle);
    if (ok != pdPASS) return RV9_ERR_NOMEM;

    if (out_task) *out_task = (rv9_task_t)handle;
    return RV9_OK;
}

rv9_err_t rv9_kal_start(rv9_task_fn fn, const char *name, size_t stack_bytes,
                        void *arg, int priority)
{
    /* FreeRTOS is already running by the time app_main is called, so
       starting the system is just starting its first task. */
    return rv9_task_create(fn, name, stack_bytes, arg, priority, NULL);
}

void rv9_task_delete(rv9_task_t task)
{
    vTaskDelete((TaskHandle_t)task);   /* NULL means "this task" */
}

rv9_task_t rv9_task_self(void)
{
    return (rv9_task_t)xTaskGetCurrentTaskHandle();
}

void rv9_task_yield(void) { taskYIELD(); }

void rv9_task_delay_ms(uint32_t ms)
{
    /* Always yield at least one tick, so delay(0) is not a busy spin. */
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

rv9_err_t rv9_task_priority_set(rv9_task_t task, int priority)
{
    vTaskPrioritySet((TaskHandle_t)task, prio_to_native(priority));
    return RV9_OK;
}

/* FreeRTOS schedules strictly by priority and never ages. */
bool rv9_sched_ages(void) { return false; }

/* Nothing to do: this scheduler takes the CPU whenever it wants it. */
void rv9_preempt_point(void) { }

/* ---------------- semaphores ---------------- */

rv9_err_t rv9_sem_create(uint32_t max_count, uint32_t initial_count,
                         rv9_sem_t *out_sem)
{
    if (out_sem == NULL || max_count == 0 || initial_count > max_count) {
        return RV9_ERR_INVAL;
    }
    SemaphoreHandle_t h = xSemaphoreCreateCounting(max_count, initial_count);
    if (h == NULL) return RV9_ERR_NOMEM;

    *out_sem = (rv9_sem_t)h;
    return RV9_OK;
}

void rv9_sem_destroy(rv9_sem_t sem)
{
    if (sem) vSemaphoreDelete((SemaphoreHandle_t)sem);
}

rv9_err_t rv9_sem_take(rv9_sem_t sem, uint32_t timeout_ms)
{
    if (sem == NULL) return RV9_ERR_INVAL;
    return xSemaphoreTake((SemaphoreHandle_t)sem, timeout_to_ticks(timeout_ms))
           == pdTRUE ? RV9_OK : RV9_ERR_TIMEOUT;
}

rv9_err_t rv9_sem_give(rv9_sem_t sem)
{
    if (sem == NULL) return RV9_ERR_INVAL;
    /* Failure here means the count is already at max, which is a caller bug. */
    return xSemaphoreGive((SemaphoreHandle_t)sem) == pdTRUE
           ? RV9_OK : RV9_ERR_INVAL;
}

rv9_err_t rv9_sem_give_from_isr(rv9_sem_t sem, bool *higher_prio_woken)
{
    if (sem == NULL) return RV9_ERR_INVAL;

    BaseType_t woken = pdFALSE;
    BaseType_t ok = xSemaphoreGiveFromISR((SemaphoreHandle_t)sem, &woken);
    if (higher_prio_woken) *higher_prio_woken = (woken == pdTRUE);
    return ok == pdTRUE ? RV9_OK : RV9_ERR_INVAL;
}

/* ---------------- mutexes ---------------- */

/*
 * A recursive mutex must be taken with the recursive calls; the ordinary
 * ones block it against itself. Which handle it is cannot be recovered
 * from the handle, so the mutex remembers.
 *
 * This was wrong from phase 0 until the conformance suite grew a test that
 * actually locked a recursive mutex twice. The old suite only checked that
 * one could be created.
 */
typedef struct {
    SemaphoreHandle_t handle;
    bool              recursive;
} kal_mutex_t;

static rv9_err_t mutex_make(rv9_mutex_t *out_mutex, bool recursive)
{
    if (out_mutex == NULL) return RV9_ERR_INVAL;

    kal_mutex_t *m = (kal_mutex_t *)malloc(sizeof(*m));
    if (m == NULL) return RV9_ERR_NOMEM;

    m->handle = recursive ? xSemaphoreCreateRecursiveMutex()
                          : xSemaphoreCreateMutex();
    m->recursive = recursive;

    if (m->handle == NULL) {
        free(m);
        return RV9_ERR_NOMEM;
    }

    *out_mutex = (rv9_mutex_t)m;
    return RV9_OK;
}

rv9_err_t rv9_mutex_create(rv9_mutex_t *out_mutex)
{
    return mutex_make(out_mutex, false);
}

rv9_err_t rv9_mutex_create_recursive(rv9_mutex_t *out_mutex)
{
    return mutex_make(out_mutex, true);
}

void rv9_mutex_destroy(rv9_mutex_t mutex)
{
    kal_mutex_t *m = (kal_mutex_t *)mutex;
    if (m == NULL) return;

    vSemaphoreDelete(m->handle);
    free(m);
}

rv9_err_t rv9_mutex_lock(rv9_mutex_t mutex, uint32_t timeout_ms)
{
    kal_mutex_t *m = (kal_mutex_t *)mutex;
    if (m == NULL) return RV9_ERR_INVAL;

    TickType_t ticks = timeout_to_ticks(timeout_ms);
    BaseType_t ok = m->recursive
                  ? xSemaphoreTakeRecursive(m->handle, ticks)
                  : xSemaphoreTake(m->handle, ticks);

    return ok == pdTRUE ? RV9_OK : RV9_ERR_TIMEOUT;
}

rv9_err_t rv9_mutex_unlock(rv9_mutex_t mutex)
{
    kal_mutex_t *m = (kal_mutex_t *)mutex;
    if (m == NULL) return RV9_ERR_INVAL;

    BaseType_t ok = m->recursive ? xSemaphoreGiveRecursive(m->handle)
                                 : xSemaphoreGive(m->handle);

    return ok == pdTRUE ? RV9_OK : RV9_ERR_INVAL;
}

/* ---------------- queues ---------------- */

rv9_err_t rv9_queue_create(uint32_t length, size_t item_size,
                           rv9_queue_t *out_queue)
{
    if (out_queue == NULL || length == 0 || item_size == 0) {
        return RV9_ERR_INVAL;
    }
    QueueHandle_t h = xQueueCreate(length, item_size);
    if (h == NULL) return RV9_ERR_NOMEM;

    *out_queue = (rv9_queue_t)h;
    return RV9_OK;
}

void rv9_queue_destroy(rv9_queue_t queue)
{
    if (queue) vQueueDelete((QueueHandle_t)queue);
}

rv9_err_t rv9_queue_send(rv9_queue_t queue, const void *item,
                         uint32_t timeout_ms)
{
    if (queue == NULL || item == NULL) return RV9_ERR_INVAL;
    return xQueueSend((QueueHandle_t)queue, item, timeout_to_ticks(timeout_ms))
           == pdTRUE ? RV9_OK : RV9_ERR_TIMEOUT;
}

rv9_err_t rv9_queue_recv(rv9_queue_t queue, void *item, uint32_t timeout_ms)
{
    if (queue == NULL || item == NULL) return RV9_ERR_INVAL;
    return xQueueReceive((QueueHandle_t)queue, item, timeout_to_ticks(timeout_ms))
           == pdTRUE ? RV9_OK : RV9_ERR_TIMEOUT;
}

rv9_err_t rv9_queue_send_from_isr(rv9_queue_t queue, const void *item,
                                  bool *higher_prio_woken)
{
    if (queue == NULL || item == NULL) return RV9_ERR_INVAL;

    BaseType_t woken = pdFALSE;
    BaseType_t ok = xQueueSendFromISR((QueueHandle_t)queue, item, &woken);
    if (higher_prio_woken) *higher_prio_woken = (woken == pdTRUE);
    return ok == pdTRUE ? RV9_OK : RV9_ERR_TIMEOUT;
}

uint32_t rv9_queue_count(rv9_queue_t queue)
{
    if (queue == NULL) return 0;
    return (uint32_t)uxQueueMessagesWaiting((QueueHandle_t)queue);
}

/* ---------------- memory ---------------- */

void *rv9_alloc(size_t size)                 { return malloc(size); }
void *rv9_calloc(size_t count, size_t size)  { return calloc(count, size); }
void  rv9_free(void *ptr)                    { free(ptr); }

void *rv9_alloc_dma(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}

void *rv9_alloc_exec(size_t size)
{
    void *p = NULL;

#ifdef MALLOC_CAP_EXEC
    p = heap_caps_malloc(size, MALLOC_CAP_EXEC | MALLOC_CAP_8BIT);
    if (p != NULL) return p;
#endif

    /* Fall back to plain internal memory. On the C5, IRAM and DRAM are the
       same physical range (SOC_IRAM_LOW == SOC_DRAM_LOW), so internal RAM is
       executable whether or not the heap advertises MALLOC_CAP_EXEC -- that
       flag only exists when ESP_SYSTEM_MEMPROT is off. Insist on internal:
       external PSRAM would not be executable on a future board. */
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t rv9_heap_free_exec(void)
{
#ifdef MALLOC_CAP_EXEC
    return heap_caps_get_free_size(MALLOC_CAP_EXEC | MALLOC_CAP_8BIT);
#else
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
}

size_t rv9_heap_free(void)      { return heap_caps_get_free_size(MALLOC_CAP_DEFAULT); }
size_t rv9_heap_low_water(void) { return heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT); }

/* ---------------- critical sections ---------------- */

static portMUX_TYPE s_rv9_lock = portMUX_INITIALIZER_UNLOCKED;

void rv9_critical_enter(void) { portENTER_CRITICAL(&s_rv9_lock); }
void rv9_critical_exit(void)  { portEXIT_CRITICAL(&s_rv9_lock); }

/* ---------------- scheduler lock ---------------- */

void rv9_sched_lock(void)   { vTaskSuspendAll(); }
void rv9_sched_unlock(void) { (void)xTaskResumeAll(); }

/* ---------------- instruction sync ---------------- */

void rv9_isync(void)
{
    /* The C5 has unified IRAM/DRAM, so freshly written code is visible to
       the fetch unit once the pipeline is flushed. */
    __asm__ volatile ("fence.i" ::: "memory");
}
