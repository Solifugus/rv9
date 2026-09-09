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
