/*
 * The KAL contract, satisfied by RV-9's own kernel.
 *
 * Everything here must be called from inside an RV-9 thread: the blocking
 * calls reschedule, and rescheduling only means something to a thread the
 * kernel is running.
 *
 * The kernel has no heap of its own, so allocation still comes from the
 * host. That is honest rather than hidden -- a kernel that owns the
 * machine will need its own allocator, and it does not have one yet.
 */
#include "conformance.h"

#include "rv9/kernel.h"
#include "rv9/kal.h"

static uint64_t n_time_ms(void)
{
    return (uint64_t)rv9k_ticks() * 1000u / RV9K_TICK_HZ;
}

static void n_delay_ms(uint32_t ms) { rv9k_sleep_ms(ms); }
static void n_yield(void)           { rv9k_yield(); }

static void *n_alloc(size_t n) { return rv9_alloc(n); }

static int n_task_create(void (*fn)(void *), const char *name,
                         size_t stack, void *arg, int prio)
{
    return rv9k_thread_create((rv9k_entry_fn)fn, arg, name, stack, prio,
                              n_alloc) != NULL ? 0 : -1;
}

static void  n_task_exit(void) { rv9k_exit(); }
static void *n_task_self(void) { return (void *)rv9k_self(); }

static int n_sem_create(uint32_t max, uint32_t initial, void **out)
{
    rv9k_sem_t *s = (rv9k_sem_t *)rv9_alloc(sizeof(*s));
    if (s == NULL) return -1;
    rv9k_sem_init(s, (int32_t)initial, (int32_t)max);
    *out = s;
    return 0;
}
static void n_sem_destroy(void *s) { rv9_free(s); }
static int  n_sem_take(void *s, uint32_t ms)
{
    return rv9k_sem_take((rv9k_sem_t *)s, ms) ? 0 : -1;
}
static int n_sem_give(void *s)
{
    rv9k_sem_give((rv9k_sem_t *)s);
    return 0;
}

static int n_mutex_create(void **out)
{
    rv9k_mutex_t *m = (rv9k_mutex_t *)rv9_alloc(sizeof(*m));
    if (m == NULL) return -1;
    rv9k_mutex_init(m);
    *out = m;
    return 0;
}
/* The kernel's mutex counts its own nesting, so one kind serves both. */
static int  n_mutex_create_recursive(void **out) { return n_mutex_create(out); }
static void n_mutex_destroy(void *m) { rv9_free(m); }
static int  n_mutex_lock(void *m, uint32_t ms)
{
    return rv9k_mutex_lock((rv9k_mutex_t *)m, ms) ? 0 : -1;
}
static int n_mutex_unlock(void *m)
{
    rv9k_mutex_unlock((rv9k_mutex_t *)m);
    return 0;
}

/* The kernel's queue does not allocate; storage is handed to it. */
typedef struct {
    rv9k_queue_t q;
    uint8_t      storage[];
} native_queue_t;

static int n_queue_create(uint32_t len, size_t item, void **out)
{
    native_queue_t *nq =
        (native_queue_t *)rv9_alloc(sizeof(*nq) + (size_t)len * item);
    if (nq == NULL) return -1;

    rv9k_queue_init(&nq->q, nq->storage, len, (uint32_t)item);
    *out = nq;
    return 0;
}
static void n_queue_destroy(void *q) { rv9_free(q); }
static int  n_queue_send(void *q, const void *item, uint32_t ms)
{
    return rv9k_queue_send(&((native_queue_t *)q)->q, item, ms) ? 0 : -1;
}
static int n_queue_recv(void *q, void *item, uint32_t ms)
{
    return rv9k_queue_recv(&((native_queue_t *)q)->q, item, ms) ? 0 : -1;
}
static uint32_t n_queue_count(void *q)
{
    return rv9k_queue_count(&((native_queue_t *)q)->q);
}

static void *n_calloc(size_t c, size_t s) { return rv9_calloc(c, s); }
static void  n_free(void *p)              { rv9_free(p); }

static const kal_ops_t s_ops = {
    .name = "rv9-kernel",
    .time_ms = n_time_ms, .delay_ms = n_delay_ms, .yield = n_yield,
    .task_create = n_task_create, .task_exit = n_task_exit,
    .task_self = n_task_self,
    .sem_create = n_sem_create, .sem_destroy = n_sem_destroy,
    .sem_take = n_sem_take, .sem_give = n_sem_give,
    .mutex_create = n_mutex_create,
    .mutex_create_recursive = n_mutex_create_recursive,
    .mutex_destroy = n_mutex_destroy,
    .mutex_lock = n_mutex_lock, .mutex_unlock = n_mutex_unlock,
    .queue_create = n_queue_create, .queue_destroy = n_queue_destroy,
    .queue_send = n_queue_send, .queue_recv = n_queue_recv,
    .queue_count = n_queue_count,
    .alloc = n_alloc, .calloc = n_calloc, .free = n_free,
};

const kal_ops_t *rv9_ops_native(void) { return &s_ops; }
