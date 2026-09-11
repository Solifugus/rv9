/*
 * RV-9 scheduler.
 *
 * Priority with aging, implemented rather than delegated. The policy is the
 * one phase 2 proved on FreeRTOS: effective priority is base plus age, the
 * thread that runs has its age reset, and everyone else climbs. What
 * changes here is that the scheduler acts on that directly instead of
 * nudging another kernel's priorities from above.
 *
 * See rv9/kernel.h for where this runs today and where it is going.
 */
#include "rv9/kernel.h"

/* From switch.S */
extern void rv9_ctx_switch(uint32_t **save_sp, uint32_t *load_sp);
extern void rv9_thread_trampoline(void);

/* The trampoline jumps here if a thread returns from its entry point. */
void rv9_thread_exited(void);

static void reschedule(void);

static rv9k_thread_t  s_threads[RV9K_MAX_THREADS];
static rv9k_thread_t *s_current;
static uint32_t      *s_host_sp;          /* whoever called rv9k_run */
static volatile uint32_t s_ticks;         /* written only by the tick ISR */
static uint32_t       s_last_age_tick;
static uint64_t       s_switches;
static uint64_t       s_blocks;
static bool           s_running;
static uint32_t       s_sched_lock;
static void         (*s_idle_hook)(void);
static void        *(*s_alloc)(size_t);
static void         (*s_release)(void *);

/* Stack layout built for a thread that has never run. Mirrors exactly what
   rv9_ctx_switch pops, so a first entry and a resume are the same code. */
#define CTX_WORDS 14
#define CTX_RA    0
#define CTX_S0    1
#define CTX_S1    2

void rv9k_init(void)
{
    for (int i = 0; i < RV9K_MAX_THREADS; i++) {
        s_threads[i].state = RV9K_DEAD;
        s_threads[i].next  = NULL;
    }
    s_current       = NULL;
    s_last_age_tick = s_ticks;
    s_switches      = 0;
    s_blocks        = 0;
    s_running       = false;
    s_sched_lock    = 0;
}

/*
 * The whole of what an interrupt does. One counter, one word, no locking
 * and no decisions -- everything the tick implies is worked out later in
 * thread context, where the thread table can be touched safely.
 */
void rv9k_tick(void) { s_ticks++; }

volatile uint32_t *rv9k_tick_ref(void) { return &s_ticks; }

uint32_t rv9k_ticks(void) { return s_ticks; }

static uint32_t now(void) { return s_ticks; }

rv9k_thread_t *rv9k_thread_create(rv9k_entry_fn fn, void *arg, const char *name,
                                  size_t stack_bytes, int priority)
{
    if (fn == NULL || s_alloc == NULL) return NULL;

    rv9k_thread_t *t = NULL;
    for (int i = 0; i < RV9K_MAX_THREADS; i++) {
        if (s_threads[i].state == RV9K_DEAD && s_threads[i].stack == NULL) {
            t = &s_threads[i];
            break;
        }
    }
    if (t == NULL) return NULL;

    size_t words = (stack_bytes + 3) / 4;
    if (words < 128) words = 128;

    uint32_t *stack = (uint32_t *)s_alloc(words * 4);
    if (stack == NULL) return NULL;

    t->stack       = stack;
    t->stack_words = words;

    /* Stacks grow down. RISC-V wants the pointer 16-byte aligned. */
    uint32_t *top = stack + words;
    top = (uint32_t *)((uintptr_t)top & ~(uintptr_t)0xF);

    uint32_t *frame = top - CTX_WORDS;
    for (int i = 0; i < CTX_WORDS; i++) frame[i] = 0;

    frame[CTX_RA] = (uint32_t)(uintptr_t)rv9_thread_trampoline;
    frame[CTX_S0] = (uint32_t)(uintptr_t)fn;    /* trampoline calls this */
    frame[CTX_S1] = (uint32_t)(uintptr_t)arg;   /* with this */

    t->sp = frame;

    int n = 0;
    while (name && name[n] && n < RV9K_NAME_LEN - 1) { t->name[n] = name[n]; n++; }
    t->name[n] = '\0';

    if (priority < RV9K_PRIO_MIN) priority = RV9K_PRIO_MIN;
    if (priority > RV9K_PRIO_MAX) priority = RV9K_PRIO_MAX;

    t->base_priority      = priority;
    t->effective_priority = priority;
    t->age                = 0;
    t->boost              = 0;
    t->state              = RV9K_READY;
    t->wake_at_tick       = 0;
    t->blocked_on         = NULL;
    t->ran_ticks          = 0;
    t->entered_tick       = 0;
    t->last_ran_seq       = 0;

    return t;
}

/* ------------------------------------------------------------------ */
/* Aging                                                               */
/* ------------------------------------------------------------------ */

static void age_threads(void)
{
    uint32_t t = now();
    if ((uint32_t)(t - s_last_age_tick) < RV9K_AGE_PERIOD_TICKS) return;
    s_last_age_tick = t;

    rv9k_thread_t *top = NULL;
    for (int i = 0; i < RV9K_MAX_THREADS; i++) {
        rv9k_thread_t *th = &s_threads[i];
        if (th->state != RV9K_READY && th->state != RV9K_RUNNING) continue;
        if (top == NULL || th->effective_priority > top->effective_priority) {
            top = th;
        }
    }

    for (int i = 0; i < RV9K_MAX_THREADS; i++) {
        rv9k_thread_t *th = &s_threads[i];
        if (th->state != RV9K_READY && th->state != RV9K_RUNNING) continue;

        if (th == top) {
            th->age = 0;
        } else if (th->age < RV9K_AGE_MAX) {
            th->age++;
        }

        int eff = th->base_priority + th->age;
        if (eff > RV9K_PRIO_MAX) eff = RV9K_PRIO_MAX;

        /* An inherited priority is a floor: aging may lift a thread above
           it, but nothing drops it below while it holds the lock. */
        if (th->boost > eff) eff = th->boost;

        th->effective_priority = eff;
    }
}

/* ------------------------------------------------------------------ */
/* Wait queues                                                         */
/*                                                                     */
/* A blocked thread is off the run queue entirely: the scheduler does   */
/* not consider it, so waiting costs nothing. Before this, blocking was */
/* a spin -- mark ready, reschedule, look again -- which was correct    */
/* and burned the CPU of every thread that dared wait for anything.     */
/* ------------------------------------------------------------------ */

static void waitq_push(rv9k_waitq_t *wq, rv9k_thread_t *t)
{
    t->next = wq->head;
    wq->head = t;
}

static void waitq_remove(rv9k_waitq_t *wq, rv9k_thread_t *t)
{
    rv9k_thread_t **pp = &wq->head;
    while (*pp) {
        if (*pp == t) { *pp = t->next; t->next = NULL; return; }
        pp = &(*pp)->next;
    }
}

/* Wake the highest-priority waiter; ties go to whoever waited longest,
   which the list order gives us for free. */
static rv9k_thread_t *waitq_pop_best(rv9k_waitq_t *wq)
{
    rv9k_thread_t *best = NULL;
    for (rv9k_thread_t *t = wq->head; t; t = t->next) {
        if (best == NULL || t->effective_priority > best->effective_priority) {
            best = t;
        }
    }
    if (best) {
        waitq_remove(wq, best);
        best->blocked_on   = NULL;
        best->wake_at_tick = 0;
        best->state        = RV9K_READY;
    }
    return best;
}

/*
 * Block the running thread on a wait queue. Returns true if it was woken
 * by a give, false if the timeout expired first.
 */
static bool block_on(rv9k_waitq_t *wq, uint32_t timeout_ms)
{
    if (s_current == NULL) return false;

    rv9k_thread_t *self = s_current;

    self->blocked_on   = wq;
    self->wake_at_tick = (timeout_ms == 0) ? 0
                       : now() + RV9K_MS_TO_TICKS(timeout_ms);
    self->state        = RV9K_BLOCKED;
    waitq_push(wq, self);
    s_blocks++;

    reschedule();

    /* Awake again. If the timeout fired we are still on the queue. */
    if (self->blocked_on == wq) {
        waitq_remove(wq, self);
        self->blocked_on = NULL;
        return false;
    }
    return true;
}

/* Wake anything whose sleep has expired. */
static void wake_sleepers(void)
{
    uint32_t t = now();
    for (int i = 0; i < RV9K_MAX_THREADS; i++) {
        rv9k_thread_t *th = &s_threads[i];
        if (th->state == RV9K_SLEEPING &&
            RV9K_TICK_REACHED(t, th->wake_at_tick)) {
            th->state = RV9K_READY;
            continue;
        }

        /* A blocked thread with a deadline that has passed comes back
           runnable and discovers for itself that it timed out. */
        if (th->state == RV9K_BLOCKED && th->wake_at_tick != 0 &&
            RV9K_TICK_REACHED(t, th->wake_at_tick)) {
            th->state = RV9K_READY;
        }
    }
}

/*
 * Highest effective priority among the runnable; least-recently-selected
 * first as a tiebreak, so equal priorities round-robin.
 *
 * The tiebreak used a millisecond timestamp at first, which does not work:
 * a thread that yields thousands of times inside one millisecond compares
 * equal to itself and keeps winning. 100 yields produced 4 switches. A
 * monotonic selection counter has no resolution to run out of.
 */
static rv9k_thread_t *pick_next(void)
{
    rv9k_thread_t *best = NULL;

    for (int i = 0; i < RV9K_MAX_THREADS; i++) {
        rv9k_thread_t *th = &s_threads[i];
        if (th->state != RV9K_READY && th->state != RV9K_RUNNING) continue;

        if (best == NULL ||
            th->effective_priority > best->effective_priority ||
            (th->effective_priority == best->effective_priority &&
             th->last_ran_seq < best->last_ran_seq)) {
            best = th;
        }
    }
    return best;
}

/*
 * Return finished threads' stacks and free their slots.
 *
 * Never the running thread: a thread marks itself dead and then
 * reschedules, so it is still standing on the stack being considered
 * until the switch completes. Whoever runs next reaps it.
 */
static void reap_dead(void)
{
    for (int i = 0; i < RV9K_MAX_THREADS; i++) {
        rv9k_thread_t *th = &s_threads[i];
        if (th == s_current) continue;
        if (th->state != RV9K_DEAD || th->stack == NULL) continue;

        if (s_release) s_release(th->stack);
        th->stack       = NULL;
        th->stack_words = 0;
        th->sp          = NULL;
        th->name[0]     = '\0';
    }
}

static bool any_alive(void)
{
    for (int i = 0; i < RV9K_MAX_THREADS; i++) {
        if (s_threads[i].state != RV9K_DEAD) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Switching                                                           */
/* ------------------------------------------------------------------ */

/* Give up the CPU. Called from a thread; returns when it runs again. */
static void reschedule(void)
{
    /* Held off. The caller keeps the CPU; whatever it wanted to happen
       happens when the lock is released. */
    if (s_sched_lock > 0) return;

    rv9k_thread_t *prev = s_current;

    reap_dead();
    wake_sleepers();
    age_threads();

    rv9k_thread_t *next = pick_next();

    if (next == NULL) {
        /* Nothing runnable. Go back to the host, which will either give us
           time again or let the system exit. */
        s_current = NULL;
        if (prev) rv9_ctx_switch(&prev->sp, s_host_sp);
        else return;
        return;
    }

    if (next == prev) {
        if (prev->state == RV9K_RUNNING) return;   /* still the best choice */
    }

    if (prev) {
        /* Charge the outgoing thread for the CPU it actually received. */
        prev->ran_ticks += (uint32_t)(now() - prev->entered_tick);
        if (prev->state == RV9K_RUNNING) prev->state = RV9K_READY;
    }

    next->state        = RV9K_RUNNING;
    next->entered_tick = now();
    s_switches++;
    next->last_ran_seq = s_switches;
    s_current          = next;

    if (prev) {
        rv9_ctx_switch(&prev->sp, next->sp);
    } else {
        rv9_ctx_switch(&s_host_sp, next->sp);
    }
}

void rv9k_sched_lock(void) { s_sched_lock++; }

void rv9k_sched_unlock(void)
{
    if (s_sched_lock > 0) s_sched_lock--;
    if (s_sched_lock == 0 && s_current) reschedule();
}

void rv9k_set_idle_hook(void (*fn)(void)) { s_idle_hook = fn; }

void rv9k_set_allocators(void *(*alloc)(size_t), void (*release)(void *))
{
    s_alloc   = alloc;
    s_release = release;
}

void rv9k_thread_kill(rv9k_thread_t *t)
{
    if (t == NULL) return;
    if (t == s_current) { rv9k_exit(); return; }

    if (t->blocked_on) {
        waitq_remove((rv9k_waitq_t *)t->blocked_on, t);
        t->blocked_on = NULL;
    }
    t->state = RV9K_DEAD;
}

/*
 * Host an operating system: run threads forever, idling when there is
 * nothing to run. Unlike rv9k_run this never returns, because a kernel
 * with nothing to do has not finished -- it is waiting.
 */
void rv9k_serve(void)
{
    s_running = true;

    for (;;) {
        reap_dead();
        wake_sleepers();
        age_threads();

        rv9k_thread_t *next = pick_next();
        if (next == NULL) {
            if (s_idle_hook) s_idle_hook();
            continue;
        }

        next->state        = RV9K_RUNNING;
        next->entered_tick = now();
        s_switches++;
        next->last_ran_seq = s_switches;
        s_current          = next;

        rv9_ctx_switch(&s_host_sp, next->sp);
        s_current = NULL;
    }
}

void rv9k_yield(void)
{
    if (s_current) reschedule();
}

void rv9k_sleep_ticks(uint32_t ticks)
{
    if (s_current == NULL) return;

    s_current->wake_at_tick = now() + ticks;
    s_current->state        = RV9K_SLEEPING;
    reschedule();
}

void rv9k_sleep_ms(uint32_t ms) { rv9k_sleep_ticks(RV9K_MS_TO_TICKS(ms)); }

/*
 * Preemption, taken at a point of the thread's choosing rather than forced
 * on it. If the clock has not moved since this thread was scheduled there
 * is nothing to decide, so the common case is one comparison.
 */
void rv9k_preempt_point(void)
{
    if (s_current == NULL) return;
    if (s_ticks == s_current->entered_tick) return;
    reschedule();
}

void rv9k_exit(void)
{
    if (s_current == NULL) return;

    s_current->state = RV9K_DEAD;
    reschedule();
}

void rv9_thread_exited(void)
{
    rv9k_exit();

    /* rv9k_exit never comes back, but the compiler does not know that and a
       dead thread returning here would be a disaster worth catching. */
    for (;;) { }
}

rv9k_thread_t *rv9k_self(void) { return s_current; }

static void recompute_priority(rv9k_thread_t *t)
{
    int eff = t->base_priority + t->age;
    if (eff > RV9K_PRIO_MAX) eff = RV9K_PRIO_MAX;
    if (t->boost > eff) eff = t->boost;
    t->effective_priority = eff;
}

void rv9k_priority_set(rv9k_thread_t *t, int priority)
{
    if (t == NULL) return;
    if (priority < RV9K_PRIO_MIN) priority = RV9K_PRIO_MIN;
    if (priority > RV9K_PRIO_MAX) priority = RV9K_PRIO_MAX;

    t->base_priority = priority;
    t->age           = 0;
    recompute_priority(t);
}

void rv9k_priority_boost(rv9k_thread_t *t, int priority)
{
    if (t == NULL) return;
    if (priority > RV9K_PRIO_MAX) priority = RV9K_PRIO_MAX;
    if (priority <= t->boost) return;      /* already at least this high */

    t->boost = priority;
    recompute_priority(t);
}

void rv9k_priority_unboost(rv9k_thread_t *t)
{
    if (t == NULL || t->boost == 0) return;

    t->boost = 0;
    recompute_priority(t);
}

void rv9k_run(void)
{
    if (s_running) return;
    s_running = true;

    while (any_alive()) {
        reap_dead();
        wake_sleepers();
        age_threads();

        rv9k_thread_t *next = pick_next();
        if (next == NULL) {
            /* Everything is asleep or blocked. Nothing to do but let time
               pass; a real kernel waits for an interrupt here. */
            continue;
        }

        next->state        = RV9K_RUNNING;
        next->entered_tick = now();
        s_switches++;
        next->last_ran_seq = s_switches;
        s_current          = next;

        rv9_ctx_switch(&s_host_sp, next->sp);
        s_current = NULL;
    }

    s_running = false;
}

/* ------------------------------------------------------------------ */
/* Semaphores                                                          */
/* ------------------------------------------------------------------ */

void rv9k_sem_init(rv9k_sem_t *sem, int32_t initial, int32_t max)
{
    if (sem == NULL) return;
    sem->count        = initial;
    sem->max          = max;
    sem->waiters.head = NULL;
}

bool rv9k_sem_take(rv9k_sem_t *sem, uint32_t timeout_ms)
{
    if (sem == NULL) return false;

    for (;;) {
        if (sem->count > 0) {
            sem->count--;
            return true;
        }
        if (timeout_ms == 0 || s_current == NULL) return false;

        /* Sleep on the queue. Woken either by a give or by the deadline;
           the loop then decides which by looking at the count again,
           because a give can be taken by someone else in between. */
        if (!block_on(&sem->waiters, timeout_ms)) return false;
    }
}

void rv9k_sem_give(rv9k_sem_t *sem)
{
    if (sem == NULL) return;
    if (sem->max <= 0 || sem->count < sem->max) sem->count++;

    waitq_pop_best(&sem->waiters);
}

/* ------------------------------------------------------------------ */
/* Mutexes                                                             */
/* ------------------------------------------------------------------ */

void rv9k_mutex_init(rv9k_mutex_t *m)
{
    if (m == NULL) return;
    rv9k_sem_init(&m->sem, 1, 1);
    m->owner = NULL;
    m->depth = 0;
}

bool rv9k_mutex_lock(rv9k_mutex_t *m, uint32_t timeout_ms)
{
    if (m == NULL) return false;

    /* Already ours: count the nesting rather than deadlock on ourselves. */
    if (m->owner != NULL && m->owner == s_current) {
        m->depth++;
        return true;
    }

    if (!rv9k_sem_take(&m->sem, timeout_ms)) return false;

    m->owner = s_current;
    m->depth = 1;
    return true;
}

void rv9k_mutex_unlock(rv9k_mutex_t *m)
{
    if (m == NULL || m->depth == 0) return;

    if (--m->depth > 0) return;

    m->owner = NULL;
    rv9k_sem_give(&m->sem);
}

/* ------------------------------------------------------------------ */
/* Queues                                                              */
/* ------------------------------------------------------------------ */

static void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}

void rv9k_queue_init(rv9k_queue_t *q, void *storage, uint32_t capacity,
                     uint32_t item_size)
{
    if (q == NULL) return;
    q->storage   = (uint8_t *)storage;
    q->capacity  = capacity;
    q->item_size = item_size;
    q->head = q->tail = q->count = 0;
    q->not_empty.head = NULL;
    q->not_full.head  = NULL;
}

uint32_t rv9k_queue_count(const rv9k_queue_t *q)
{
    return q ? q->count : 0;
}

bool rv9k_queue_send(rv9k_queue_t *q, const void *item, uint32_t timeout_ms)
{
    if (q == NULL || item == NULL) return false;

    for (;;) {
        if (q->count < q->capacity) {
            copy_bytes(q->storage + (size_t)q->tail * q->item_size,
                       (const uint8_t *)item, q->item_size);
            q->tail = (q->tail + 1) % q->capacity;
            q->count++;
            waitq_pop_best(&q->not_empty);
            return true;
        }
        if (timeout_ms == 0 || s_current == NULL) return false;
        if (!block_on(&q->not_full, timeout_ms)) return false;
    }
}

bool rv9k_queue_recv(rv9k_queue_t *q, void *item, uint32_t timeout_ms)
{
    if (q == NULL || item == NULL) return false;

    for (;;) {
        if (q->count > 0) {
            copy_bytes((uint8_t *)item,
                       q->storage + (size_t)q->head * q->item_size,
                       q->item_size);
            q->head = (q->head + 1) % q->capacity;
            q->count--;
            waitq_pop_best(&q->not_full);
            return true;
        }
        if (timeout_ms == 0 || s_current == NULL) return false;
        if (!block_on(&q->not_empty, timeout_ms)) return false;
    }
}

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

int rv9k_thread_count(void)
{
    int n = 0;
    for (int i = 0; i < RV9K_MAX_THREADS; i++) {
        if (s_threads[i].state != RV9K_DEAD || s_threads[i].stack) n++;
    }
    return n;
}

const rv9k_thread_t *rv9k_thread_at(int index)
{
    if (index < 0 || index >= RV9K_MAX_THREADS) return NULL;
    return &s_threads[index];
}

uint64_t rv9k_switch_count(void) { return s_switches; }
uint64_t rv9k_block_count(void)  { return s_blocks; }
