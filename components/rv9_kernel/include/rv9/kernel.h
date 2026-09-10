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

/* Aging, matching the policy proven in phase 2. */
#define RV9K_AGE_MAX       10
#define RV9K_AGE_PERIOD_MS 20

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

    uint64_t       wake_at_ms;      /* when sleeping */
    void          *blocked_on;      /* which primitive, when blocked */

    uint64_t       ran_for_ms;      /* accounting, and proof aging works */
    uint64_t       last_ran_seq;    /* selection order, for round-robin */

    rv9k_thread_t *next;
};

/* A counting semaphore, and the thing mutexes and queues are built from. */
typedef struct {
    int32_t        count;
    int32_t        max;
    rv9k_thread_t *waiters;
} rv9k_sem_t;

/* Start the kernel. `now_ms` is the only thing it is given from outside. */
void rv9k_init(uint64_t (*now_ms)(void));

rv9k_thread_t *rv9k_thread_create(rv9k_entry_fn fn, void *arg, const char *name,
                                  size_t stack_bytes, int priority,
                                  void *(*alloc)(size_t));

/* Run until every thread has finished. Returns to the caller's context. */
void rv9k_run(void);

void rv9k_yield(void);
void rv9k_sleep_ms(uint32_t ms);
void rv9k_exit(void);

rv9k_thread_t *rv9k_self(void);
void           rv9k_priority_set(rv9k_thread_t *t, int priority);

void rv9k_sem_init(rv9k_sem_t *sem, int32_t initial, int32_t max);
bool rv9k_sem_take(rv9k_sem_t *sem, uint32_t timeout_ms);
void rv9k_sem_give(rv9k_sem_t *sem);

/* Introspection, for tests and for `procs` when this becomes the kernel. */
int  rv9k_thread_count(void);
const rv9k_thread_t *rv9k_thread_at(int index);
uint64_t rv9k_switch_count(void);

#ifdef __cplusplus
}
#endif
