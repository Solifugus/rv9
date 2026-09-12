#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#define RV9_OK 0
typedef void *rv9_lock_t;
static inline int rv9_lock_create(rv9_lock_t *l){*l=(void*)1;return RV9_OK;}
static inline void rv9_lock_acquire(rv9_lock_t l){(void)l;}
static inline void rv9_lock_release(rv9_lock_t l){(void)l;}
static inline void *rv9_alloc(size_t n){return malloc(n);}
static inline void *rv9_alloc_dma(size_t n){return malloc(n);}
static inline void *rv9_calloc(size_t a,size_t b){return calloc(a,b);}
static inline void rv9_free(void *p){free(p);}
static inline uint64_t rv9_time_us(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000u+t.tv_nsec/1000;}
