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

#define RV9K_MAX_THREADS   16
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

    uint32_t       wake_at_tick;    /* when sleeping */
    void          *blocked_on;      /* which primitive, when blocked */

    uint32_t       ran_ticks;       /* accounting: CPU actually received */
    uint32_t       entered_tick;    /* when this thread last got the CPU */
    uint64_t       last_ran_seq;    /* selection order, for round-robin */

    rv9k_thread_t *next;
};

/* A counting semaphore, and the thing mutexes and queues are built from. */
typedef struct {
    int32_t        count;
    int32_t        max;
    rv9k_thread_t *waiters;
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

rv9k_thread_t *rv9k_thread_create(rv9k_entry_fn fn, void *arg, const char *name,
                                  size_t stack_bytes, int priority,
                                  void *(*alloc)(size_t));

/* Run until every thread has finished. Returns to the caller's context. */
void rv9k_run(void);

void rv9k_yield(void);
void rv9k_sleep_ms(uint32_t ms);
void rv9k_sleep_ticks(uint32_t ticks);
void rv9k_exit(void);

rv9k_thread_t *rv9k_self(void);
void           rv9k_priority_set(rv9k_thread_t *t, int priority);

void rv9k_sem_init(rv9k_sem_t *sem, int32_t initial, int32_t max);
bool rv9k_sem_take(rv9k_sem_t *sem, uint32_t timeout_ms);
void rv9k_sem_give(rv9k_sem_t *sem);

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
    uint8_t  *storage;
    uint32_t  capacity;      /* items */
    uint32_t  item_size;
    uint32_t  head;
    uint32_t  tail;
    uint32_t  count;
} rv9k_queue_t;

void     rv9k_queue_init(rv9k_queue_t *q, void *storage, uint32_t capacity,
                         uint32_t item_size);
bool     rv9k_queue_send(rv9k_queue_t *q, const void *item, uint32_t timeout_ms);
bool     rv9k_queue_recv(rv9k_queue_t *q, void *item, uint32_t timeout_ms);
uint32_t rv9k_queue_count(const rv9k_queue_t *q);

/* Introspection, for tests and for `procs` when this becomes the kernel. */
int  rv9k_thread_count(void);
const rv9k_thread_t *rv9k_thread_at(int index);
uint64_t rv9k_switch_count(void);

#ifdef __cplusplus
}
#endif
