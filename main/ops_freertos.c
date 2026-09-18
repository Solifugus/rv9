/*
 * The KAL contract, satisfied by the host-backed KAL. This is the
 * reference: it is the implementation five phases of RV-9 have been
 * running on, so if a test disagrees with it the test is wrong.
 */
#include "conformance.h"

#include "rv9/kal.h"

_Static_assert(CONF_WAIT_FOREVER == RV9_WAIT_FOREVER,
               "the suite's 'forever' must be the one this backend is given");

static uint64_t f_time_ms(void)          { return rv9_time_ms(); }
static void     f_delay_ms(uint32_t ms)  { rv9_task_delay_ms(ms); }
static void     f_yield(void)            { rv9_task_yield(); }

static int f_task_create(void (*fn)(void *), const char *name,
                         size_t stack, void *arg, int prio)
{
    return rv9_task_create((rv9_task_fn)fn, name, stack, arg, prio, NULL)
           == RV9_OK ? 0 : -1;
}

static void  f_task_exit(void) { rv9_task_delete(NULL); }
static void *f_task_self(void) { return (void *)rv9_task_self(); }

static int f_sem_create(uint32_t max, uint32_t initial, void **out)
{
    rv9_sem_t s = NULL;
    if (rv9_sem_create(max, initial, &s) != RV9_OK) return -1;
    *out = (void *)s;
    return 0;
}
static void f_sem_destroy(void *s) { rv9_sem_destroy((rv9_sem_t)s); }
static int  f_sem_take(void *s, uint32_t ms)
{
    return rv9_sem_take((rv9_sem_t)s, ms) == RV9_OK ? 0 : -1;
}
static int f_sem_give(void *s)
{
    return rv9_sem_give((rv9_sem_t)s) == RV9_OK ? 0 : -1;
}

static int f_mutex_create(void **out)
{
    rv9_mutex_t m = NULL;
    if (rv9_mutex_create(&m) != RV9_OK) return -1;
    *out = (void *)m;
    return 0;
}
static int f_mutex_create_recursive(void **out)
{
    rv9_mutex_t m = NULL;
    if (rv9_mutex_create_recursive(&m) != RV9_OK) return -1;
    *out = (void *)m;
    return 0;
}
static void f_mutex_destroy(void *m) { rv9_mutex_destroy((rv9_mutex_t)m); }
static int  f_mutex_lock(void *m, uint32_t ms)
{
    return rv9_mutex_lock((rv9_mutex_t)m, ms) == RV9_OK ? 0 : -1;
}
static int f_mutex_unlock(void *m)
{
    return rv9_mutex_unlock((rv9_mutex_t)m) == RV9_OK ? 0 : -1;
}

static int f_queue_create(uint32_t len, size_t item, void **out)
{
    rv9_queue_t q = NULL;
    if (rv9_queue_create(len, item, &q) != RV9_OK) return -1;
    *out = (void *)q;
    return 0;
}
static void f_queue_destroy(void *q) { rv9_queue_destroy((rv9_queue_t)q); }
static int  f_queue_send(void *q, const void *item, uint32_t ms)
{
    return rv9_queue_send((rv9_queue_t)q, item, ms) == RV9_OK ? 0 : -1;
}
static int f_queue_recv(void *q, void *item, uint32_t ms)
{
    return rv9_queue_recv((rv9_queue_t)q, item, ms) == RV9_OK ? 0 : -1;
}
static uint32_t f_queue_count(void *q) { return rv9_queue_count((rv9_queue_t)q); }

static void *f_alloc(size_t n)               { return rv9_alloc(n); }
static void *f_calloc(size_t c, size_t s)    { return rv9_calloc(c, s); }
static void  f_free(void *p)                 { rv9_free(p); }

static const kal_ops_t s_ops = {
    .name = "kal",
    .time_ms = f_time_ms, .delay_ms = f_delay_ms, .yield = f_yield,
    .task_create = f_task_create, .task_exit = f_task_exit,
    .task_self = f_task_self,
    .sem_create = f_sem_create, .sem_destroy = f_sem_destroy,
    .sem_take = f_sem_take, .sem_give = f_sem_give,
    .mutex_create = f_mutex_create,
    .mutex_create_recursive = f_mutex_create_recursive,
    .mutex_destroy = f_mutex_destroy,
    .mutex_lock = f_mutex_lock, .mutex_unlock = f_mutex_unlock,
    .queue_create = f_queue_create, .queue_destroy = f_queue_destroy,
    .queue_send = f_queue_send, .queue_recv = f_queue_recv,
    .queue_count = f_queue_count,
    .alloc = f_alloc, .calloc = f_calloc, .free = f_free,
};

const kal_ops_t *rv9_ops_freertos(void) { return &s_ops; }
