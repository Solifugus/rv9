/*
 * pico -- Espressif's pedestrian detector, in plain integer C.
 *
 * The engine behind `detect`. It reads a model file made by tools/mkpico.py
 * from ESP-DL's pedestrian_detect_pico_s8_v1 and reproduces ESP-DL's int8
 * arithmetic, rounding included, with no floating point, no allocator and
 * nothing from a C library -- so the same file builds as an RV-9 module and
 * on a PC, where it can be checked against what ESP-DL says.
 *
 * Memory is the caller's: the model as read from its file, and an arena of
 * pico_arena_bytes() for the activations.
 */
#pragma once

#include <stdint.h>

#define PICO_RGB565  0      /* little-endian 16-bit, as /cam0 gives */
#define PICO_RGB888  1      /* three bytes, red first */

typedef struct {
    int32_t  x1, y1, x2, y2;    /* in the picture's pixels, inclusive */
    uint32_t score;             /* 0..65536 is 0..1 */
} pico_box_t;

typedef struct {
    const uint8_t *m;           /* the model file */
    const uint8_t *tensors, *nodes, *blob;
    uint32_t n_tensors, n_nodes, arena;
    int pie;                    /* use the P4's vector unit: see below */
} pico_t;

/*
 * The P4's vector unit. pico_open leaves it off; a caller that knows the
 * CPU has one (RV9_ISA_PIE) turns it on. Only a build with PICO_PIE has the
 * code at all, and the answers are the same either way: the vector kernels
 * round in the hardware exactly as ESP-DL has it do, half to even.
 *
 * The model and the arena must be 16-byte aligned for it.
 */

/* 0, or negative if this is not a model file this engine reads. */
int pico_open(pico_t *p, const void *model, uint32_t size);

static inline uint32_t pico_arena_bytes(const pico_t *p) { return p->arena; }

/*
 * Find the people in a picture of w x h pixels. Returns how many boxes it
 * wrote to `out`, best first, at most `max` (and at most ten).
 */
int pico_run(const pico_t *p, void *arena, const void *px, int w, int h,
             int fmt, pico_box_t *out, int max);

/* Step by step, for checking a layer at a time: preprocess, then node i. */
void pico_input(const pico_t *p, void *arena, const void *px, int w, int h,
                int fmt);
void pico_node(const pico_t *p, void *arena, uint32_t i);
int  pico_post(const pico_t *p, void *arena, int w, int h, pico_box_t *out,
               int max);

/* Tensor t: where it is in the arena and what shape. */
const int8_t *pico_tensor(const pico_t *p, const void *arena, uint32_t t,
                          int *h, int *w, int *c, int *exp);

/* A hash of the six output tensors, after a run: the same number on the
   board and on a PC means the same bytes, without moving 34 KB to show it. */
uint32_t pico_hash(const pico_t *p, const void *arena);
