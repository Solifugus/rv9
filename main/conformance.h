/*
 * The KAL contract, as a table of operations and a suite that exercises it.
 *
 * This exists so the same tests can be run against more than one
 * implementation. Phase 0 said the conformance suite would become the
 * acceptance test for the native kernel; a suite that calls one
 * implementation directly cannot do that, so it takes the implementation
 * as an argument instead.
 *
 * Return convention: 0 on success, negative on failure or timeout.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;

    uint64_t (*time_ms)(void);
    void     (*delay_ms)(uint32_t ms);
    void     (*yield)(void);

    int      (*task_create)(void (*fn)(void *), const char *name,
                            size_t stack_bytes, void *arg, int priority);
    void     (*task_exit)(void);
    void    *(*task_self)(void);

    int      (*sem_create)(uint32_t max, uint32_t initial, void **out);
    void     (*sem_destroy)(void *sem);
    int      (*sem_take)(void *sem, uint32_t timeout_ms);
    int      (*sem_give)(void *sem);

    int      (*mutex_create)(void **out);
    int      (*mutex_create_recursive)(void **out);
    void     (*mutex_destroy)(void *m);
    int      (*mutex_lock)(void *m, uint32_t timeout_ms);
    int      (*mutex_unlock)(void *m);

    int      (*queue_create)(uint32_t length, size_t item_size, void **out);
    void     (*queue_destroy)(void *q);
    int      (*queue_send)(void *q, const void *item, uint32_t timeout_ms);
    int      (*queue_recv)(void *q, void *item, uint32_t timeout_ms);
    uint32_t (*queue_count)(void *q);

    void    *(*alloc)(size_t n);
    void    *(*calloc)(size_t count, size_t size);
    void     (*free)(void *p);
} kal_ops_t;

/* Runs the portable contract. Returns the number of failures. */
int rv9_conformance_run(const kal_ops_t *ops);

/* The two implementations. */
const kal_ops_t *rv9_ops_freertos(void);
const kal_ops_t *rv9_ops_native(void);
