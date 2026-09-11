/*
 * Real-time tasks.
 *
 * WHY THESE ARE NOT ORDINARY RV-9 THREADS
 *
 * RV-9's own scheduler is cooperative: it switches when asked. That is
 * fine for work that is merely important and useless for work that is
 * late if it is late -- a control loop cannot depend on every other
 * thread in the system being polite.
 *
 * So a real-time task is not an RV-9 thread. It runs on the host's
 * preemptive scheduler, above everything else including the task RV-9's
 * kernel lives in, and is released by a hardware timer rather than by a
 * software tick. Nothing below the microsecond timer is in its way.
 *
 * This is deliberately the same shape the native implementation will have
 * when RV-9 owns the machine: a preemptible thread at the top of the
 * priority order, released by a hardware comparator. The API does not
 * change when the implementation moves -- which matters, because code
 * written against it is control code, and control code should not be
 * rewritten because the kernel underneath grew up.
 *
 * WHAT IS MEASURED, AND WHY
 *
 * "Real-time" is a property you measure, not a claim you make. Every
 * release records how late it was (jitter) and how long the work took
 * (execution). A period that cannot be met is counted as an overrun
 * rather than quietly absorbed, because a control loop that silently
 * misses deadlines is worse than one that stops.
 */
#include "rv9/kal.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "rv9-rt";

#define MAX_RT_TASKS 4
#define MISS_BACKLOG 8      /* releases the semaphore may hold before we
                               stop counting; more than this is not late,
                               it is broken */

typedef struct {
    TaskHandle_t       task;
    SemaphoreHandle_t  release;
    esp_timer_handle_t timer;
    uint32_t           period_us;

    /* Measurement. A control loop is only as trustworthy as its numbers. */
    uint64_t           activations;
    uint64_t           overruns;
    uint32_t           max_jitter_us;
    uint32_t           max_exec_us;
    uint32_t           last_exec_us;
    uint64_t           released_at_us;   /* when this activation began */
    bool               in_use;
} rt_task_t;

static rt_task_t s_rt[MAX_RT_TASKS];

/* ------------------------------------------------------------------ */
/* Locks usable from any context                                        */
/*                                                                     */
/* Host mutexes in both builds, deliberately. The data they guard is    */
/* shared between RV-9 threads and host-scheduled work, and only the    */
/* host's scheduler knows about both.                                   */
/* ------------------------------------------------------------------ */

rv9_err_t rv9_lock_create(rv9_lock_t *out_lock)
{
    if (out_lock == NULL) return RV9_ERR_INVAL;

    SemaphoreHandle_t h = xSemaphoreCreateMutex();
    if (h == NULL) return RV9_ERR_NOMEM;

    *out_lock = (rv9_lock_t)h;
    return RV9_OK;
}

void rv9_lock_destroy(rv9_lock_t lock)
{
    if (lock) vSemaphoreDelete((SemaphoreHandle_t)lock);
}

void rv9_lock_acquire(rv9_lock_t lock)
{
    if (lock) xSemaphoreTake((SemaphoreHandle_t)lock, portMAX_DELAY);
}

void rv9_lock_release(rv9_lock_t lock)
{
    if (lock) xSemaphoreGive((SemaphoreHandle_t)lock);
}

static rt_task_t *slot_for(TaskHandle_t t)
{
    for (int i = 0; i < MAX_RT_TASKS; i++) {
        if (s_rt[i].in_use && s_rt[i].task == t) return &s_rt[i];
    }
    return NULL;
}

/*
 * IRAM, and it touches nothing that lives in flash: an interrupt can
 * arrive while the flash cache is disabled, and a release that faults
 * then would be a control loop that stops when the radio writes NVS.
 */
static void IRAM_ATTR release_isr(void *arg)
{
    rt_task_t *rt = (rt_task_t *)arg;
    BaseType_t woken = pdFALSE;

    xSemaphoreGiveFromISR(rt->release, &woken);
    if (woken) portYIELD_FROM_ISR();
}

rv9_err_t rv9_rt_declare(uint32_t period_us)
{
    if (period_us == 0) return RV9_ERR_INVAL;

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (slot_for(self) != NULL) return RV9_ERR_INVAL;   /* already declared */

    rt_task_t *rt = NULL;
    for (int i = 0; i < MAX_RT_TASKS; i++) {
        if (!s_rt[i].in_use) { rt = &s_rt[i]; break; }
    }
    if (rt == NULL) return RV9_ERR_NOMEM;

    memset(rt, 0, sizeof(*rt));
    rt->task      = self;
    rt->period_us = period_us;

    /* Counting, so that a release arriving while the task is still busy is
       remembered rather than lost. Lost releases would make a loop that
       misses deadlines look punctual. */
    rt->release = xSemaphoreCreateCounting(MISS_BACKLOG, 0);
    if (rt->release == NULL) return RV9_ERR_NOMEM;

    const esp_timer_create_args_t args = {
        .callback        = release_isr,
        .arg             = rt,
        .dispatch_method = ESP_TIMER_ISR,
        .name            = "rv9-rt",
    };
    if (esp_timer_create(&args, &rt->timer) != ESP_OK) {
        vSemaphoreDelete(rt->release);
        return RV9_ERR_NOMEM;
    }

    rt->in_use = true;

    if (esp_timer_start_periodic(rt->timer, period_us) != ESP_OK) {
        rt->in_use = false;
        return RV9_ERR_NOMEM;
    }

    rt->released_at_us = rv9_time_us();

    ESP_LOGI(TAG, "real-time task declared: %lu us period (%lu Hz)",
             (unsigned long)period_us,
             (unsigned long)(1000000UL / period_us));
    return RV9_OK;
}

int rv9_rt_wait(void)
{
    rt_task_t *rt = slot_for(xTaskGetCurrentTaskHandle());
    if (rt == NULL) return -1;

    /* Charge this activation before sleeping, so execution time is the
       work itself and not the work plus the wait. */
    uint64_t now = rv9_time_us();
    uint32_t exec = (uint32_t)(now - rt->released_at_us);
    rt->last_exec_us = exec;
    if (exec > rt->max_exec_us) rt->max_exec_us = exec;

    if (xSemaphoreTake(rt->release, portMAX_DELAY) != pdTRUE) return -1;

    /*
     * Anything still queued is a period that came and went while this task
     * was busy. Drain it, count it, and tell the caller -- a control loop
     * that has fallen behind usually needs to know, and sometimes needs to
     * skip ahead rather than work through a backlog.
     */
    int missed = 0;
    while (uxSemaphoreGetCount(rt->release) > 0) {
        xSemaphoreTake(rt->release, 0);
        missed++;
    }

    uint64_t woken = rv9_time_us();
    uint64_t expected = rt->released_at_us + (uint64_t)rt->period_us
                        * (uint64_t)(missed + 1);
    uint32_t jitter = (woken > expected) ? (uint32_t)(woken - expected) : 0;
    if (jitter > rt->max_jitter_us) rt->max_jitter_us = jitter;

    rt->released_at_us = woken;
    rt->activations++;
    rt->overruns += (uint64_t)missed;

    return missed;
}

void rv9_rt_release(void)
{
    rt_task_t *rt = slot_for(xTaskGetCurrentTaskHandle());
    if (rt == NULL) return;

    esp_timer_stop(rt->timer);
    esp_timer_delete(rt->timer);
    vSemaphoreDelete(rt->release);

    rt->in_use = false;
    rt->task   = NULL;
}

rv9_err_t rv9_rt_stats(rv9_rt_stats_t *out)
{
    rt_task_t *rt = slot_for(xTaskGetCurrentTaskHandle());
    if (rt == NULL || out == NULL) return RV9_ERR_INVAL;

    out->period_us     = rt->period_us;
    out->activations   = rt->activations;
    out->overruns      = rt->overruns;
    out->max_jitter_us = rt->max_jitter_us;
    out->max_exec_us   = rt->max_exec_us;
    out->last_exec_us  = rt->last_exec_us;
    return RV9_OK;
}

rv9_err_t rv9_rt_stats_by_index(int index, rv9_rt_stats_t *out, bool *valid)
{
    if (index < 0 || index >= MAX_RT_TASKS || out == NULL) {
        return RV9_ERR_INVAL;
    }
    rt_task_t *rt = &s_rt[index];
    if (valid) *valid = rt->in_use;
    if (!rt->in_use) return RV9_OK;

    out->period_us     = rt->period_us;
    out->activations   = rt->activations;
    out->overruns      = rt->overruns;
    out->max_jitter_us = rt->max_jitter_us;
    out->max_exec_us   = rt->max_exec_us;
    out->last_exec_us  = rt->last_exec_us;
    return RV9_OK;
}

rv9_err_t rv9_task_create_rt(rv9_task_fn fn, const char *name,
                             size_t stack_bytes, void *arg,
                             rv9_task_t *out_task)
{
    if (fn == NULL) return RV9_ERR_INVAL;
    if (stack_bytes == 0) stack_bytes = 4096;

    TaskHandle_t h = NULL;

    /*
     * Above everything: above ordinary RV-9 processes, above the task
     * RV-9's kernel runs in, above the drivers. The whole point of a
     * real-time class is that nothing in the system outranks it.
     */
    if (xTaskCreate((TaskFunction_t)fn, name ? name : "rv9-rt",
                    (uint32_t)stack_bytes, arg,
                    configMAX_PRIORITIES - 1, &h) != pdPASS) {
        return RV9_ERR_NOMEM;
    }

    if (out_task) *out_task = (rv9_task_t)h;
    return RV9_OK;
}
