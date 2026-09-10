/*
 * The portable KAL contract, exercised against whichever implementation is
 * handed in.
 *
 * Only the portable part is here. Host-specific capabilities -- DMA-capable
 * memory, heap statistics, critical sections -- are not the kernel's
 * business and are checked separately against the host backend.
 */
#include "conformance.h"

#include "esp_log.h"

static const char *TAG = "conform";

static int s_passed;
static int s_failed;
static const char *s_backend;

static void check(bool ok, const char *what)
{
    if (ok) { s_passed++; ESP_LOGI(TAG, "  [%s] pass  %s", s_backend, what); }
    else    { s_failed++; ESP_LOGE(TAG, "  [%s] FAIL  %s", s_backend, what); }
}

/* ---- handshake between two threads ---- */

typedef struct {
    const kal_ops_t *ops;
    void            *done;
    volatile int     value;
} handshake_t;

static void worker(void *arg)
{
    handshake_t *h = (handshake_t *)arg;
    h->value = 0x5A;
    h->ops->sem_give(h->done);
    h->ops->task_exit();
}

int rv9_conformance_run(const kal_ops_t *ops)
{
    s_passed  = 0;
    s_failed  = 0;
    s_backend = ops->name;

    ESP_LOGI(TAG, "KAL contract against '%s'", ops->name);

    /* ---- time ---- */
    uint64_t t0 = ops->time_ms();
    ops->delay_ms(50);
    uint64_t t1 = ops->time_ms();

    check(t1 > t0, "time advances");
    check(t1 - t0 >= 40 && t1 - t0 <= 250, "delay_ms is roughly accurate");

    /* ---- tasks and semaphores ---- */
    static handshake_t h;
    h.ops = ops;
    h.value = 0;
    h.done = NULL;

    check(ops->sem_create(1, 0, &h.done) == 0, "sem_create");
    if (h.done != NULL) {
        check(ops->sem_take(h.done, 0) < 0,
              "sem_take on an empty semaphore times out");

        check(ops->task_create(worker, "conf-worker", 4096, &h, 8) == 0,
              "task_create");
        check(ops->sem_take(h.done, 2000) == 0, "worker signalled within 2s");
        check(h.value == 0x5A, "worker ran and wrote its value");
        check(ops->task_self() != NULL, "task_self returns a handle");

        ops->sem_destroy(h.done);
    }

    /* ---- mutexes ---- */
    void *m = NULL;
    check(ops->mutex_create(&m) == 0, "mutex_create");
    if (m) {
        check(ops->mutex_lock(m, 100) == 0, "mutex_lock");
        check(ops->mutex_unlock(m) == 0, "mutex_unlock");
        ops->mutex_destroy(m);
    }

    void *rm = NULL;
    check(ops->mutex_create_recursive(&rm) == 0, "recursive mutex_create");
    if (rm) {
        check(ops->mutex_lock(rm, 100) == 0 && ops->mutex_lock(rm, 100) == 0,
              "recursive mutex locks twice without deadlocking");
        ops->mutex_unlock(rm);
        ops->mutex_unlock(rm);
        ops->mutex_destroy(rm);
    }

    /* ---- queues ---- */
    void *q = NULL;
    check(ops->queue_create(4, sizeof(uint32_t), &q) == 0, "queue_create");
    if (q) {
        uint32_t out = 0;
        check(ops->queue_recv(q, &out, 0) < 0, "recv on an empty queue times out");

        uint32_t in = 0xDEADBEEF;
        check(ops->queue_send(q, &in, 100) == 0, "queue_send");
        check(ops->queue_count(q) == 1, "queue_count reflects one item");
        check(ops->queue_recv(q, &out, 100) == 0, "queue_recv");
        check(out == 0xDEADBEEF, "queue preserved the value");
        check(ops->queue_count(q) == 0, "queue empty after recv");

        ops->queue_destroy(q);
    }

    /* ---- memory ---- */
    void *p = ops->alloc(1024);
    check(p != NULL, "alloc");
    ops->free(p);

    uint8_t *z = (uint8_t *)ops->calloc(64, 1);
    bool zeroed = (z != NULL);
    for (int i = 0; zeroed && i < 64; i++) if (z[i] != 0) zeroed = false;
    check(zeroed, "calloc returns zeroed memory");
    ops->free(z);

    /* ---- yielding ---- */
    ops->yield();
    check(true, "yield returns");

    if (s_failed == 0) ESP_LOGI(TAG, "[%s] %d passed, 0 failed", ops->name, s_passed);
    else               ESP_LOGE(TAG, "[%s] %d passed, %d FAILED", ops->name, s_passed, s_failed);

    return s_failed;
}
