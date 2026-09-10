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
    uint64_t          deadline;
} burner_t;

static void burner_thread(void *arg)
{
    burner_t *b = (burner_t *)arg;

    while (rv9_time_ms() < b->deadline) {
        volatile uint32_t acc = 0;
        for (uint32_t i = 0; i < 1500; i++) acc += i;
        b->units++;
        rv9k_yield();      /* cooperative until step 2 brings the timer */
    }
}

static void *test_alloc(size_t n) { return rv9_alloc(n); }

bool rv9_kernel_selftest(void)
{
    s_passed = 0;
    s_failed = 0;
    s_order_counter = 0;

    ESP_LOGI(TAG, "native kernel tests");

    /* --- threads run at all, and alternate --- */
    rv9k_init(rv9_time_ms);

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
    rv9k_init(rv9_time_ms);

    static burner_t hi, lo;
    uint64_t deadline = rv9_time_ms() + 300;
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

    check(hi.units > 0 && lo.units > 0,
          "aging let the low-priority thread run at all");
    check(hi.units > lo.units * 2,
          "high priority still got much the larger share");

    if (s_failed == 0) ESP_LOGI(TAG, "%d passed, 0 failed", s_passed);
    else               ESP_LOGE(TAG, "%d passed, %d FAILED", s_passed, s_failed);

    return s_failed == 0;
}
