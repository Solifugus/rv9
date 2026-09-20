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
extern void rv9_ctx_load(uint32_t *load_sp);      /* switch away, unsaved */
extern void rv9_thread_trampoline(void);

/* The trampoline jumps here if a thread returns from its entry point. */
void rv9_thread_exited(void);

static void reschedule(void);
static rv9k_thread_t *waitq_pop_best(rv9k_waitq_t *wq);
static bool stack_intact(const rv9k_thread_t *t);
static void stack_fault(rv9k_thread_t *t);

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
static uint32_t       s_stack_faults;

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/*                                                                     */
/* A kernel masks interrupts itself rather than borrowing somebody      */
/* else's critical section -- this one is below the seam and has no     */
/* host to ask. Single core, so clearing MIE is mutual exclusion        */
/* against every interrupt handler, and the sections here are a         */
/* handful of instructions long.                                       */
/* ------------------------------------------------------------------ */

static inline uint32_t irq_save(void)
{
    uint32_t prev;
    __asm__ volatile ("csrrci %0, mstatus, 8" : "=r"(prev) :: "memory");
    return prev;
}

static inline void irq_restore(uint32_t prev)
{
    /* Set MIE only if it was set: restoring the whole register would
       clobber whatever else has changed since. */
    if (prev & 8u) __asm__ volatile ("csrsi mstatus, 8" ::: "memory");
}

/* ------------------------------------------------------------------ */
/* Gives that arrive from an interrupt                                 */
/*                                                                     */
/* An interrupt cannot wake a thread directly. Waking means unlinking   */
/* it from a wait queue and putting it on the ready list, and an        */
/* interrupt landing in the middle of the scheduler doing the same      */
/* thing to the same list leaves it in pieces. Making every queue       */
/* operation interrupt-safe was the obvious reading of the problem and  */
/* is the expensive one.                                               */
/*                                                                     */
/* It is also unnecessary. The *count* is what a waiter is waiting on,  */
/* and an interrupt can raise that safely in four instructions. The     */
/* wake is only how a sleeping thread finds out, so it can be left as   */
/* a note for the kernel to act on in thread context, where the lists   */
/* are nobody else's business.                                         */
/*                                                                     */
/* So a thread about to block never blocks -- it sees the count and     */
/* carries on -- and one already blocked is woken at the next turn of   */
/* the scheduler. Which makes the latency of this a scheduling round,   */
/* not an interrupt: right for a semaphore, and the reason real-time    */
/* work does not come through here at all.                             */
/* ------------------------------------------------------------------ */

#define PENDING_MAX 16

static rv9k_waitq_t  *s_pending[PENDING_MAX];
static volatile uint32_t s_pend_head, s_pend_tail;
static volatile uint32_t s_pend_lost;      /* notes the ring had no room for */

/* Caller must hold the interrupt mask. */
static void pend_wake(rv9k_waitq_t *wq)
{
    if (wq->head == NULL) return;          /* nobody to wake */

    uint32_t next = (s_pend_head + 1u) % PENDING_MAX;
    if (next != s_pend_tail) {
        s_pending[s_pend_head] = wq;
        s_pend_head = next;
    } else {
        /* The count or the item still landed, so nothing is lost and
           nobody blocking from here on misses it; only a thread already
           asleep waits for the next one. Counted rather than hidden. */
        s_pend_lost++;
    }
}

static void drain_pending(void)
{
    for (;;) {
        uint32_t st = irq_save();

        if (s_pend_tail == s_pend_head) { irq_restore(st); return; }

        rv9k_waitq_t *wq = s_pending[s_pend_tail];
        s_pend_tail = (s_pend_tail + 1u) % PENDING_MAX;

        irq_restore(st);

        /* Thread context now, so the wait queue is safe to touch. */
        if (wq != NULL) waitq_pop_best(wq);
    }
}
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
        s_threads[i].held  = false;
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
        if (s_threads[i].state == RV9K_DEAD && s_threads[i].stack == NULL &&
            !s_threads[i].held) {
            t = &s_threads[i];
            break;
        }
    }
    if (t == NULL) return NULL;

    size_t words = (stack_bytes + 3) / 4;
    if (words < 128) words = 128;

    /* The pad sits underneath and is never handed to the thread: see
       RV9K_STACK_PAD_WORDS. It is painted with everything else so that a
       thread which reaches it is still caught by the guard above it. */
    uint32_t *base = (uint32_t *)s_alloc((words + RV9K_STACK_PAD_WORDS) * 4);
    if (base == NULL) return NULL;

    uint32_t *stack = base + RV9K_STACK_PAD_WORDS;

    /*
     * Paint it, so that how much was used can be asked afterwards.
     *
     * Without this the only stack measurement available is "it did not
     * crash", which is not a measurement. A pattern and a scan turn a guess
     * into a number, which is what anyone sizing a stack actually needs.
     *
     * The same pattern in the lowest words is the guard: see
     * RV9K_GUARD_WORDS and stack_intact().
     */
    for (size_t i = 0; i < words + RV9K_STACK_PAD_WORDS; i++) {
        base[i] = RV9K_STACK_PAINT;
    }

    t->stack       = stack;
    t->stack_words = words;
    t->fault       = RV9K_FAULT_NONE;
    t->held        = false;
    t->holds       = 0;
    t->cancel      = false;
    t->mem_class   = 0;

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
    t->has_deadline       = false;
    t->blocked_on         = NULL;
    t->ran_ticks          = 0;
    t->entered_tick       = 0;
    t->last_ran_seq       = 0;
    t->local              = NULL;

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
    self->has_deadline = (timeout_ms != RV9K_WAIT_FOREVER);
    self->wake_at_tick = self->has_deadline
                       ? now() + RV9K_MS_TO_TICKS(timeout_ms) : 0;
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
           runnable and discovers for itself that it timed out. One
           blocked forever has no deadline and is left where it is; only a
           give, a kill or a stop moves it. */
        if (th->state == RV9K_BLOCKED && th->has_deadline &&
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

        if (s_release) s_release(th->stack - RV9K_STACK_PAD_WORDS);
        th->stack       = NULL;
        th->stack_words = 0;
        th->sp          = NULL;

        /* A faulted thread's memory goes back, but its slot does not: see
           rv9k_thread_release. Its name is what identifies it in the
           report, so that stays too. */
        if (!th->held) th->name[0] = '\0';
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
    drain_pending();

    /* Held off. The caller keeps the CPU; whatever it wanted to happen
       happens when the lock is released. */
    if (s_sched_lock > 0) return;

    rv9k_thread_t *prev = s_current;

    /*
     * Before anything else: did the thread we are leaving run off the
     * bottom of its own stack?
     *
     * This is the only moment the question can be asked cheaply. A thread
     * that is running has its stack pointer in a register, not in memory,
     * so there is nothing to inspect until it stops -- and every thread
     * stops here.
     *
     * `abandon` says the outgoing context must not be saved: see
     * rv9_ctx_load. s_current deliberately keeps pointing at the corpse so
     * that reap_dead() below leaves the stack alone while we are still
     * standing on it; whoever runs next frees it.
     */
    bool abandon = false;

    if (prev != NULL && !stack_intact(prev)) {
        stack_fault(prev);
        abandon = true;
    }

    reap_dead();
    wake_sleepers();
    age_threads();

    rv9k_thread_t *next = pick_next();

    if (next == NULL) {
        /* Nothing runnable. Go back to the host, which will either give us
           time again or let the system exit. */
        s_current = NULL;
        if (abandon)   rv9_ctx_load(s_host_sp);        /* does not return */
        else if (prev) rv9_ctx_switch(&prev->sp, s_host_sp);
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

    if (abandon) {
        rv9_ctx_load(next->sp);                        /* does not return */
    } else if (prev) {
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

int rv9k_thread_stop(rv9k_thread_t *t)
{
    if (t == NULL || t == s_current || t->state == RV9K_DEAD) return -1;
    if (t->holds > 0) {
        t->cancel = true;
        return -2;
    }

    if (t->blocked_on) {
        waitq_remove((rv9k_waitq_t *)t->blocked_on, t);
        t->blocked_on = NULL;
    }
    t->fault = RV9K_FAULT_KILLED;
    t->held  = true;
    t->state = RV9K_DEAD;
    return 0;
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
        drain_pending();
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

/*
 * Bytes of this thread's stack never written.
 *
 * Counted from the low end, where a stack grows down to: the first word
 * still holding the pattern marks the deepest the thread has ever been.
 * Zero means it has touched every byte it was given, which means it has
 * very likely gone past them.
 */
size_t rv9k_stack_unused(const rv9k_thread_t *t)
{
    if (t == NULL || t->stack == NULL) return 0;

    size_t i = 0;
    while (i < t->stack_words && t->stack[i] == RV9K_STACK_PAINT) i++;
    return i * sizeof(uint32_t);
}

/*
 * Has this thread written below the floor of its own stack?
 *
 * Checked when it is switched away from, which is every time it blocks,
 * yields or is preempted -- so an overrun is caught within one scheduling
 * decision of happening rather than whenever the damage surfaces.
 */
static bool stack_intact(const rv9k_thread_t *t)
{
    if (t == NULL || t->stack == NULL) return true;

    /*
     * The pad is part of the guard, because containment without detection
     * turned out to be worth very little.
     *
     * Four words used to be the whole of it, and four words is a wall that
     * a frame can step over without touching: a function whose deepest
     * local is a buffer it only half fills writes at its own offsets and
     * leaves gaps. `smash` found this by panicking the board -- it descends
     * until the paint scan says eight bytes remain, and that scan can only
     * see *inside* the stack, so it reads "eight bytes left" while the
     * deepest frame has reached a hundred and fifty bytes below the floor.
     * The crash was in free(), reaping that stack: the heap block header
     * underneath it had been rewritten.
     *
     * The pad below every stack was already allocated and already painted,
     * for exactly those writes to land in. Reading it costs thirty-two
     * more comparisons at a switch and turns "it landed somewhere
     * harmless" into "it was caught", which is the difference between a
     * detector and a cushion.
     *
     * Downward, so the commonest overrun -- the word just below the floor
     * -- is found first.
     */
    const uint32_t *p = t->stack;
    for (int i = RV9K_GUARD_WORDS - 1; i >= -RV9K_STACK_PAD_WORDS; i--) {
        if (p[i] != RV9K_STACK_PAINT) return false;
    }
    return true;
}

bool rv9k_stack_ok(const rv9k_thread_t *t)
{
    return stack_intact(t);
}

/*
 * Stop it, and say so.
 *
 * There is no recovering the memory below the stack -- it is already
 * written -- so the only useful act is to stop the thread running again.
 *
 * Nothing is printed here. A kernel below the seam has no business owning
 * a console, and the layer that knows this thread is a *process* is the
 * one that can say so usefully; it notices through rv9k_thread_fault.
 */
static void stack_fault(rv9k_thread_t *t)
{
    t->fault = RV9K_FAULT_STACK;
    t->state = RV9K_DEAD;
    t->held  = true;
    s_stack_faults++;
}

bool rv9k_is_thread(const void *p)
{
    if (p == NULL) return false;

    const uint8_t *b  = (const uint8_t *)p;
    const uint8_t *lo = (const uint8_t *)&s_threads[0];
    const uint8_t *hi = (const uint8_t *)&s_threads[RV9K_MAX_THREADS];

    if (b < lo || b >= hi) return false;
    return ((size_t)(b - lo) % sizeof(rv9k_thread_t)) == 0;
}

void rv9k_thread_release(rv9k_thread_t *t)
{
    if (t == NULL) return;
    t->held    = false;
    t->fault   = RV9K_FAULT_NONE;
    t->name[0] = '\0';
}

int rv9k_thread_fault(const rv9k_thread_t *t)
{
    return t ? t->fault : RV9K_FAULT_NONE;
}

bool rv9k_thread_alive(const rv9k_thread_t *t)
{
    return t != NULL && t->state != RV9K_DEAD;
}

uint32_t rv9k_stack_faults(void) { return s_stack_faults; }

size_t rv9k_stack_size(const rv9k_thread_t *t)
{
    if (t == NULL) return 0;
    return t->stack_words * sizeof(uint32_t);
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
        uint32_t st = irq_save();
        bool got = (sem->count > 0);
        if (got) sem->count--;
        irq_restore(st);

        if (got) return true;
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

    uint32_t st = irq_save();
    if (sem->max <= 0 || sem->count < sem->max) sem->count++;
    irq_restore(st);

    waitq_pop_best(&sem->waiters);
}

/*
 * Give from an interrupt. Raises the count now and leaves the waking to
 * the kernel; see the note above the pending ring.
 *
 * `woken` reports whether anything was actually waiting, which is what a
 * handler uses to decide whether to ask for a reschedule on its way out.
 */
bool rv9k_sem_give_from_isr(rv9k_sem_t *sem, bool *woken)
{
    if (woken) *woken = false;
    if (sem == NULL) return false;

    uint32_t st = irq_save();

    bool room = (sem->max <= 0 || sem->count < sem->max);
    if (room) sem->count++;

    bool waiting = (sem->waiters.head != NULL);
    pend_wake(&sem->waiters);

    irq_restore(st);

    if (woken) *woken = waiting;
    return room;
}

uint32_t rv9k_pending_lost(void) { return s_pend_lost; }

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
    if (s_current) s_current->holds++;
    return true;
}

void rv9k_mutex_unlock(rv9k_mutex_t *m)
{
    if (m == NULL || m->depth == 0) return;

    if (--m->depth > 0) return;

    if (m->owner && m->owner->holds > 0) m->owner->holds--;
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
        uint32_t st = irq_save();
        bool room = (q->count < q->capacity);
        if (room) {
            copy_bytes(q->storage + (size_t)q->tail * q->item_size,
                       (const uint8_t *)item, q->item_size);
            q->tail = (q->tail + 1) % q->capacity;
            q->count++;
        }
        irq_restore(st);

        if (room) {
            waitq_pop_best(&q->not_empty);
            return true;
        }
        if (timeout_ms == 0 || s_current == NULL) return false;
        if (!block_on(&q->not_full, timeout_ms)) return false;
    }
}

/*
 * Send from an interrupt. Same bargain as the semaphore: the item lands
 * now, the waking waits for thread context.
 *
 * Returns false when the queue is full, which for a handler means the item
 * is gone -- there is nowhere to put it and nothing useful to do about it
 * inside an interrupt.
 */
bool rv9k_queue_send_from_isr(rv9k_queue_t *q, const void *item, bool *woken)
{
    if (woken) *woken = false;
    if (q == NULL || item == NULL) return false;

    uint32_t st = irq_save();

    bool room = (q->count < q->capacity);
    if (room) {
        copy_bytes(q->storage + (size_t)q->tail * q->item_size,
                   (const uint8_t *)item, q->item_size);
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
    }

    bool waiting = (q->not_empty.head != NULL);
    if (room) pend_wake(&q->not_empty);

    irq_restore(st);

    if (woken) *woken = waiting && room;
    return room;
}

bool rv9k_queue_recv(rv9k_queue_t *q, void *item, uint32_t timeout_ms)
{
    if (q == NULL || item == NULL) return false;

    for (;;) {
        /* Masked, because an interrupt may be filling this queue at the
           same time and count, head and tail are shared with it. */
        uint32_t st = irq_save();
        bool have = (q->count > 0);
        if (have) {
            copy_bytes((uint8_t *)item,
                       q->storage + (size_t)q->head * q->item_size,
                       q->item_size);
            q->head = (q->head + 1) % q->capacity;
            q->count--;
        }
        irq_restore(st);

        if (have) {
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

void *rv9k_thread_local_get(rv9k_thread_t *t) { return t ? t->local : NULL; }

void rv9k_thread_local_set(rv9k_thread_t *t, void *value)
{
    if (t) t->local = value;
}

uint64_t rv9k_switch_count(void) { return s_switches; }
uint64_t rv9k_block_count(void)  { return s_blocks; }
