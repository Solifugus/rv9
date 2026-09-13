/*
 * RV-9 native kernel.
 *
 * This is what phase 7 is for: RV-9's own scheduler, implementing the
 * policy the process manager has been asking for since phase 2 -- priority
 * with aging -- directly rather than by steering someone else's priorities
 * from above.
 *
 * WHERE THIS RUNS TODAY
 *
 * The kernel currently runs inside one host task, which gives it a stack
 * and a CPU to borrow. Threads switch cooperatively: they yield, sleep, or
 * block on a kernel primitive. That is deliberate sequencing, not the
 * destination:
 *
 *   step 1 (here)  context switch, run queues, priority + aging, and the
 *                  KAL conformance suite passing against it
 *   step 2         our own timer interrupt, so preemption does not need
 *                  anyone's cooperation
 *   step 3         own the CPU from reset; the host scheduler goes away
 *   step 4         wifi_osi_funcs_t, so the radio blobs run on RV-9
 *   step 5         PMP, so processes cannot reach each other's memory
 *
 * Doing step 1 inside a host task means the context switch and the
 * scheduler can be proven correct while something known-good is still
 * holding the machine up. Bringing up a scheduler with no working system
 * to compare against is how these projects stall.
 *
 * The kernel depends on nothing. It is handed a time source at init and
 * has no other outside reference -- no KAL, no host headers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RV9K_MAX_THREADS   32
#define RV9K_NAME_LEN      16

/* Same priority band as the rest of RV-9; see rv9/kal.h. */
#define RV9K_PRIO_MIN      0
#define RV9K_PRIO_MAX      15

/*
 * The kernel's clock is its own tick and nothing else. Time is counted in
 * ticks, sleeps are measured in ticks, and aging happens every so many
 * ticks. Nothing outside is consulted -- an interrupt calls rv9k_tick()
 * and that is the entire dependency.
 */
#define RV9K_TICK_HZ          1000
#define RV9K_MS_TO_TICKS(ms)  ((ms) * RV9K_TICK_HZ / 1000)

/* Aging, matching the policy proven in phase 2. */
#define RV9K_AGE_MAX          10
#define RV9K_AGE_PERIOD_TICKS 20

typedef enum {
    RV9K_DEAD = 0,
    RV9K_READY,
    RV9K_RUNNING,
    RV9K_SLEEPING,
    RV9K_BLOCKED,
} rv9k_state_t;

typedef struct rv9k_thread rv9k_thread_t;

typedef void (*rv9k_entry_fn)(void *arg);

struct rv9k_thread {
    uint32_t      *sp;              /* saved stack pointer; must be first */
    uint32_t      *stack;           /* allocation, for freeing */
    size_t         stack_words;

    char           name[RV9K_NAME_LEN];
    rv9k_state_t   state;
    int            base_priority;
    int            age;
    int            effective_priority;
    int            boost;           /* floor imposed by priority inheritance */

    uint32_t       wake_at_tick;    /* when sleeping */
    void          *blocked_on;      /* which primitive, when blocked */

    uint32_t       ran_ticks;       /* accounting: CPU actually received */
    uint32_t       entered_tick;    /* when this thread last got the CPU */
    uint64_t       last_ran_seq;    /* selection order, for round-robin */

    void          *local;           /* one pointer belonging to this thread */
    int            fault;           /* RV9K_FAULT_*, why the kernel stopped it */

    /* Locks this thread holds right now, of either kind -- and blocking
       operations it is inside that must finish to leave nothing behind. A
       thread holding one cannot be stopped from outside: see
       rv9k_thread_stop. */
    uint32_t       holds;

    /* Somebody tried to stop this thread while it held something. Anything
       it is blocked in that can give up early should, and let go. */
    bool           cancel;

    /* RV9_MEM_*: how far into the heap's reserves this thread may allocate.
       See rv9_mem_class_set. Zero is ordinary. */
    uint8_t        mem_class;
    bool           held;            /* corpse kept for whoever is watching */

    rv9k_thread_t *next;
};

/* Task-local storage: one pointer, whoever wants it. */
void *rv9k_thread_local_get(rv9k_thread_t *t);
void  rv9k_thread_local_set(rv9k_thread_t *t, void *value);

/*
 * A queue of threads waiting for something. Threads are linked through
 * their own `next` field, so a wait queue costs one pointer and no
 * allocation -- which matters in a kernel that must be able to block a
 * thread when memory is exhausted.
 */
typedef struct {
    rv9k_thread_t *head;
} rv9k_waitq_t;

/* A counting semaphore, and the thing mutexes and queues are built from. */
typedef struct {
    int32_t      count;
    int32_t      max;
    rv9k_waitq_t waiters;
} rv9k_sem_t;

/* Start the kernel. It is given nothing; time arrives via rv9k_tick(). */
void rv9k_init(void);

/*
 * The kernel's clock.
 *
 * rv9k_tick() advances it and does nothing else; everything the tick
 * implies -- waking sleepers, aging, deciding a switch is due -- happens
 * later in thread context where the thread table can be touched safely.
 *
 * rv9k_tick_ref() hands out the counter's address so that whoever owns the
 * timer can increment it directly from an interrupt handler, without
 * calling into the kernel at all. On this hardware that matters: interrupt
 * handlers may run while the flash cache is disabled, and code that lives
 * in flash faults if it is called then. Keeping the kernel out of
 * interrupt context entirely is simpler than annotating it to survive
 * being there.
 *
 * The counter is 32 bits so that an increment is a single store on a
 * 32-bit machine and a reader can never see half of one. It wraps after
 * about 49 days at 1 kHz; every comparison below is written to survive
 * that.
 *
 * True asynchronous preemption -- interrupting a thread and resuming a
 * different one -- needs the trap vector, and arrives with step 3.
 */
void rv9k_tick(void);
volatile uint32_t *rv9k_tick_ref(void);

uint32_t rv9k_ticks(void);

/* Wraparound-safe "has `deadline` arrived?" */
#define RV9K_TICK_REACHED(now, deadline) ((int32_t)((now) - (deadline)) >= 0)

/*
 * A cheap place for a thread to be preempted. Returns immediately unless
 * the clock has moved since this thread was last scheduled, in which case
 * the scheduler gets a chance to pick someone else.
 */
void rv9k_preempt_point(void);

/*
 * Where thread stacks come from and go back to. Both are needed: a kernel
 * that can only allocate stacks runs out of thread slots, which is how a
 * system that forks a process per command dies after its sixteenth one.
 */
void rv9k_set_allocators(void *(*alloc)(size_t), void (*release)(void *));

rv9k_thread_t *rv9k_thread_create(rv9k_entry_fn fn, void *arg, const char *name,
                                  size_t stack_bytes, int priority);

/* Run until every thread has finished. Returns to the caller's context. */
void rv9k_run(void);

/*
 * Run forever, hosting an operating system rather than a test.
 *
 * When nothing is runnable the idle hook is called. On bare metal that is
 * a wait-for-interrupt; while RV-9 is a guest it is the host's way of
 * giving up the CPU, so the rest of the machine keeps working. Without it
 * an idle kernel would spin at whatever priority its host task has.
 */
void rv9k_set_idle_hook(void (*fn)(void));

/* Stacks are painted at creation so their use can be measured rather than
   guessed. Unused counts from the low end; zero means it has run out. */
#define RV9K_STACK_PAINT 0xA5C3A5C3u

/*
 * The lowest words of a stack are a guard, never legitimately written.
 *
 * A stack grows down, so the deepest thing a thread does lands here first.
 * Four words is not a wall -- a single large local can step straight over
 * it -- but it catches the ordinary case of a call chain one frame too
 * deep, which is the failure a hand-declared stack actually produces.
 *
 * Real prevention needs the PMP, which is phase 7's last step. Until then
 * this is detection: the corruption has already happened when it fires,
 * and the value is that it is reported rather than mysterious.
 */
#define RV9K_GUARD_WORDS 4

/*
 * Space below the guard that belongs to nobody.
 *
 * A guard that only reports is worth much less than one that also
 * contains. Without this, a thread one frame too deep has already
 * overwritten whatever the allocator put underneath it by the time the
 * guard is noticed -- so the report arrives alongside a second, silent
 * failure in an unrelated process.
 *
 * Every stack is allocated with this much extra underneath it, and the
 * thread is never told about it: `stack` points above it, `stack_words`
 * excludes it, and the measurement reports the stack the thread asked for.
 * It is a place for a modest overrun to land, not more stack.
 *
 * 128 bytes covers the ordinary case -- a call chain a frame or two too
 * deep. A single large local still steps over it, which is what the PMP is
 * for.
 */
#define RV9K_STACK_PAD_WORDS 32

/* Why a thread was stopped by the kernel rather than by itself. */
#define RV9K_FAULT_NONE   0
#define RV9K_FAULT_STACK  1
#define RV9K_FAULT_KILLED 2   /* stopped from outside; see rv9k_thread_stop */

size_t rv9k_stack_unused(const rv9k_thread_t *t);

/* Non-zero once the kernel has stopped this thread for a fault. */
int    rv9k_thread_fault(const rv9k_thread_t *t);
bool   rv9k_thread_alive(const rv9k_thread_t *t);

/*
 * Let go of a faulted thread's slot.
 *
 * A thread the kernel killed keeps its slot after its stack is freed, so
 * that whoever holds a handle to it can still ask what happened. Without
 * that hold the slot is reused by the next thread created, and a watcher
 * comparing against a stale handle is told the corpse is alive and well --
 * which is worse than no answer at all.
 *
 * The layer that noticed the fault calls this when it has finished with
 * it. Threads that end normally are never held.
 */
void   rv9k_thread_release(rv9k_thread_t *t);

/*
 * Is this handle one of ours?
 *
 * Not every task in the system is an RV-9 thread. A real-time process
 * runs on the host's scheduler, and its handle is a host object that
 * happens to be a pointer -- so anything above the seam holding a task
 * handle may be holding either kind, and reading one as the other returns
 * whatever was at that offset. That is how `stacks` came to report a
 * 34-megabyte stack, and it is how a healthy real-time process could have
 * been declared dead and buried by the fault collector.
 *
 * The kernel's threads live in one fixed array, so the question has an
 * exact answer: is the pointer inside it, and at an element boundary.
 */
bool   rv9k_is_thread(const void *p);

/* How many threads have been stopped for overrunning their stack. */
uint32_t rv9k_stack_faults(void);
size_t rv9k_stack_size(const rv9k_thread_t *t);
void rv9k_serve(void);

/* Stop scheduling other threads. Nesting counts. */
void rv9k_sched_lock(void);
void rv9k_sched_unlock(void);

/* End a thread that is not the caller. */
void rv9k_thread_kill(rv9k_thread_t *t);

/*
 * Stop another thread, and leave the corpse for whoever asked.
 *
 * Unlike rv9k_thread_kill, this refuses when stopping would break
 * something else. A thread that is not running is parked at a switch
 * point, and a switch point is a safe place to stop *unless the thread is
 * holding a lock there*: that lock would then never be released, and the
 * next thread to want it waits forever -- which on a machine running a
 * control loop is a second failure caused by fixing the first.
 *
 * So a thread holding anything is not stopped. The caller is told, and
 * asks again; the lock is normally held for microseconds. The thread is
 * also marked `cancel`, because what it holds is not always a lock: it may
 * be inside an open waiting for a network connection that never comes,
 * and only it can let go of the socket that wait created.
 *
 * The corpse is held like a stack fault's, fault RV9K_FAULT_KILLED, so the
 * layer that asked can read what happened before the slot is reused.
 *
 * Returns 0 when stopped, -1 for the caller itself or a thread already
 * dead, -2 when it holds a lock.
 */
int rv9k_thread_stop(rv9k_thread_t *t);

void rv9k_yield(void);
void rv9k_sleep_ms(uint32_t ms);
void rv9k_sleep_ticks(uint32_t ticks);
void rv9k_exit(void);

rv9k_thread_t *rv9k_self(void);
void           rv9k_priority_set(rv9k_thread_t *t, int priority);

/*
 * Priority inheritance, the scheduler's half.
 *
 * A thread holding a lock that something more urgent is waiting for runs
 * at the waiter's priority until it lets go. Without this, the holder sits
 * behind every medium-priority thread in the system while the waiter --
 * which may be a control loop -- waits on it. That is priority inversion,
 * and it is the classic way a real-time system misses a deadline for
 * reasons that look like nothing to do with timing.
 *
 * A boost is a floor, not an assignment: aging may raise the thread
 * further, and releasing the lock returns it to whatever it had earned.
 */
void rv9k_priority_boost(rv9k_thread_t *t, int priority);
void rv9k_priority_unboost(rv9k_thread_t *t);

void rv9k_sem_init(rv9k_sem_t *sem, int32_t initial, int32_t max);
bool rv9k_sem_take(rv9k_sem_t *sem, uint32_t timeout_ms);
void rv9k_sem_give(rv9k_sem_t *sem);

/*
 * Give from an interrupt handler.
 *
 * The count rises immediately; the wake happens at the next turn of the
 * scheduler, because unlinking a thread from a wait queue is not
 * something an interrupt may do to a cooperative kernel. A thread about
 * to block therefore never blocks, and one already asleep wakes within a
 * scheduling round rather than within microseconds -- which is what a
 * semaphore is for. Real-time work does not come through here.
 *
 * Returns false only if the count was already at its maximum.
 */
bool rv9k_sem_give_from_isr(rv9k_sem_t *sem, bool *woken);

/* Gives from an interrupt that found the pending ring full. Should be
   zero; a non-zero value means a sleeping thread waited longer than it
   had to. */
uint32_t rv9k_pending_lost(void);

/*
 * A mutex is a binary semaphore that remembers its owner, so that a
 * recursive lock does not deadlock against itself.
 */
typedef struct {
    rv9k_sem_t     sem;
    rv9k_thread_t *owner;
    uint32_t       depth;
} rv9k_mutex_t;

void rv9k_mutex_init(rv9k_mutex_t *m);
bool rv9k_mutex_lock(rv9k_mutex_t *m, uint32_t timeout_ms);
void rv9k_mutex_unlock(rv9k_mutex_t *m);

/*
 * A fixed-capacity ring of fixed-size items. The storage belongs to the
 * caller, which keeps the kernel out of the allocation business -- it has
 * no heap of its own and should not pretend otherwise.
 */
typedef struct {
    uint8_t     *storage;
    uint32_t     capacity;      /* items */
    uint32_t     item_size;
    uint32_t     head;
    uint32_t     tail;
    uint32_t     count;
    rv9k_waitq_t not_empty;     /* receivers waiting for an item */
    rv9k_waitq_t not_full;      /* senders waiting for room */
} rv9k_queue_t;

void     rv9k_queue_init(rv9k_queue_t *q, void *storage, uint32_t capacity,
                         uint32_t item_size);
bool     rv9k_queue_send(rv9k_queue_t *q, const void *item, uint32_t timeout_ms);
bool     rv9k_queue_recv(rv9k_queue_t *q, void *item, uint32_t timeout_ms);

/* Send from an interrupt handler. The item lands now; a waiting receiver
   is woken at the next turn of the scheduler. False means the queue was
   full and the item was dropped. See rv9k_sem_give_from_isr. */
bool     rv9k_queue_send_from_isr(rv9k_queue_t *q, const void *item,
                                  bool *woken);
uint32_t rv9k_queue_count(const rv9k_queue_t *q);

/* ------------------------------------------------------------------ */
/* The kernel's own heap                                               */
/*                                                                     */
/* Given one region at init and asked no further questions. A kernel    */
/* that owns the machine cannot borrow someone else's allocator, and    */
/* until now this one did.                                             */
/*                                                                     */
/* No locking: the kernel is cooperative and single-core, none of these */
/* functions reschedule, and the tick interrupt does not touch the      */
/* heap. That reasoning is the lock. It stops being sufficient the      */
/* moment either assumption changes, which is why it is written here.   */
/* ------------------------------------------------------------------ */

void  rv9k_heap_init(void *base, size_t bytes);
void *rv9k_alloc(size_t bytes);
void *rv9k_calloc(size_t count, size_t size);
void  rv9k_free(void *ptr);

typedef struct {
    size_t   total;
    size_t   free_bytes;
    size_t   largest_free;   /* the number fragmentation ruins */
    uint32_t blocks;
    uint32_t free_blocks;
} rv9k_heap_stats_t;

void rv9k_heap_stats(rv9k_heap_stats_t *out);

/* Introspection, for tests and for `procs` when this becomes the kernel. */
int  rv9k_thread_count(void);
const rv9k_thread_t *rv9k_thread_at(int index);
uint64_t rv9k_switch_count(void);

/* How many times a thread has blocked without a spin. Proof, for the
   tests, that waiting costs nothing rather than burning the CPU. */
uint64_t rv9k_block_count(void);

#ifdef __cplusplus
}
#endif
