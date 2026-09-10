/*
 * Native kernel tests.
 *
 * The context switch either works or the board reboots, so these are less
 * about assertions and more about proving that threads genuinely alternate
 * and that the scheduling policy is the one we intended.
 *
 * Note what is being compared: the same aging behaviour phase 2 measured
 * on FreeRTOS, now produced by RV-9's own scheduler.
 */
#include "kernel_test.h"

#include "rv9/kernel.h"
#include "rv9/kal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

static const char *TAG = "kern-test";

static int s_passed;
static int s_failed;

static void check(bool ok, const char *what)
{
    if (ok) { s_passed++; ESP_LOGI(TAG, "  pass  %s", what); }
    else    { s_failed++; ESP_LOGE(TAG, "  FAIL  %s", what); }
}

/* ---- a thread that counts and yields ---- */

typedef struct {
    volatile uint32_t ticks;
    uint32_t          limit;
    volatile bool     done;
    volatile uint32_t order;      /* filled from a shared counter */
} counter_t;

static volatile uint32_t s_order_counter;

static void counter_thread(void *arg)
{
    counter_t *c = (counter_t *)arg;

    while (c->ticks < c->limit) {
        c->ticks++;
        if (c->order == 0) c->order = ++s_order_counter;
        rv9k_yield();
    }
    c->done = true;
}

/* ---- a thread that burns time without yielding voluntarily ---- */

/*
 * Both burners stop at the same wall-clock moment, set before either runs.
 *
 * Letting each thread start its own clock when it first executes measures
 * nothing: a starved thread simply begins late and then runs unimpeded to
 * the same total. Phase 2 taught this exact lesson on FreeRTOS and the
 * first version of this test repeated it faithfully.
 */
typedef struct {
    volatile uint32_t units;
    uint32_t          deadline;
} burner_t;

static void burner_thread(void *arg)
{
    burner_t *b = (burner_t *)arg;

    while (!RV9K_TICK_REACHED(rv9k_ticks(), b->deadline)) {
        volatile uint32_t acc = 0;
        for (uint32_t i = 0; i < 1500; i++) acc += i;
        b->units++;

        /* Not a yield. This switches only when the tick says a switch is
           due, so the scheduler's cadence comes from the clock rather than
           from how often a thread happens to be polite. */
        rv9k_preempt_point();
    }
}

static void *test_alloc(size_t n) { return rv9_alloc(n); }

/* ---- the kernel's tick ---- */

static esp_timer_handle_t   s_tick_timer;
static volatile uint32_t   *s_tick_ref;

/*
 * IRAM_ATTR, and it calls nothing.
 *
 * An interrupt handler here may run while the flash cache is disabled --
 * the WiFi driver writes NVS, and anything in flash is unreachable while
 * it does. A handler that called into the kernel faulted with a cache
 * error the moment the radio was used. Incrementing the counter directly
 * keeps every instruction in RAM.
 */
static void IRAM_ATTR tick_isr(void *arg)
{
    (void)arg;
    if (s_tick_ref) (*s_tick_ref)++;
}

/*
 * Dispatched from the interrupt, not from a task. The host scheduler is
 * suspended while RV-9's scheduler drives, so a task-dispatched callback
 * would simply never run -- the kernel's clock would stop precisely when
 * it was needed.
 */
static bool tick_start(void)
{
    if (s_tick_timer != NULL) return true;

    const esp_timer_create_args_t args = {
        .callback        = tick_isr,
        .dispatch_method = ESP_TIMER_ISR,
        .name            = "rv9k-tick",
    };

    s_tick_ref = rv9k_tick_ref();

    if (esp_timer_create(&args, &s_tick_timer) != ESP_OK) return false;
    return esp_timer_start_periodic(s_tick_timer,
                                    1000000 / RV9K_TICK_HZ) == ESP_OK;
}

bool rv9_kernel_selftest(void)
{
    s_passed = 0;
    s_failed = 0;
    s_order_counter = 0;

    ESP_LOGI(TAG, "native kernel tests");

    check(tick_start(), "kernel tick running from a timer interrupt");

    uint32_t t0 = rv9k_ticks();
    rv9_task_delay_ms(50);
    check((uint32_t)(rv9k_ticks() - t0) >= 40, "the tick advances on its own");
    ESP_LOGI(TAG, "  %lu ticks in 50 ms",
             (unsigned long)(rv9k_ticks() - t0));

    /* --- threads run at all, and alternate --- */
    rv9k_init();

    static counter_t a, b;
    a = (counter_t){ .limit = 50 };
    b = (counter_t){ .limit = 50 };

    rv9k_thread_t *ta = rv9k_thread_create(counter_thread, &a, "count-a",
                                           4096, 8, test_alloc);
    rv9k_thread_t *tb = rv9k_thread_create(counter_thread, &b, "count-b",
                                           4096, 8, test_alloc);

    check(ta != NULL && tb != NULL, "thread_create");
    if (ta == NULL || tb == NULL) return false;

    /*
     * Hold the host scheduler still while ours is driving.
     *
     * RV-9's threads run on heap stacks the host kernel has never heard
     * of. If it preempts us while the stack pointer is one of those, it
     * saves and restores a context it cannot account for. Interrupts still
     * run -- this only stops task switching -- so no blocking call may be
     * made in here, which is why nothing logs from inside a thread.
     */
    uint64_t before = rv9k_switch_count();
    rv9_sched_lock();
    rv9k_run();
    rv9_sched_unlock();

    check(a.done && b.done, "both threads ran to completion");
    check(a.ticks == 50 && b.ticks == 50, "both counted to their limit");
    check(rv9k_switch_count() - before > 50, "context switches actually happened");
    ESP_LOGI(TAG, "  %llu switches", (unsigned long long)(rv9k_switch_count() - before));

    /* Equal priority means both start early rather than one finishing
       first: proof the run queue round-robins instead of latching. */
    check(a.order <= 2 && b.order <= 2, "equal priorities interleaved");

    /* --- priority is respected, and aging prevents starvation --- */
    rv9k_init();

    static burner_t hi, lo;
    uint32_t deadline = rv9k_ticks() + RV9K_MS_TO_TICKS(300);
    hi = (burner_t){ .deadline = deadline };
    lo = (burner_t){ .deadline = deadline };

    rv9k_thread_t *th = rv9k_thread_create(burner_thread, &hi, "hi",
                                           4096, 12, test_alloc);
    rv9k_thread_t *tl = rv9k_thread_create(burner_thread, &lo, "lo",
                                           4096, 4, test_alloc);
    check(th != NULL && tl != NULL, "priority threads created");

    rv9_sched_lock();
    rv9k_run();
    rv9_sched_unlock();

    ESP_LOGI(TAG, "  high priority did %lu units, low did %lu",
             (unsigned long)hi.units, (unsigned long)lo.units);
    ESP_LOGI(TAG, "  cpu charged: high %lu ticks, low %lu ticks",
             (unsigned long)th->ran_ticks, (unsigned long)tl->ran_ticks);

    /* Switching should now follow the clock, not the loop count. Roughly
       one switch per tick or two, over 300 ms -- not thousands. */
    uint64_t sw = rv9k_switch_count();
    ESP_LOGI(TAG, "  %llu switches over 300 ms", (unsigned long long)sw);
    check(sw > 10 && sw < 2000, "switch rate follows the tick, not the loop");

    check(hi.units > 0 && lo.units > 0,
          "aging let the low-priority thread run at all");
    check(hi.units > lo.units * 2,
          "high priority still got much the larger share");

    if (s_failed == 0) ESP_LOGI(TAG, "%d passed, 0 failed", s_passed);
    else               ESP_LOGE(TAG, "%d passed, %d FAILED", s_passed, s_failed);

    return s_failed == 0;
}
