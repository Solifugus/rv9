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

/* stack_bytes of 0 selects a backend-chosen default. */
rv9_err_t rv9_task_create(rv9_task_fn fn, const char *name, size_t stack_bytes,
                          void *arg, int priority, rv9_task_t *out_task);

/* Passing NULL deletes the calling task, which does not return. */
void      rv9_task_delete(rv9_task_t task);
rv9_task_t rv9_task_self(void);
void      rv9_task_yield(void);
void      rv9_task_delay_ms(uint32_t ms);
rv9_err_t rv9_task_priority_set(rv9_task_t task, int priority);

/* ------------------------------------------------------------------ */
/* Counting semaphores                                                 */
/* ------------------------------------------------------------------ */

typedef struct rv9_sem *rv9_sem_t;

rv9_err_t rv9_sem_create(uint32_t max_count, uint32_t initial_count,
                         rv9_sem_t *out_sem);
void      rv9_sem_destroy(rv9_sem_t sem);
rv9_err_t rv9_sem_take(rv9_sem_t sem, uint32_t timeout_ms);
rv9_err_t rv9_sem_give(rv9_sem_t sem);
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
void  rv9_free(void *ptr);

size_t rv9_heap_free(void);        /* bytes currently free */
size_t rv9_heap_low_water(void);   /* smallest free ever seen */

/* ------------------------------------------------------------------ */
/* Critical sections                                                   */
/*                                                                     */
/* Short, non-blocking regions only. No allocation, no waiting, no I/O */
/* between enter and exit.                                             */
/* ------------------------------------------------------------------ */

void rv9_critical_enter(void);
void rv9_critical_exit(void);

/*
 * TODO (phase 3): ISR attach/detach. Not needed until drivers exist, and
 * getting the shape right matters more than having it early -- the native
 * kernel and wifi_osi_funcs_t both constrain it.
 */

#ifdef __cplusplus
}
#endif
