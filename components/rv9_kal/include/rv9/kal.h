/*
 * RV-9 Kernel Abstraction Layer
 *
 * This is the seam. Everything above the KAL is portable RV-9; everything
 * below it is whichever kernel is hosting us today. Phase 1-6 run on
 * FreeRTOS; phase 7 replaces the backend with the native RV-9 kernel and
 * nothing above this header changes.
 *
 * The surface is deliberately a superset of what `wifi_osi_funcs_t` and
 * lwIP's `sys_arch` require, because those are the interfaces the native
 * kernel will eventually have to satisfy anyway. See docs/design.md §4.
 *
 * RULE: no header or source above the KAL may include FreeRTOS headers.
 * Enforced by tools/check_layering.sh at build time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Where real-time code lives                                          */
/* ------------------------------------------------------------------ */

/*
 * RV9_RT_CODE marks a function that a control loop runs while a deadline
 * is pending, and that therefore must be reachable at all times.
 *
 * On this host that means IRAM. The flash on an ESP32 is memory-mapped
 * through a cache, and the cache is switched off for the duration of every
 * write to flash -- which the radio does by itself, storing calibration
 * data after it associates. Code sitting in flash simply is not there
 * while that happens. Measured on the C5: the first real-time loop after a
 * boot with WiFi enabled lost 200 ms in one piece, and the same loop with
 * the radio off never lost more than 12 us.
 *
 * So this is not an optimisation. A loop whose code can vanish for a fifth
 * of a second is not a real-time loop, however good its average looks.
 *
 * When RV-9 owns the machine there is no cache to lose and this becomes
 * nothing -- which is the point of naming the property rather than the
 * mechanism.
 */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define RV9_RT_CODE IRAM_ATTR
#else
#define RV9_RT_CODE
#endif

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    RV9_OK = 0,
    RV9_ERR_INVAL,        /* bad argument */
    RV9_ERR_NOMEM,        /* allocation failed */
    RV9_ERR_TIMEOUT,      /* wait expired */
    RV9_ERR_UNSUPPORTED,  /* not implemented by this backend */
} rv9_err_t;

const char *rv9_strerror(rv9_err_t err);

/* ------------------------------------------------------------------ */
/* Time and waiting                                                    */
/* ------------------------------------------------------------------ */

#define RV9_NO_WAIT      ((uint32_t)0)
#define RV9_WAIT_FOREVER ((uint32_t)0xFFFFFFFFu)

uint64_t rv9_time_us(void);
uint64_t rv9_time_ms(void);

/*
 * Milliseconds in whatever unit the host kernel counts time in. Needed
 * because some hardware APIs demand a tick count; nothing above the KAL
 * should have to know what a tick is.
 */
uint32_t rv9_ms_to_ticks(uint32_t ms);

/* ------------------------------------------------------------------ */
/* Tasks                                                               */
/*                                                                     */
/* RV-9 defines its own priority range; backends map onto it. Higher   */
/* number means more urgent. Aging (design §6) belongs to the RV-9     */
/* process manager, not here -- the KAL only exposes raw priority.     */
/* ------------------------------------------------------------------ */

#define RV9_PRIO_IDLE    0
#define RV9_PRIO_LOW     4
#define RV9_PRIO_NORMAL  8
#define RV9_PRIO_HIGH    12
#define RV9_PRIO_MAX     15

typedef struct rv9_task *rv9_task_t;
typedef void (*rv9_task_fn)(void *arg);

/*
 * Bring the kernel up and start the system's first task.
 *
 * Whichever kernel backs the KAL, this is how RV-9 starts: the backend
 * does whatever its kernel needs -- FreeRTOS needs nothing, RV-9's own
 * kernel needs a heap, a tick and something to run it -- and then runs
 * `fn` as the first task. It does not return.
 */
rv9_err_t rv9_kal_start(rv9_task_fn fn, const char *name, size_t stack_bytes,
                        void *arg, int priority);

/* stack_bytes of 0 selects a backend-chosen default. */
rv9_err_t rv9_task_create(rv9_task_fn fn, const char *name, size_t stack_bytes,
                          void *arg, int priority, rv9_task_t *out_task);

/* Passing NULL deletes the calling task, which does not return. */
void      rv9_task_delete(rv9_task_t task);

/*
 * How big this task's stack is and how much of it has never been touched.
 *
 * Stacks are the dominant per-process cost -- everything else about a
 * process is about a kilobyte -- so sizing them by guesswork wastes most
 * of a small machine's memory or corrupts it. This is the measurement
 * that lets a stack be cut down with evidence rather than nerve.
 */
rv9_err_t rv9_task_stack(rv9_task_t task, size_t *size, size_t *unused);

/*
 * Why a task stopped, if it stopped for a reason worth naming.
 *
 * A task that runs off the bottom of its stack cannot report that itself:
 * by the time anything notices, the thing that would have done the
 * reporting is the thing that was overwritten. So the scheduler notices
 * instead, and leaves a fault code behind on the corpse.
 *
 * Only RV-9's own kernel detects this. On the host backend the answer is
 * always NONE, which is a statement about what is known, not about what
 * happened.
 */
#define RV9_TASK_FAULT_NONE   0
#define RV9_TASK_FAULT_STACK  1

int  rv9_task_fault(rv9_task_t task);

/*
 * Is this task still able to run?
 *
 * The ordinary way a process ends is by returning, and the trampoline that
 * called it reports that. A task killed by the scheduler never returns, so
 * nothing reports anything and anybody in wait() waits forever. This is
 * how the process manager finds out anyway.
 *
 * On the host backend this is always true: the trampoline is the only
 * authority there, and claiming otherwise would invent knowledge.
 */
bool rv9_task_alive(rv9_task_t task);

/*
 * Done with the corpse.
 *
 * A task the scheduler killed is kept, emptied of everything expensive,
 * until whoever was watching has read the fault off it -- otherwise the
 * handle is reused underneath them and reports the next task's health as
 * though it were this one's. This says the reading is finished.
 *
 * Harmless on a task that ended normally, and harmless on the host
 * backend, which keeps nothing.
 */
void rv9_task_reap(rv9_task_t task);
rv9_task_t rv9_task_self(void);
void      rv9_task_yield(void);
void      rv9_task_delay_ms(uint32_t ms);
rv9_err_t rv9_task_priority_set(rv9_task_t task, int priority);

/*
 * Does the scheduler underneath age priorities itself?
 *
 * RV-9's process manager has implemented aging since phase 2 by nudging
 * task priorities from above, because the host scheduler does not do it.
 * RV-9's own kernel does, and the two fight: setting a priority resets the
 * age the kernel just applied. So the process manager asks.
 *
 * The policy is unchanged either way -- what changes is which layer
 * carries it out, which is the whole point of having written the policy
 * down rather than left it implicit in one implementation.
 */
bool rv9_sched_ages(void);

/*
 * A point at which this task may be descheduled.
 *
 * On a preemptive host this is free: the scheduler already takes the CPU
 * whenever it likes. On RV-9's own kernel it is what makes a compute-bound
 * thread preemptible at all, because that kernel switches only when asked.
 *
 * The system call layer calls this, which makes every system call a
 * preemption point -- so a module doing work interleaved with any kernel
 * service is scheduled fairly without knowing this function exists. A
 * module that computes for a long time touching nothing still cannot be
 * interrupted; that needs the trap vector, and arrives with phase 7 step 3.
 */
void rv9_preempt_point(void);

/* ------------------------------------------------------------------ */
/* Locks usable from any context                                        */
/*                                                                     */
/* A mutex (above) is a scheduling object: it blocks the caller in      */
/* whichever scheduler the caller belongs to. That is correct and       */
/* useless for data shared between RV-9 threads and host-scheduled      */
/* work -- a real-time process, or a driver's own task -- because       */
/* those belong to different schedulers.                                */
/*                                                                     */
/* A lock works from either. Use it for structures both worlds touch:   */
/* the process table, the path tables, driver state. Hold it briefly    */
/* and never block inside it: an RV-9 thread waiting on one stalls the  */
/* whole cooperative kernel until it is released.                       */
/* ------------------------------------------------------------------ */

typedef struct rv9_lock *rv9_lock_t;

/*
 * Priority inheritance is on. It can be turned off, which exists so that
 * the cost of inversion can be demonstrated rather than asserted.
 */
void rv9_lock_set_inheritance(bool on);
bool rv9_lock_get_inheritance(void);

rv9_err_t rv9_lock_create(rv9_lock_t *out_lock);
void      rv9_lock_destroy(rv9_lock_t lock);
void      rv9_lock_acquire(rv9_lock_t lock);
void      rv9_lock_release(rv9_lock_t lock);

/* ------------------------------------------------------------------ */
/* Task-local storage                                                   */
/*                                                                     */
/* One pointer belonging to whichever task or thread is running. Used   */
/* by the I/O manager to remember where a process's path table is, so   */
/* that reading and writing a path costs a dereference instead of a     */
/* lock and a search.                                                   */
/*                                                                     */
/* That matters for real-time work: a control loop should not have to   */
/* acquire a lock shared with the shell in order to move a servo.       */
/* ------------------------------------------------------------------ */

void *rv9_task_local_get(void);
void  rv9_task_local_set(void *value);

/* ------------------------------------------------------------------ */
/* Events -- something happened, and when                              */
/*                                                                     */
/* An event is a rendezvous between an interrupt and a thread. OS-9    */
/* had these as first-class kernel objects (F$Event) and named them by */
/* a small integer, which is what we do too: a driver creates one and  */
/* hands out its id through getstat, and whoever wants waking asks to  */
/* be released by that id. Neither end needs a pointer to the other,   */
/* which is what keeps a driver above the seam from having to know     */
/* that real-time tasks exist at all.                                  */
/*                                                                     */
/* The signal carries a timestamp taken in the interrupt handler. That */
/* is the whole point: the number a reactive system is judged on is    */
/* how long after the world changed the software noticed, and that     */
/* cannot be measured from the far end.                                */
/* ------------------------------------------------------------------ */

typedef struct rv9_event *rv9_event_t;

rv9_err_t rv9_event_create(rv9_event_t *out_event);
void      rv9_event_destroy(rv9_event_t ev);

/* Small integer naming an event. 0 is "no event"; ids start at 1. */
int         rv9_event_id(rv9_event_t ev);
rv9_event_t rv9_event_by_id(int id);

/*
 * Signal from an interrupt handler. Resident, because the interrupt that
 * matters most is the one that arrives while the flash cache is off.
 *
 * The caller does not deal with waking anyone: yielding from an interrupt
 * is the host kernel's business and stays on this side of the seam.
 *
 * (The attribute goes on the definition, not here. IRAM_ATTR names a unique
 * section per use, so repeating it on the declaration puts the two in
 * different sections and the compiler rightly objects.)
 */
void rv9_event_signal_from_isr(rv9_event_t ev);

/* The same from ordinary code, for sources that are not interrupts. */
void rv9_event_signal(rv9_event_t ev);

/* ------------------------------------------------------------------ */
/* Real-time tasks                                                     */
/*                                                                     */
/* A class of task that is late if it is late. These do not run on      */
/* RV-9's cooperative scheduler -- a control loop cannot depend on      */
/* every other thread being polite -- but on a preemptive scheduler at  */
/* the top of the priority order, released by a hardware timer or by a  */
/* hardware event.                                                     */
/*                                                                     */
/* The shape is the one the native implementation will have when RV-9   */
/* owns the machine, so control code written against this API does not  */
/* get rewritten when the kernel underneath grows up.                   */
/* ------------------------------------------------------------------ */

/*
 * The same six numbers describe both kinds of real-time task, because the
 * questions are the same ones:
 *
 *   periodic        period_us is the declared period; max_jitter_us is how
 *                   late the worst release was against the timer; overruns
 *                   counts periods that elapsed while still working.
 *
 *   event-driven    period_us is the declared minimum inter-arrival, or 0
 *                   for a source with no bound; max_jitter_us is how long
 *                   the worst event waited between the interrupt and this
 *                   task running; overruns counts events that arrived while
 *                   still working and were coalesced into one activation.
 *
 * Lateness is one idea measured against two different clocks -- the timer
 * we own, or the world we do not -- and control code that treats it as one
 * idea is control code that can change its release source without being
 * rewritten. That is why rv9_rt_wait() is the same call for both.
 */
typedef struct {
    uint32_t period_us;
    uint64_t activations;
    uint64_t overruns;        /* periods missed, or events coalesced */
    uint32_t max_jitter_us;   /* worst lateness of a release */
    uint32_t max_exec_us;     /* worst time spent in one activation */
    uint32_t last_exec_us;

    /* Event-driven only, and not in the module ABI: see below. */
    bool     event_driven;
    uint32_t min_interval_us; /* shortest gap between events actually seen */
    uint64_t floods;          /* events closer together than declared */
} rv9_rt_stats_t;

rv9_err_t rv9_task_create_rt(rv9_task_fn fn, const char *name,
                             size_t stack_bytes, void *arg,
                             rv9_task_t *out_task);

/* Called by the task itself, once, before its loop. */
rv9_err_t rv9_rt_declare(uint32_t period_us);

/*
 * Declare this task released by an event rather than by a period.
 *
 * min_interval_us is the shortest gap between events the caller is
 * promising to cope with -- a sporadic task's equivalent of a period, and
 * the thing that makes the load analysable at all. Pass 0 to say honestly
 * that there is no bound; nothing is then promised, and the arrivals are
 * still measured so that the bound can be discovered.
 *
 * An event source that fires faster than declared is not stopped. It is
 * counted, because a control system whose inputs are arriving faster than
 * its designer expected needs to be told, not throttled.
 */
rv9_err_t rv9_rt_declare_event(rv9_event_t ev, uint32_t min_interval_us);

/*
 * Wait for the next release -- the next period, or the next event.
 *
 * Returns how many releases were missed while the caller was still working
 * (0 when on time), negative on error. A missed deadline is reported rather
 * than absorbed. A control loop that silently falls behind is worse than one
 * that stops, because it looks correct right up until something hits
 * something.
 */
int rv9_rt_wait(void);

/* Give up the period and the timer. A real-time process that ends without
   this leaves its slot occupied, and the next one cannot declare. */
void rv9_rt_release(void);

rv9_err_t rv9_rt_stats(rv9_rt_stats_t *out);
rv9_err_t rv9_rt_stats_by_index(int index, rv9_rt_stats_t *out, bool *valid);

/* ------------------------------------------------------------------ */
/* Counting semaphores                                                 */
/* ------------------------------------------------------------------ */

/*
 * For calls whose refusal a caller must not drop on the floor -- the ones
 * a backend may legitimately not implement. Everything else in the KAL
 * either cannot fail or fails in a way the caller will notice anyway.
 */
#define RV9_MUST_CHECK __attribute__((warn_unused_result))

typedef struct rv9_sem *rv9_sem_t;

rv9_err_t rv9_sem_create(uint32_t max_count, uint32_t initial_count,
                         rv9_sem_t *out_sem);
void      rv9_sem_destroy(rv9_sem_t sem);
rv9_err_t rv9_sem_take(rv9_sem_t sem, uint32_t timeout_ms);
rv9_err_t rv9_sem_give(rv9_sem_t sem);
/*
 * Give from an interrupt.
 *
 * NOT IMPLEMENTED under the native kernel, which refuses with
 * RV9_ERR_UNSUPPORTED rather than pretend -- making its wait queues
 * interrupt-safe is real work and has not been done. The FreeRTOS backend
 * supports it.
 *
 * Hence RV9_MUST_CHECK. A caller that drops the result gets a primitive
 * that silently does nothing, and the consequence lands somewhere else
 * entirely: the panel driver waited on a semaphore nothing could ever
 * give, timed out on every transfer, and turned a 1.2 second boot into
 * fourteen. The stub was honest; the caller was not listening.
 */
RV9_MUST_CHECK
rv9_err_t rv9_sem_give_from_isr(rv9_sem_t sem, bool *higher_prio_woken);

/* ------------------------------------------------------------------ */
/* Mutexes                                                             */
/* ------------------------------------------------------------------ */

typedef struct rv9_mutex *rv9_mutex_t;

rv9_err_t rv9_mutex_create(rv9_mutex_t *out_mutex);
rv9_err_t rv9_mutex_create_recursive(rv9_mutex_t *out_mutex);
void      rv9_mutex_destroy(rv9_mutex_t mutex);
rv9_err_t rv9_mutex_lock(rv9_mutex_t mutex, uint32_t timeout_ms);
rv9_err_t rv9_mutex_unlock(rv9_mutex_t mutex);

/* ------------------------------------------------------------------ */
/* Queues                                                              */
/* ------------------------------------------------------------------ */

typedef struct rv9_queue *rv9_queue_t;

rv9_err_t rv9_queue_create(uint32_t length, size_t item_size,
                           rv9_queue_t *out_queue);
void      rv9_queue_destroy(rv9_queue_t queue);
rv9_err_t rv9_queue_send(rv9_queue_t queue, const void *item,
                         uint32_t timeout_ms);
rv9_err_t rv9_queue_recv(rv9_queue_t queue, void *item, uint32_t timeout_ms);
/* Also unimplemented under the native kernel. See rv9_sem_give_from_isr. */
RV9_MUST_CHECK
rv9_err_t rv9_queue_send_from_isr(rv9_queue_t queue, const void *item,
                                  bool *higher_prio_woken);
uint32_t  rv9_queue_count(rv9_queue_t queue);

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/*                                                                     */
/* rv9_alloc_dma returns memory usable by DMA-capable peripherals.      */
/* On this hardware that is a real distinction; do not conflate them.  */
/* ------------------------------------------------------------------ */

void *rv9_alloc(size_t size);
void *rv9_calloc(size_t count, size_t size);
void *rv9_alloc_dma(size_t size);

/* Memory that may be executed. The module loader copies module text here.
   On this hardware IRAM and DRAM are the same physical range, so this is
   cheap -- but do not assume that of every future target. */
void *rv9_alloc_exec(size_t size);

/* Internal RAM specifically: thread stacks are written to with the flash
   cache disabled, so they cannot live anywhere else. */
void *rv9_alloc_internal(size_t size);

void  rv9_free(void *ptr);

size_t rv9_heap_free(void);        /* bytes currently free */
size_t rv9_heap_free_exec(void);   /* bytes free that may be executed */
size_t rv9_heap_low_water(void);   /* smallest free ever seen */

/*
 * The floor: memory RV-9 will not take.
 *
 * Everything above allocates through this file and can be told no.
 * ESP-IDF's own internals -- WiFi, the PHY, the SPI driver -- cannot: they
 * allocate straight from the heap and abort when they fail, inside a layer
 * RV-9 does not own. That is a reboot caused by somebody else's request,
 * and it is how this board actually died: the window, an SSH session and a
 * control loop together.
 *
 * So the last few kilobytes are never offered to RV-9. An allocation that
 * would take free memory below the floor returns NULL, the process manager
 * says "no memory to start it", and the machine stays up. It does not make
 * more memory exist -- it chooses which failure happens, and only one of
 * them leaves a system running.
 *
 * `rv9_heap_free` says how much exists. `rv9_heap_available` says how much
 * may be spent, which is the number that decides whether the next process
 * starts; reporting the first as though it were the second is how a system
 * walks confidently into a wall.
 */
size_t   rv9_heap_floor(void);
void     rv9_heap_floor_set(size_t bytes);
size_t   rv9_heap_available(void);
uint32_t rv9_heap_refusals(void);   /* allocations turned away, since boot */

/* Spend the reserve deliberately. For making a failure legible, and
   nothing else -- never for anything a module can reach. */
void *rv9_alloc_critical(size_t size);

/* ------------------------------------------------------------------ */
/* Critical sections                                                   */
/*                                                                     */
/* Short, non-blocking regions only. No allocation, no waiting, no I/O */
/* between enter and exit.                                             */
/* ------------------------------------------------------------------ */

void rv9_critical_enter(void);
void rv9_critical_exit(void);

/* ------------------------------------------------------------------ */
/* Scheduler lock                                                      */
/*                                                                     */
/* Stop other threads from being scheduled, without disabling          */
/* interrupts. Weaker than a critical section and safe to hold for      */
/* longer, but no blocking call may be made while it is held.          */
/*                                                                     */
/* RV-9's own scheduler needs this while it is a guest: its threads run */
/* on stacks the host kernel knows nothing about, and a host context    */
/* switch taken while the stack pointer is one of those is a very bad   */
/* afternoon. The native backend implements it in one line, and step 3  */
/* of phase 7 removes the need entirely.                               */
/* ------------------------------------------------------------------ */

void rv9_sched_lock(void);
void rv9_sched_unlock(void);

/* ------------------------------------------------------------------ */
/* Instruction stream synchronisation                                  */
/*                                                                     */
/* Call after writing code into memory and before jumping to it. On    */
/* RISC-V this is fence.i; other targets may need more.                */
/* ------------------------------------------------------------------ */

void rv9_isync(void);

/*
 * TODO (phase 3): ISR attach/detach. Not needed until drivers exist, and
 * getting the shape right matters more than having it early -- the native
 * kernel and wifi_osi_funcs_t both constrain it.
 */

#ifdef __cplusplus
}
#endif
