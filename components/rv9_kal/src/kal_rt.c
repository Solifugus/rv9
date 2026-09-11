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
#include "rv9/kernel.h"
#include "kal_internal.h"

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

/*
 * PRIORITY INHERITANCE, IN BOTH SCHEDULERS
 *
 * A FreeRTOS mutex already lends its priority to whoever holds it -- but
 * what it lends to is the *task*, and every RV-9 thread shares one task.
 * Boosting that task makes the kernel run; it does not make the kernel run
 * the thread that holds the lock. RV-9's scheduler picks by its own
 * priorities and will happily run something else while the waiter waits.
 *
 * So inheritance happens twice. The host mutex lends to the task, which
 * gets the kernel scheduled. The lock separately boosts the holding RV-9
 * thread, which gets the *right* thread scheduled inside it. Neither half
 * is sufficient alone.
 *
 * This matters because the waiter is often a control loop. A missed
 * deadline caused by inversion looks like nothing to do with timing: the
 * loop was ready, the lock was held by something trivial, and something
 * medium-priority and irrelevant ran instead.
 */
typedef struct {
    SemaphoreHandle_t mux;
    rv9k_thread_t    *holder;      /* non-NULL when an RV-9 thread holds it */
    bool              boosted;
} lock_impl_t;

static portMUX_TYPE s_lock_guard = portMUX_INITIALIZER_UNLOCKED;

/* Off by default nowhere -- this exists so the demonstration can show what
   inversion costs by turning it off. */
static bool s_inherit = true;

void rv9_lock_set_inheritance(bool on) { s_inherit = on; }
bool rv9_lock_get_inheritance(void)    { return s_inherit; }

rv9_err_t rv9_lock_create(rv9_lock_t *out_lock)
{
    if (out_lock == NULL) return RV9_ERR_INVAL;

    lock_impl_t *l = (lock_impl_t *)calloc(1, sizeof(*l));
    if (l == NULL) return RV9_ERR_NOMEM;

    /* A mutex, not a binary semaphore: the difference is that FreeRTOS
       lends the holder's task its priority. */
    l->mux = xSemaphoreCreateMutex();
    if (l->mux == NULL) {
        free(l);
        return RV9_ERR_NOMEM;
    }

    *out_lock = (rv9_lock_t)l;
    return RV9_OK;
}

void rv9_lock_destroy(rv9_lock_t lock)
{
    lock_impl_t *l = (lock_impl_t *)lock;
    if (l == NULL) return;

    vSemaphoreDelete(l->mux);
    free(l);
}

void rv9_lock_acquire(rv9_lock_t lock)
{
    lock_impl_t *l = (lock_impl_t *)lock;
    if (l == NULL) return;

    /* Uncontended: nothing to inherit, and nothing to pay for it. */
    if (xSemaphoreTake(l->mux, 0) == pdTRUE) {
        portENTER_CRITICAL(&s_lock_guard);
        l->holder = rv9_kal_self_thread();
        portEXIT_CRITICAL(&s_lock_guard);
        return;
    }

    /*
     * Contended. If an RV-9 thread is holding it, lift that thread so the
     * kernel runs it rather than whatever else is runnable. The host mutex
     * takes care of getting the kernel itself scheduled.
     */
    rv9k_thread_t *holder = NULL;

    if (s_inherit) {
        portENTER_CRITICAL(&s_lock_guard);
        holder = l->holder;
        if (holder != NULL) l->boosted = true;
        portEXIT_CRITICAL(&s_lock_guard);

        if (holder != NULL) rv9k_priority_boost(holder, RV9K_PRIO_MAX);
    }

    /*
     * How to wait depends on who is waiting.
     *
     * A host task blocks: that costs it nothing and lets everything else
     * run. An RV-9 thread must NOT block, because every RV-9 thread shares
     * one host task -- blocking it stops the whole kernel, including the
     * thread holding the lock, which can then never release it. The waiter
     * would be deadlocking against its own scheduler.
     *
     * So an RV-9 thread yields through its own scheduler instead, which
     * gives the (now boosted) holder the CPU.
     */
    if (rv9_kal_self_thread() != NULL) {
        /*
         * Sleep, do not yield.
         *
         * Yielding leaves the waiter runnable, and a waiter that outranks
         * the holder is simply picked again -- it spins at full priority
         * while the thread it is waiting for never runs. Sleeping takes it
         * off the run queue entirely, so the holder (boosted above) gets
         * the CPU and can let go.
         *
         * The cost is up to one tick of latency for a contended lock held
         * by an RV-9 thread. Real-time waiters do not pay it: they are
         * host tasks and take the branch below.
         */
        while (xSemaphoreTake(l->mux, 0) != pdTRUE) {
            rv9k_sleep_ticks(1);
        }
    } else {
        xSemaphoreTake(l->mux, portMAX_DELAY);
    }

    portENTER_CRITICAL(&s_lock_guard);
    l->holder = rv9_kal_self_thread();
    portEXIT_CRITICAL(&s_lock_guard);
}

void rv9_lock_release(rv9_lock_t lock)
{
    lock_impl_t *l = (lock_impl_t *)lock;
    if (l == NULL) return;

    portENTER_CRITICAL(&s_lock_guard);
    rv9k_thread_t *holder = l->holder;
    bool boosted = l->boosted;
    l->holder  = NULL;
    l->boosted = false;
    portEXIT_CRITICAL(&s_lock_guard);

    /* Give the borrowed priority back before letting go, so the thread
       cannot keep it by grabbing the lock again immediately. */
    if (boosted && holder != NULL) rv9k_priority_unboost(holder);

    xSemaphoreGive(l->mux);
}

static RV9_RT_CODE rt_task_t *slot_for(TaskHandle_t t)
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

/*
 * Resident. This is the call a control loop is sitting in when the radio
 * decides to write its calibration data to flash; if it lived in flash the
 * loop would not be able to wake up and find out how late it was.
 */
RV9_RT_CODE int rv9_rt_wait(void)
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

    /* Drop any releases still queued: they are periods that came and went
       while this task was elsewhere, and working through a backlog of stale
       deadlines is not what a control loop wants. */
    while (uxSemaphoreGetCount(rt->release) > 0) xSemaphoreTake(rt->release, 0);

    uint64_t woken = rv9_time_us();

    /*
     * Count what was missed from the clock, not from the semaphore.
     *
     * The semaphore holds at most MISS_BACKLOG releases, so counting drains
     * stopped at 8 however long the stall was -- a 200 ms hole in a 1 kHz
     * loop reported seven missed periods instead of a hundred and ninety
     * nine. An instrument that saturates just where it matters is worse than
     * none: it says "slightly late" about a loop that stopped.
     */
    uint64_t gap = (woken > rt->released_at_us)
                   ? (woken - rt->released_at_us) : 0;

    /*
     * Lateness is the whole overshoot, not the remainder after whole
     * periods are taken out of it. Reporting the remainder was the same
     * mistake in a different place: a loop stalled for 200 ms and one
     * stalled for 200 us both came back "late by a little".
     */
    uint32_t jitter = (gap > rt->period_us)
                      ? (uint32_t)(gap - rt->period_us) : 0;
    if (jitter > rt->max_jitter_us) rt->max_jitter_us = jitter;

    int missed = (int)(jitter / rt->period_us);

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
