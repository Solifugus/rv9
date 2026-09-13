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
 * kernel lives in, and is released by hardware -- a microsecond timer, or
 * an interrupt from the device it is controlling -- rather than by a
 * software tick. Nothing below the interrupt is in its way.
 *
 * TWO RELEASE SOURCES, ONE CALL
 *
 * A periodic task is released by a timer; an event-driven (sporadic) one
 * by an interrupt. Both sit in rv9_rt_wait(), and both are measured the
 * same way, because the question is the same: how long after the release
 * should have happened did this code actually run? Only the clock differs
 * -- one we own, one we do not. Keeping it one call means a control loop
 * can change what wakes it without being rewritten, which matters when the
 * thing being written against this is a language for reactive systems.
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

#include "riscv/rvruntime-frames.h"

static const char *TAG = "rv9-rt";

#define MAX_RT_TASKS 4
#define MAX_EVENTS   8
#define MISS_BACKLOG 8      /* releases the semaphore may hold before we
                               stop counting; more than this is not late,
                               it is broken */

typedef struct rv9_event {
    SemaphoreHandle_t sem;
    uint64_t          at_us;    /* when the oldest unserviced signal arrived */
    uint32_t          pending;  /* signals since the waiter last looked */
    uint64_t          last_us;  /* when the previous signal arrived */
    bool              in_use;
} event_impl_t;

static event_impl_t s_events[MAX_EVENTS];
static portMUX_TYPE s_event_guard = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    TaskHandle_t       task;
    SemaphoreHandle_t  release;
    esp_timer_handle_t timer;
    uint32_t           period_us;

    /* Non-NULL when this task is released by an event rather than a timer.
       The two are exclusive: a task has one release source, or its numbers
       mean nothing. */
    event_impl_t      *event;

    /* Measurement. A control loop is only as trustworthy as its numbers. */
    uint64_t           activations;
    uint64_t           overruns;
    uint32_t           max_jitter_us;
    uint32_t           max_exec_us;
    uint32_t           last_exec_us;
    uint32_t           min_interval_us;  /* shortest arrival gap seen */
    uint64_t           floods;           /* arrivals inside the declared gap */
    bool               flood_reported;
    uint64_t           released_at_us;   /* when this activation began */

    /*
     * The schedule, and how late against it.
     *
     * due_us is when the next periodic release *should* happen, advanced
     * by whole periods rather than restarted from each wakeup. Measuring
     * lateness from the previous wakeup instead -- which this did until
     * deadlines were enforced -- forgives lateness that accumulates: a
     * loop late by 500 us and then by 600 reports 100 for the second,
     * because it only counts what got worse. That was tolerable as a
     * statistic. It is not tolerable as the thing that decides whether a
     * process is stopped.
     */
    uint64_t           due_us;
    uint32_t           late_us;          /* this activation's lateness */

    uint32_t           deadline_us;      /* 0: not checked */
    bool               fault_on_miss;
    uint64_t           deadline_misses;
    uint32_t           max_response_us;
    uint32_t           last_response_us;

    /* Set by rv9_rt_stop from another task; read here between activations. */
    volatile bool      stop;

    /*
     * What the watchdog looks at.
     *
     * `active` is true from a release until the task comes back to wait,
     * and is false whenever released_at_us is being rewritten -- a 64-bit
     * store is two on this CPU, and an interrupt reading between them would
     * see a release from somewhere else in time.
     */
    volatile bool      active;
    volatile uint32_t  samples;     /* watchdog ticks that found it running */
    volatile int       flagged;     /* 0, or the RV9_RT_* it must not pass */

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
        /* Counted on the thread, so nothing stops it while it holds this:
           see rv9_task_kill. */
        if (l->holder) l->holder->holds++;
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
    if (l->holder) l->holder->holds++;
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
    if (holder && holder->holds > 0) holder->holds--;
    portEXIT_CRITICAL(&s_lock_guard);

    /* Give the borrowed priority back before letting go, so the thread
       cannot keep it by grabbing the lock again immediately. */
    if (boosted && holder != NULL) rv9k_priority_unboost(holder);

    xSemaphoreGive(l->mux);
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

rv9_err_t rv9_event_create(rv9_event_t *out_event)
{
    if (out_event == NULL) return RV9_ERR_INVAL;

    /*
     * A fixed pool, not the heap. An event is signalled from an interrupt
     * handler, so it must live somewhere that is always mapped -- and being
     * able to name one by a small index is what lets a driver hand it out
     * through getstat without a pointer crossing the seam.
     */
    event_impl_t *e = NULL;
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (!s_events[i].in_use) { e = &s_events[i]; break; }
    }
    if (e == NULL) return RV9_ERR_NOMEM;

    memset(e, 0, sizeof(*e));

    /* Counting, so a signal arriving while the waiter is busy is remembered
       rather than lost. How many were coalesced is counted separately. */
    e->sem = xSemaphoreCreateCounting(MISS_BACKLOG, 0);
    if (e->sem == NULL) return RV9_ERR_NOMEM;

    e->in_use = true;
    *out_event = (rv9_event_t)e;
    return RV9_OK;
}

void rv9_event_destroy(rv9_event_t ev)
{
    event_impl_t *e = (event_impl_t *)ev;
    if (e == NULL || !e->in_use) return;

    /*
     * Whoever is waiting must be gone first. There is no way to check that
     * here, which is why the only caller is a driver closing a unit it
     * opened -- the path is closed, so nothing can still be armed on it.
     */
    e->in_use = false;
    vSemaphoreDelete(e->sem);
    e->sem = NULL;
}

int rv9_event_id(rv9_event_t ev)
{
    event_impl_t *e = (event_impl_t *)ev;
    if (e == NULL || !e->in_use) return 0;
    return (int)(e - s_events) + 1;      /* 0 means "no event" */
}

rv9_event_t rv9_event_by_id(int id)
{
    if (id < 1 || id > MAX_EVENTS) return NULL;
    event_impl_t *e = &s_events[id - 1];
    return e->in_use ? (rv9_event_t)e : NULL;
}

/*
 * Resident, and so is everything it calls. This runs in an interrupt
 * handler installed with ESP_INTR_FLAG_IRAM, which means it may be entered
 * while the flash cache is off -- reaching anything in flash from here
 * would not be slow, it would be a panic.
 */
RV9_RT_CODE void rv9_event_signal_from_isr(rv9_event_t ev)
{
    event_impl_t *e = (event_impl_t *)ev;
    if (e == NULL) return;

    uint64_t now = rv9_time_us();

    portENTER_CRITICAL_ISR(&s_event_guard);
    /*
     * Stamp the OLDEST unserviced signal, not the newest.
     *
     * If two edges arrive before the task runs, the honest latency is
     * measured from the first one: that is how long the system actually
     * took to respond to something that had already happened. Stamping the
     * newest would quietly subtract the part of the delay that was our
     * fault, which is the direction an instrument must never round in.
     */
    if (e->pending == 0) e->at_us = now;
    e->pending++;
    portEXIT_CRITICAL_ISR(&s_event_guard);

    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(e->sem, &woken);

    /* Yielding from an interrupt is the host kernel's business, and it
       stays on this side of the seam. */
    if (woken) portYIELD_FROM_ISR();
}

void rv9_event_signal(rv9_event_t ev)
{
    event_impl_t *e = (event_impl_t *)ev;
    if (e == NULL) return;

    uint64_t now = rv9_time_us();

    portENTER_CRITICAL(&s_event_guard);
    if (e->pending == 0) e->at_us = now;
    e->pending++;
    portEXIT_CRITICAL(&s_event_guard);

    xSemaphoreGive(e->sem);
}

/* ------------------------------------------------------------------ */
/* Real-time tasks                                                     */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* The watchdog                                                        */
/*                                                                     */
/* Everything else here acts when a task comes to wait. This is for the */
/* task that does not come.                                             */
/* ------------------------------------------------------------------ */

/*
 * Every 2 ms. The resolution a deadline is enforced to while a task is
 * still running, and a sample rate for deciding whether it is running at
 * all. Four slots and a comparison each; the cost is the interrupt.
 */
#define WATCH_US   2000

static esp_timer_handle_t s_watch_timer;
static SemaphoreHandle_t  s_watch_wake;
static TaskHandle_t       s_watch_task;
static rv9_rt_overrun_fn  s_overrun;
static volatile int       s_watch_started;

void rv9_rt_set_overrun_handler(rv9_rt_overrun_fn fn) { s_overrun = fn; }

/*
 * The running task, read directly rather than asked for.
 *
 * xTaskGetCurrentTaskHandle is linked into flash, and this is read from an
 * interrupt that runs while the flash cache is off -- the first boot with
 * the watchdog in it panicked with a cache error the moment WiFi wrote its
 * calibration to NVS during a test. Every other call in watch_isr was
 * checked against the linked image and is resident; this is the one that
 * was not. The variable is what the port's own interrupt entry reads, and
 * it is in RAM. Single core, so element zero.
 */
extern TaskHandle_t volatile pxCurrentTCBs[];

/*
 * Resident: see release_isr. It decides and flags, nothing more -- stopping
 * a task means clearing up after it, and none of that belongs in an
 * interrupt.
 */
static void IRAM_ATTR watch_isr(void *arg)
{
    (void)arg;

    /* The task this interrupt interrupted. Single core, so there is one. */
    TaskHandle_t cur = pxCurrentTCBs[0];
    uint64_t now = rv9_time_us();
    bool wake = false;

    for (int i = 0; i < MAX_RT_TASKS; i++) {
        rt_task_t *rt = &s_rt[i];
        if (!rt->in_use || !rt->active || rt->flagged) continue;

        if (rt->task == cur) rt->samples++;

        uint64_t ran = (now > rt->released_at_us)
                       ? now - rt->released_at_us : 0;

        /*
         * A deadline that has passed with the work unfinished is a miss
         * now, not when the work eventually finishes -- which, for a loop
         * that has stopped, is never.
         */
        if (rt->fault_on_miss && rt->deadline_us != 0 &&
            rt->activations > 0 && ran + rt->late_us > rt->deadline_us) {
            rt->flagged = RV9_RT_DEADLINE;
        }
        /*
         * Runaway: in one activation for longer than the limit, and on the
         * CPU for at least half of it. The second half of the test is what
         * tells a loop spinning from one blocked in a slow write.
         */
        else if (ran > (uint64_t)RV9_RT_RUNAWAY_MS * 1000u &&
                 (uint64_t)rt->samples * WATCH_US * 2u >= ran) {
            rt->flagged = RV9_RT_RUNAWAY;
        }

        if (rt->flagged) wake = true;
    }

    if (wake) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_watch_wake, &woken);
        if (woken) portYIELD_FROM_ISR();
    }
}

/*
 * At the real-time priority, so that a runaway at that priority shares
 * the CPU with it -- time slicing hands it a tick -- rather than starving
 * it. Anything lower would be waiting for the very task it exists to stop.
 */
static void watch_task(void *arg)
{
    (void)arg;
    bool again = false;

    for (;;) {
        xSemaphoreTake(s_watch_wake, again ? 1 : portMAX_DELAY);
        again = false;

        for (int i = 0; i < MAX_RT_TASKS; i++) {
            rt_task_t *rt = &s_rt[i];
            int why = rt->flagged;
            TaskHandle_t t = rt->task;
            if (!rt->in_use || why == 0 || t == NULL) continue;

            /* With nobody to hand it to, the flag still ends the task if it
               ever waits. Asking again every tick would change nothing. */
            if (s_overrun != NULL && !s_overrun((rv9_task_t)t, why)) {
                again = true;
            }
        }
    }
}

/* Once, at the first declaration: nothing to watch before that. */
static void watch_start(void)
{
    int expected = 0;
    if (!__atomic_compare_exchange_n(&s_watch_started, &expected, 1, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        return;
    }

    s_watch_wake = xSemaphoreCreateBinary();
    if (s_watch_wake == NULL ||
        xTaskCreate(watch_task, "rv9-rtwatch", 6144, NULL,
                    configMAX_PRIORITIES - 1, &s_watch_task) != pdPASS) {
        ESP_LOGE(TAG, "no watchdog: a real-time task that stops waiting "
                      "will not be stopped");
        return;
    }

    const esp_timer_create_args_t args = {
        .callback        = watch_isr,
        .dispatch_method = ESP_TIMER_ISR,
        .name            = "rv9-rtwatch",
    };
    if (esp_timer_create(&args, &s_watch_timer) != ESP_OK ||
        esp_timer_start_periodic(s_watch_timer, WATCH_US) != ESP_OK) {
        ESP_LOGE(TAG, "no watchdog timer");
        return;
    }

    ESP_LOGI(TAG, "watchdog every %d us: runaway after %d ms", WATCH_US,
             RV9_RT_RUNAWAY_MS);
}

rv9_err_t rv9_rt_seize(rv9_task_t task, const void *code, size_t len,
                       bool lower)
{
    TaskHandle_t t = (TaskHandle_t)task;
    if (t == NULL || t == xTaskGetCurrentTaskHandle()) return RV9_ERR_INVAL;

    rv9_err_t err;

    /*
     * With the scheduler held the task cannot run, and it is not running
     * now -- this is -- so its context is saved and stays saved while it is
     * read. On this port every switch, voluntary or not, goes through the
     * interrupt entry, which leaves a full frame at pxTopOfStack (the first
     * word of the TCB) with the program counter first.
     */
    vTaskSuspendAll();

    rt_task_t *rt = slot_for(t);
    if (rt == NULL) {
        err = RV9_ERR_INVAL;
    } else {
        const RvExcFrame *frame = *(RvExcFrame **)t;
        uintptr_t pc = (uintptr_t)frame->mepc;
        uintptr_t lo = (uintptr_t)code;

        if (code != NULL && pc >= lo && pc < lo + len) {
            /* In its own code, so not blocked -- a module has no way to
               block except by calling out -- and holding nothing. */
            vTaskSuspend(t);
            rt->active = false;
            err = RV9_OK;
        } else {
            /* Lowering is not suspending: it leaves the task able to finish
               what it is in and let go, and lets everything else run. */
            if (lower) vTaskPrioritySet(t, tskIDLE_PRIORITY + 1);
            err = RV9_ERR_BUSY;
        }
    }

    xTaskResumeAll();
    return err;
}

/* A slot belongs to a task, not to a release source: the accounting is the
   same either way, and only the thing that wakes it differs. */
static rt_task_t *claim_slot(TaskHandle_t self)
{
    watch_start();

    if (slot_for(self) != NULL) return NULL;            /* already declared */

    for (int i = 0; i < MAX_RT_TASKS; i++) {
        if (!s_rt[i].in_use) {
            memset(&s_rt[i], 0, sizeof(s_rt[i]));
            s_rt[i].task = self;
            s_rt[i].min_interval_us = UINT32_MAX;
            return &s_rt[i];
        }
    }
    return NULL;
}

rv9_err_t rv9_rt_declare_event(rv9_event_t ev, uint32_t min_interval_us)
{
    event_impl_t *e = (event_impl_t *)ev;
    if (e == NULL || !e->in_use) return RV9_ERR_INVAL;

    rt_task_t *rt = claim_slot(xTaskGetCurrentTaskHandle());
    if (rt == NULL) return RV9_ERR_NOMEM;

    rt->event     = e;
    rt->period_us = min_interval_us;   /* the declared bound, not a period */

    /*
     * Start from now rather than from the first event.
     *
     * The alternative -- leave released_at_us at zero and let the first
     * wait compute a gap since the epoch -- makes the first activation
     * report an execution time of however long the board has been up. An
     * instrument's first reading should not be its worst.
     */
    rt->released_at_us = rv9_time_us();
    rt->in_use = true;
    rt->active = true;       /* initialisation is watched for running away */

    ESP_LOGI(TAG, "real-time task declared: event %d, min interval %lu us",
             rv9_event_id(ev), (unsigned long)min_interval_us);
    return RV9_OK;
}

rv9_err_t rv9_rt_declare(uint32_t period_us)
{
    if (period_us == 0) return RV9_ERR_INVAL;

    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    rt_task_t *rt = claim_slot(self);
    if (rt == NULL) return RV9_ERR_NOMEM;

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

    /* Read before starting, so the schedule is if anything a microsecond
       early. Lateness measured against it then errs towards "late", which
       is the direction an instrument deciding faults must err in. */
    uint64_t t0 = rv9_time_us();

    if (esp_timer_start_periodic(rt->timer, period_us) != ESP_OK) {
        rt->in_use = false;
        return RV9_ERR_NOMEM;
    }

    rt->released_at_us = t0;
    rt->due_us         = t0 + period_us;
    rt->active         = true;   /* initialisation is watched for running away */

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
/*
 * Waiting for an event.
 *
 * Everything here that a periodic task does against a timer, this does
 * against the world: lateness is measured from the instant the interrupt
 * handler stamped, not from a deadline we set ourselves. That number --
 * pin to process -- is the one a reactive system lives or dies on, and it
 * is the only one that cannot be obtained from inside the task alone.
 */
static RV9_RT_CODE int rt_wait_event(rt_task_t *rt)
{
    event_impl_t *e = rt->event;
    uint64_t at, prev;
    uint32_t pending;

    for (;;) {
        if (xSemaphoreTake(e->sem, portMAX_DELAY) != pdTRUE) return -1;

        /* Drain the rest: they are signals about a world that has since
           changed again, and the handler is about to look at the world as
           it is now. */
        while (uxSemaphoreGetCount(e->sem) > 0) xSemaphoreTake(e->sem, 0);

        if (rt->stop) return RV9_RT_STOPPED;

        portENTER_CRITICAL(&s_event_guard);
        pending = e->pending;
        at      = e->at_us;
        prev    = e->last_us;
        if (pending > 0) {
            e->last_us = at;
            e->pending = 0;
        }
        portEXIT_CRITICAL(&s_event_guard);

        /* A wakeup with no event behind it is rv9_rt_stop's, left in the
           semaphore by a task that stopped before it came to wait. It is
           nothing to respond to, and measuring it would stamp a latency
           from an event that happened some time last week. */
        if (pending > 0) break;
    }

    uint64_t woken = rv9_time_us();

    /* Pin to process. */
    uint32_t latency = (woken > at) ? (uint32_t)(woken - at) : 0;
    if (latency > rt->max_jitter_us) rt->max_jitter_us = latency;

    /* An event's deadline runs from the event, so this activation starts
       already that far into it. */
    rt->late_us = latency;

    /* How fast is this source really going? Worth knowing whether or not a
       bound was declared -- a bound nobody measured is a guess. */
    if (prev != 0 && at > prev) {
        uint64_t gap = at - prev;
        uint32_t g = (gap > UINT32_MAX) ? UINT32_MAX : (uint32_t)gap;
        if (g < rt->min_interval_us) rt->min_interval_us = g;

        if (rt->period_us != 0 && g < rt->period_us) {
            rt->floods++;
            /* Once. A source that is flooding will flood thousands of
               times, and a log that scrolls is a log nobody reads. */
            if (!rt->flood_reported) {
                rt->flood_reported = true;
                ESP_LOGW(TAG, "event %d arrived %lu us apart, %lu declared",
                         rv9_event_id((rv9_event_t)e),
                         (unsigned long)g, (unsigned long)rt->period_us);
            }
        }
    }

    rt->released_at_us = woken;
    rt->activations++;

    /*
     * Signals that piled up while we were working. They are the aperiodic
     * form of a missed period: the system was handed more to respond to
     * than it responded to, and coalescing them is a decision, not an
     * accident, so it is reported.
     */
    int coalesced = (pending > 1) ? (int)(pending - 1) : 0;
    rt->overruns += (uint64_t)coalesced;

    rt->samples = 0;
    rt->active  = true;
    return coalesced;
}

RV9_RT_CODE int rv9_rt_wait(void)
{
    rt_task_t *rt = slot_for(xTaskGetCurrentTaskHandle());
    if (rt == NULL) return -1;

    /* Out of the activation before anything about it is rewritten. */
    rt->active = false;

    /* Charge this activation before sleeping, so execution time is the
       work itself and not the work plus the wait. Both release sources owe
       this, so it happens before they part company. */
    uint64_t now = rv9_time_us();
    uint32_t exec = (uint32_t)(now - rt->released_at_us);
    rt->last_exec_us = exec;
    if (exec > rt->max_exec_us) rt->max_exec_us = exec;

    /* Asked to stop while working. Between activations is the one place a
       task is known to hold nothing, and this is it. */
    if (rt->stop) return RV9_RT_STOPPED;

    /*
     * Did the activation that just finished meet its deadline?
     *
     * Checked here because this is when it is known: the work is done, and
     * nothing about it can change. Response is how late the activation
     * started plus how long it ran. Skipped for the first call, which ends
     * initialisation rather than an activation -- nothing released it.
     */
    if (rt->activations > 0) {
        uint64_t r = (uint64_t)exec + rt->late_us;
        uint32_t response = (r > UINT32_MAX) ? UINT32_MAX : (uint32_t)r;
        rt->last_response_us = response;
        if (response > rt->max_response_us) rt->max_response_us = response;

        if (rt->deadline_us != 0 && response > rt->deadline_us) {
            rt->deadline_misses++;
            /* The late activation is the last one. Waiting for the next
               release first would hand the actuators one more period of
               output from a loop already known to be wrong. */
            if (rt->fault_on_miss) return RV9_RT_DEADLINE;
        }
    }

    /*
     * The watchdog got here first. Coming to wait is the clean way to be
     * stopped for it, and the task has just done that -- but only after the
     * activation above is on the record. Returning before it left the
     * previous activation's response in the report of this one's fault:
     * "answered in 13 us", about a loop that had just spent five
     * milliseconds.
     */
    if (rt->flagged) return rt->flagged;

    if (rt->event != NULL) return rt_wait_event(rt);

    if (xSemaphoreTake(rt->release, portMAX_DELAY) != pdTRUE) return -1;

    /* Drop any releases still queued: they are periods that came and went
       while this task was elsewhere, and working through a backlog of stale
       deadlines is not what a control loop wants. */
    while (uxSemaphoreGetCount(rt->release) > 0) xSemaphoreTake(rt->release, 0);

    /* Woken to be told to stop, not to work. */
    if (rt->stop) return RV9_RT_STOPPED;

    uint64_t woken = rv9_time_us();

    /*
     * Count what was missed from the clock, not from the semaphore.
     *
     * The semaphore holds at most MISS_BACKLOG releases, so counting drains
     * stopped at 8 however long the stall was -- a 200 ms hole in a 1 kHz
     * loop reported seven missed periods instead of a hundred and ninety
     * nine. An instrument that saturates just where it matters is worse than
     * none: it says "slightly late" about a loop that stopped.
     *
     * And from the schedule, not from the last wakeup: see due_us.
     *
     * Lateness is the whole overshoot, not the remainder after whole
     * periods are taken out of it. Reporting the remainder was the same
     * mistake in a different place: a loop stalled for 200 ms and one
     * stalled for 200 us both came back "late by a little".
     */
    uint64_t over = (woken > rt->due_us) ? (woken - rt->due_us) : 0;
    uint32_t jitter = (over > UINT32_MAX) ? UINT32_MAX : (uint32_t)over;
    if (jitter > rt->max_jitter_us) rt->max_jitter_us = jitter;

    uint32_t missed = jitter / rt->period_us;

    /* The activation now starting serves the most recent release; the
       ones before it got no activation at all. */
    rt->late_us  = jitter - missed * rt->period_us;
    rt->due_us  += (uint64_t)(missed + 1) * rt->period_us;

    bool first = (rt->activations == 0);
    rt->released_at_us = woken;
    rt->activations++;
    rt->overruns += (uint64_t)missed;

    /*
     * A release that got no activation missed its deadline outright, and
     * that is known now rather than at the end of the next activation. The
     * first wakeup is forgiven for the same reason the first completion
     * is: periods consumed by initialisation were never promised.
     */
    if (missed > 0 && !first && rt->deadline_us != 0) {
        rt->deadline_misses += missed;
        if (rt->fault_on_miss) return RV9_RT_DEADLINE;
    }

    rt->samples = 0;
    rt->active  = true;
    return (int)missed;
}

rv9_err_t rv9_rt_deadline(uint32_t deadline_us, bool fault)
{
    rt_task_t *rt = slot_for(xTaskGetCurrentTaskHandle());
    if (rt == NULL) return RV9_ERR_INVAL;

    rt->deadline_us   = deadline_us;
    rt->fault_on_miss = fault && deadline_us != 0;
    return RV9_OK;
}

/*
 * The scheduler is held off rather than interrupts: what this races is the
 * task releasing its own slot and deleting the semaphore about to be given,
 * and that happens in task context. Giving with no wait is permitted while
 * the scheduler is suspended; the switch it earns happens on resume.
 */
rv9_err_t rv9_rt_stop(rv9_task_t task)
{
    if (task == NULL) return RV9_ERR_INVAL;

    rv9_err_t err = RV9_ERR_INVAL;

    vTaskSuspendAll();
    rt_task_t *rt = slot_for((TaskHandle_t)task);
    if (rt != NULL) {
        rt->stop = true;
        xSemaphoreGive(rt->event != NULL ? rt->event->sem : rt->release);
        err = RV9_OK;
    }
    xTaskResumeAll();

    return err;
}

static void release_slot(rt_task_t *rt)
{
    rt->active = false;

    /* Releases stop first, while the slot is still visibly ours. */
    bool borrowed = (rt->event != NULL);
    if (!borrowed) esp_timer_stop(rt->timer);

    /* Out of the table before anything it points at is freed, and with the
       scheduler held so rv9_rt_stop cannot be halfway through giving the
       semaphore about to be deleted. */
    vTaskSuspendAll();
    rt->in_use = false;
    rt->task   = NULL;
    rt->event  = NULL;
    xTaskResumeAll();

    /* An event-driven task borrowed its release source; it did not make it,
       and the device it belongs to is still there. Only let go of it. */
    if (!borrowed) {
        esp_timer_delete(rt->timer);
        vSemaphoreDelete(rt->release);
    }
}

void rv9_rt_release(void)
{
    rt_task_t *rt = slot_for(xTaskGetCurrentTaskHandle());
    if (rt != NULL) release_slot(rt);
}

void rv9_rt_release_task(rv9_task_t task)
{
    rt_task_t *rt = (task != NULL) ? slot_for((TaskHandle_t)task) : NULL;
    if (rt != NULL) release_slot(rt);
}

static void fill_stats(const rt_task_t *rt, rv9_rt_stats_t *out)
{
    out->period_us       = rt->period_us;
    out->activations     = rt->activations;
    out->overruns        = rt->overruns;
    out->max_jitter_us   = rt->max_jitter_us;
    out->max_exec_us     = rt->max_exec_us;
    out->last_exec_us    = rt->last_exec_us;
    out->event_driven    = (rt->event != NULL);
    out->floods          = rt->floods;
    /* Nothing seen yet reads as zero, not as four billion. */
    out->min_interval_us = (rt->min_interval_us == UINT32_MAX)
                           ? 0 : rt->min_interval_us;
    out->deadline_us      = rt->deadline_us;
    out->deadline_misses  = rt->deadline_misses;
    out->max_response_us  = rt->max_response_us;
    out->last_response_us = rt->last_response_us;
}

rv9_err_t rv9_rt_stats(rv9_rt_stats_t *out)
{
    rt_task_t *rt = slot_for(xTaskGetCurrentTaskHandle());
    if (rt == NULL || out == NULL) return RV9_ERR_INVAL;

    fill_stats(rt, out);
    return RV9_OK;
}

rv9_err_t rv9_rt_stats_for(rv9_task_t task, rv9_rt_stats_t *out)
{
    if (task == NULL || out == NULL) return RV9_ERR_INVAL;

    rt_task_t *rt = slot_for((TaskHandle_t)task);
    if (rt == NULL) return RV9_ERR_INVAL;   /* not a real-time task */

    fill_stats(rt, out);
    return RV9_OK;
}

int rv9_rt_slot_count(void) { return MAX_RT_TASKS; }

int rv9_rt_slots_used(void)
{
    int n = 0;
    for (int i = 0; i < MAX_RT_TASKS; i++) if (s_rt[i].in_use) n++;
    return n;
}

rv9_err_t rv9_rt_stats_by_index(int index, rv9_rt_stats_t *out, bool *valid)
{
    if (index < 0 || index >= MAX_RT_TASKS || out == NULL) {
        return RV9_ERR_INVAL;
    }
    rt_task_t *rt = &s_rt[index];
    if (valid) *valid = rt->in_use;
    if (!rt->in_use) return RV9_OK;

    fill_stats(rt, out);
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
