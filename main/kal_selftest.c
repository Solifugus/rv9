/*
 * KAL conformance tests.
 *
 * These run against whichever backend is built in. Today that is FreeRTOS;
 * in phase 7 it is the native RV-9 kernel, and these same tests are how we
 * know the replacement behaves. Keep them backend-agnostic -- no assumptions
 * beyond what docs/design.md §4 promises.
 */
#include "kal_selftest.h"

#include "rv9/kal.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "kal-test";

static int s_passed;
static int s_failed;

static void check(bool ok, const char *what)
{
    if (ok) {
        s_passed++;
        ESP_LOGI(TAG, "  pass  %s", what);
    } else {
        s_failed++;
        ESP_LOGE(TAG, "  FAIL  %s", what);
    }
}

/* ---- a semaphore given from a real interrupt ---- */

/*
 * The one primitive nothing used, so nothing had ever noticed that the
 * native kernel did not implement it -- until a driver called it, ignored
 * the refusal, and spent two hundred milliseconds a transfer waiting for a
 * signal that could never arrive. It has a test now.
 *
 * The trigger is a timer on ISR dispatch because it has to be a real
 * interrupt: the point is not that the code runs, but that it runs while
 * the kernel may be in the middle of its own wait queues.
 */
static rv9_sem_t s_isr_sem;
static volatile uint32_t s_isr_refused;

static void RV9_RT_CODE isr_giver(void *arg)
{
    (void)arg;

    bool woken = false;
    /* Counted rather than cast away: a (void) does not silence
       warn_unused_result in GCC, which is the point of using it. */
    if (rv9_sem_give_from_isr(s_isr_sem, &woken) != RV9_OK) s_isr_refused++;
}

static void test_sem_from_isr(void)
{
    check(rv9_sem_create(4, 0, &s_isr_sem) == RV9_OK, "sem for the isr test");

    bool woken = false;
    check(rv9_sem_give_from_isr(s_isr_sem, &woken) == RV9_OK,
          "give_from_isr is implemented");
    check(rv9_sem_take(s_isr_sem, 0) == RV9_OK, "what it gave can be taken");

    const esp_timer_create_args_t args = {
        .callback        = isr_giver,
        .dispatch_method = ESP_TIMER_ISR,
        .name            = "semisr",
    };
    esp_timer_handle_t t = NULL;

    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 20000);          /* 20 ms */

        uint64_t t0 = rv9_time_us();
        rv9_err_t got = rv9_sem_take(s_isr_sem, 1000);
        uint32_t waited = (uint32_t)((rv9_time_us() - t0) / 1000);

        check(got == RV9_OK, "a sleeping thread is woken from an interrupt");
        check(waited < 200, "woken promptly rather than on the timeout");
        ESP_LOGI(TAG, "  woken %u ms after the interrupt was armed", waited);

        check(s_isr_refused == 0, "the interrupt's give was not refused");
        esp_timer_delete(t);
    } else {
        ESP_LOGW(TAG, "  no ISR-dispatch timer; wake path untested");
    }

    rv9_sem_destroy(s_isr_sem);
}

/* ---- and a queue filled from an interrupt ---- */

static rv9_queue_t s_isr_q;
static volatile uint32_t s_isr_q_refused;

static void RV9_RT_CODE isr_sender(void *arg)
{
    (void)arg;

    uint32_t v = 0xC0FFEE;
    bool woken = false;
    if (rv9_queue_send_from_isr(s_isr_q, &v, &woken) != RV9_OK) {
        s_isr_q_refused++;
    }
}

static void test_queue_from_isr(void)
{
    check(rv9_queue_create(4, sizeof(uint32_t), &s_isr_q) == RV9_OK,
          "queue for the isr test");

    const esp_timer_create_args_t args = {
        .callback        = isr_sender,
        .dispatch_method = ESP_TIMER_ISR,
        .name            = "qisr",
    };
    esp_timer_handle_t t = NULL;

    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 20000);

        uint32_t got = 0;
        check(rv9_queue_recv(s_isr_q, &got, 1000) == RV9_OK,
              "a receiver is woken by an interrupt");
        check(got == 0xC0FFEE, "and the item survived the trip");
        check(s_isr_q_refused == 0, "the interrupt's send was not refused");

        esp_timer_delete(t);
    }

    rv9_queue_destroy(s_isr_q);
}

/* ---- task + semaphore handshake ---- */

typedef struct {
    rv9_sem_t done;
    int       value;
} handshake_t;

static void worker_task(void *arg)
{
    handshake_t *h = (handshake_t *)arg;
    h->value = 0x5A;
    rv9_sem_give(h->done);
    rv9_task_delete(NULL);   /* deletes self, does not return */
}

static void test_time(void)
{
    uint64_t t0 = rv9_time_us();
    rv9_task_delay_ms(50);
    uint64_t t1 = rv9_time_us();

    check(t1 > t0, "time advances");

    uint64_t elapsed_ms = (t1 - t0) / 1000;
    /* Generous bounds: we are testing sanity, not jitter. */
    check(elapsed_ms >= 40 && elapsed_ms <= 200, "delay_ms is roughly accurate");
}

static void test_tasks_and_sems(void)
{
    handshake_t h = { .done = NULL, .value = 0 };

    check(rv9_sem_create(1, 0, &h.done) == RV9_OK, "sem_create");
    if (h.done == NULL) return;

    check(rv9_sem_take(h.done, RV9_NO_WAIT) == RV9_ERR_TIMEOUT,
          "sem_take on empty semaphore times out");

    rv9_task_t task = NULL;
    check(rv9_task_create(worker_task, "rv9-worker", 2048, &h,
                          RV9_PRIO_NORMAL, &task) == RV9_OK, "task_create");

    check(rv9_sem_take(h.done, 1000) == RV9_OK, "worker signalled within 1s");
    check(h.value == 0x5A, "worker ran and wrote its value");

    check(rv9_task_self() != NULL, "task_self returns a handle");

    rv9_sem_destroy(h.done);
}

/* ---- floating-point state belongs to the thread ---- */

#if defined(__riscv_flen)
/*
 * Two threads, each with its own values in fs0-fs11 and its own rounding
 * mode, handing the CPU back and forth. fs0-fs11 are callee-saved under
 * the hard-float ABI and the rounding mode is per-thread, so a yield must
 * hand each thread back exactly what it had. The compiler may keep a float
 * in any of those registers across any call, a switch included.
 *
 * Set and read in assembly, because a test written in float C would only
 * be as good as the compiler's choice of registers.
 */
typedef struct fp_case {
    uint32_t  seed;
    uint32_t  frm;
    int       rounds;
    volatile int bad;          /* -1 until it finishes */
    rv9_sem_t turn;            /* given when it is this thread's go */
    rv9_sem_t done;
    struct fp_case *other;
} fp_case_t;

/*
 * Inlined, and with no clobber list, on purpose. Naming fs0-fs11 as
 * clobbered makes the compiler save them on entry and put them back on
 * return -- which is the ABI working -- so a separate function that set
 * them undid its own work before the thread ever switched, and the first
 * version of this test failed every register with or without the fix.
 * fp_thread is entered from the trampoline and never returns, so nothing
 * above it expects its FP registers back.
 */
static inline __attribute__((always_inline))
void fp_set(uint32_t seed, uint32_t frm)
{
    uint32_t v[12];
    for (int i = 0; i < 12; i++) v[i] = seed + (uint32_t)i * 0x01010101u;

    __asm__ volatile (
        "flw fs0,  0(%0)\n flw fs1,  4(%0)\n flw fs2,  8(%0)\n"
        "flw fs3, 12(%0)\n flw fs4, 16(%0)\n flw fs5, 20(%0)\n"
        "flw fs6, 24(%0)\n flw fs7, 28(%0)\n flw fs8, 32(%0)\n"
        "flw fs9, 36(%0)\n flw fs10,40(%0)\n flw fs11,44(%0)\n"
        "fsrm %1\n"
        :: "r"(v), "r"(frm)
        : "memory");
}

/* How many of the thirteen no longer hold what fp_set put there. */
static inline __attribute__((always_inline))
int fp_wrong(uint32_t seed, uint32_t frm)
{
    uint32_t v[12], rm;

    __asm__ volatile (
        "fsw fs0,  0(%1)\n fsw fs1,  4(%1)\n fsw fs2,  8(%1)\n"
        "fsw fs3, 12(%1)\n fsw fs4, 16(%1)\n fsw fs5, 20(%1)\n"
        "fsw fs6, 24(%1)\n fsw fs7, 28(%1)\n fsw fs8, 32(%1)\n"
        "fsw fs9, 36(%1)\n fsw fs10,40(%1)\n fsw fs11,44(%1)\n"
        "frrm %0\n"
        : "=r"(rm) : "r"(v) : "memory");

    int wrong = (rm != frm);
    for (int i = 0; i < 12; i++)
        if (v[i] != seed + (uint32_t)i * 0x01010101u) wrong++;
    return wrong;
}

static void fp_thread(void *arg)
{
    fp_case_t *c = (fp_case_t *)arg;
    int bad = 0;

    /* Strict turns, so every switch goes from one to the other. A plain
       yield goes back to the scheduler, which may pick the same thread
       again -- the first version of this test did, and so never saw the
       other thread's rounding mode. */
    fp_set(c->seed, c->frm);
    for (int i = 0; i < c->rounds; i++) {
        rv9_sem_give(c->other->turn);
        if (rv9_sem_take(c->turn, 1000) != RV9_OK) { bad = -2; break; }
        bad += fp_wrong(c->seed, c->frm);
    }
    rv9_sem_give(c->other->turn);       /* do not leave it waiting */
    c->bad = bad;
    rv9_sem_give(c->done);
    rv9_task_delete(NULL);
}

static void test_fp_state(void)
{
    /* Rounding modes 1 (toward zero) and 2 (down): neither is the
       default, and they differ from each other. */
    static fp_case_t a, b;
    a = (fp_case_t){ .seed = 0x3f800000u, .frm = 1, .rounds = 50, .bad = -1 };
    b = (fp_case_t){ .seed = 0xc0400000u, .frm = 2, .rounds = 50, .bad = -1 };

    a.other = &b;
    b.other = &a;
    if (rv9_sem_create(1, 0, &a.done) != RV9_OK ||
        rv9_sem_create(1, 0, &b.done) != RV9_OK ||
        rv9_sem_create(1, 0, &a.turn) != RV9_OK ||
        rv9_sem_create(1, 0, &b.turn) != RV9_OK) {
        check(false, "semaphores for the FP test");
        return;
    }

    bool started =
        rv9_task_create(fp_thread, "fp-a", 2048, &a, RV9_PRIO_NORMAL, NULL) == RV9_OK &&
        rv9_task_create(fp_thread, "fp-b", 2048, &b, RV9_PRIO_NORMAL, NULL) == RV9_OK;
    check(started, "two threads with their own FP state");

    if (started) {
        bool finished = rv9_sem_take(a.done, 2000) == RV9_OK &&
                        rv9_sem_take(b.done, 2000) == RV9_OK;
        check(finished, "both handed the CPU back and forth to the end");
        check(finished && a.bad >= 0 && b.bad >= 0,
              "they took strict turns");
        check(finished && a.bad == 0 && b.bad == 0,
              "fs0-fs11 and the rounding mode survive every switch");
        if (finished && (a.bad || b.bad))
            ESP_LOGE(TAG, "  FP registers wrong after a switch: %d and %d",
                     a.bad, b.bad);
    }

    rv9_sem_destroy(a.done);
    rv9_sem_destroy(b.done);
    rv9_sem_destroy(a.turn);
    rv9_sem_destroy(b.turn);
}
#endif

static void test_mutexes(void)
{
    rv9_mutex_t m = NULL;
    check(rv9_mutex_create(&m) == RV9_OK, "mutex_create");
    if (m == NULL) return;

    check(rv9_mutex_lock(m, 100) == RV9_OK, "mutex_lock");
    check(rv9_mutex_unlock(m) == RV9_OK, "mutex_unlock");
    rv9_mutex_destroy(m);

    rv9_mutex_t rm = NULL;
    check(rv9_mutex_create_recursive(&rm) == RV9_OK, "recursive mutex_create");
    rv9_mutex_destroy(rm);
}

static void test_queues(void)
{
    rv9_queue_t q = NULL;
    check(rv9_queue_create(4, sizeof(uint32_t), &q) == RV9_OK, "queue_create");
    if (q == NULL) return;

    uint32_t out = 0;
    check(rv9_queue_recv(q, &out, RV9_NO_WAIT) == RV9_ERR_TIMEOUT,
          "recv on empty queue times out");

    uint32_t in = 0xDEADBEEF;
    check(rv9_queue_send(q, &in, 100) == RV9_OK, "queue_send");
    check(rv9_queue_count(q) == 1, "queue_count reflects one item");
    check(rv9_queue_recv(q, &out, 100) == RV9_OK, "queue_recv");
    check(out == 0xDEADBEEF, "queue preserved the value");
    check(rv9_queue_count(q) == 0, "queue empty after recv");

    rv9_queue_destroy(q);
}

static void test_memory(void)
{
    void *p = rv9_alloc(1024);
    check(p != NULL, "alloc");
    rv9_free(p);

    uint8_t *z = (uint8_t *)rv9_calloc(64, 1);
    bool zeroed = (z != NULL);
    for (int i = 0; zeroed && i < 64; i++) {
        if (z[i] != 0) zeroed = false;
    }
    check(zeroed, "calloc returns zeroed memory");
    rv9_free(z);

    void *d = rv9_alloc_dma(512);
    check(d != NULL, "alloc_dma");
    rv9_free(d);

    check(rv9_heap_free() > 0, "heap_free reports something");
    check(rv9_heap_low_water() <= rv9_heap_free(),
          "low water mark is not above current free");

    /*
     * The floor.
     *
     * Tested by raising it to swallow the whole heap rather than by trying
     * to exhaust memory, which would be a test that damages the machine it
     * runs on. What matters is that the refusal is a NULL and not an abort,
     * and that it is a refusal rather than a failure -- the memory is still
     * there afterwards.
     */
    size_t saved = rv9_heap_floor();

    rv9_heap_floor_set(rv9_heap_free() + 1);
    uint32_t refused_before = rv9_heap_refusals();
    check(rv9_heap_available() == 0, "no memory is available below the floor");

    void *nope = rv9_alloc(64);
    check(nope == NULL, "an allocation into the reserve is refused");
    check(rv9_heap_refusals() == refused_before + 1, "and is counted");
    rv9_free(nope);

    check(rv9_calloc(64, 1) == NULL, "calloc is refused too");
    check(rv9_alloc_dma(64) == NULL, "and DMA memory, which is the same RAM");

    /* The reserve is spendable on purpose, for reporting the refusal. */
    void *yes = rv9_alloc_critical(64);
    check(yes != NULL, "the reserve can still be spent deliberately");
    rv9_free(yes);

    rv9_heap_floor_set(saved);
    void *back = rv9_alloc(1024);
    check(back != NULL, "lowering the floor gives the memory back");
    rv9_free(back);
    check(rv9_heap_floor() == saved, "the floor is where it was");
}

static void test_critical(void)
{
    /* Mostly checking this does not deadlock or crash. */
    rv9_critical_enter();
    volatile int x = 1;
    x++;
    rv9_critical_exit();
    check(x == 2, "critical section entered and exited");
}

static void test_error_strings(void)
{
    check(rv9_strerror(RV9_OK) != NULL, "strerror(RV9_OK)");
    check(rv9_strerror(RV9_ERR_TIMEOUT) != NULL, "strerror(RV9_ERR_TIMEOUT)");
}

bool rv9_kal_selftest(void)
{
    s_passed = 0;
    s_failed = 0;

    ESP_LOGI(TAG, "KAL conformance tests");

    test_time();
    test_tasks_and_sems();
    test_sem_from_isr();
    test_queue_from_isr();
#if defined(__riscv_flen)
    test_fp_state();
#endif
    test_mutexes();
    test_queues();
    test_memory();
    test_critical();
    test_error_strings();

    if (s_failed == 0) {
        ESP_LOGI(TAG, "%d passed, 0 failed", s_passed);
    } else {
        ESP_LOGE(TAG, "%d passed, %d FAILED", s_passed, s_failed);
    }
    return s_failed == 0;
}
