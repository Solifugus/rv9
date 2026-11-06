/*
 * pico -- Espressif's small detectors, in plain integer C.
 *
 * The engine behind `detect`. It reads model files made by tools/mkpico.py
 * from ESP-DL's pedestrian detector (pico_s8_v1) and two-stage face
 * detector (msr_s8_v1 + mnp_s8_v1) and reproduces ESP-DL's int8
 * arithmetic, rounding included, with no floating point, no allocator and
 * nothing from a C library -- so the same file builds as an RV-9 module and
 * on a PC, where it can be checked against what ESP-DL says.
 *
 * Memory is the caller's: each model as read from its file, and an arena
 * of pico_arena_bytes() for the activations (the larger of the two, for
 * the face detector's stages, which take turns in it).
 */
#pragma once

#include <stdint.h>

#define PICO_RGB565  0      /* little-endian 16-bit, as /cam0 gives */
#define PICO_RGB888  1      /* three bytes, red first */

#define PICO_KIND_PEOPLE  1     /* pedestrian_detect pico */
#define PICO_KIND_FACE1   2     /* human_face_detect msr: proposes */
#define PICO_KIND_FACE2   3     /* human_face_detect mnp: looks closer */
#define PICO_KIND_SIG     4     /* human_face_feat mfn: who it is */

#define PICO_SIG  512           /* numbers in a face's signature */

typedef struct {
    int32_t  x1, y1, x2, y2;    /* in the picture's pixels, inclusive */
    uint32_t score;             /* 0..65536 is 0..1 */
} pico_box_t;

/* A face: its box, and five points -- left eye, left corner of the mouth,
   nose, right eye, right corner of the mouth, as x, y. "Left" is the
   picture's left. `key` is for ordering, not for reading. */
typedef struct {
    pico_box_t box;
    int32_t    pt[10];
    int32_t    key;
} pico_face_t;

typedef struct {
    const uint8_t *m;           /* the model file */
    const uint8_t *tensors, *nodes, *blob;
    uint32_t n_tensors, n_nodes, arena;
    int      kind, in_h, in_w, bgr, centred;
    uint32_t n_out, out[8];     /* output tensors, in the kind's order */
    int pie;                    /* use the P4's vector unit: see below */
} pico_t;

/*
 * The P4's vector unit. pico_open leaves it off; a caller that knows the
 * CPU has one (RV9_ISA_PIE) turns it on. Only a build with PICO_PIE has the
 * code at all, and the answers are the same either way: the vector kernels
 * round in the hardware exactly as ESP-DL has it do, half to even.
 *
 * The models and the arena must be 16-byte aligned for it.
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

/*
 * Called after every layer the face detector runs, with the model and the
 * layer's index -- for a caller that must let others run, or wants to know
 * where the time went. `ctx` is the caller's.
 */
typedef void (*pico_between_t)(void *ctx, const pico_t *p, uint32_t node);

/*
 * Find the faces: the first stage over the whole picture, then the second
 * on each square it proposed. `between` may be NULL. At most ten.
 */
int pico_faces(const pico_t *face1, const pico_t *face2, void *arena,
               const void *px, int w, int h, int fmt, pico_face_t *out,
               int max, pico_between_t between, void *ctx);

/* Step by step, for checking a layer at a time. */
void pico_input(const pico_t *p, void *arena, const void *px, int w, int h,
                int fmt);
void pico_input_rect(const pico_t *p, void *arena, const void *px, int w,
                     int h, int fmt, int x1, int y1, int x2, int y2);
void pico_node(const pico_t *p, void *arena, uint32_t i);
void pico_layers(const pico_t *p, void *arena);
int  pico_post(const pico_t *p, void *arena, int w, int h, pico_box_t *out,
               int max);
int  pico_msr_post(const pico_t *p, void *arena, int w, int h,
                   pico_face_t *out, int max);

/* Tensor t: where it is in the arena and what shape. A pixel's channels
   are pico_stride() bytes apart, which is c rounded up to sixteen. */
const int8_t *pico_tensor(const pico_t *p, const void *arena, uint32_t t,
                          int *h, int *w, int *c, int *exp);
int pico_stride(const pico_t *p, uint32_t t);

/* A hash of the output tensors, after a run: the same number on the board
   and on a PC means the same bytes, without moving them to show it. */
uint32_t pico_hash(const pico_t *p, const void *arena);

/*
 * Who a face is: its signature, 512 int8, from the five points
 * pico_faces found -- the face turned, scaled and shifted to where
 * Espressif's model expects it, then the model. Two pictures of one
 * person give similar signatures; compare them as unit vectors.
 */
int pico_signature(const pico_t *p, void *arena, const void *px, int w, int h,
                   int fmt, const int32_t pt[10], int8_t sig[PICO_SIG],
                   pico_between_t between, void *ctx);

/* A signature, or a sum of several, scaled to unit length in Q15. */
void pico_unit(const int32_t *v, int16_t out[PICO_SIG]);

/* The cosine of two unit signatures, Q16: 65536 is identical, and
   ESP-DL calls a face the same person above 0.5 (32768). */
int32_t pico_similar(const int16_t a[PICO_SIG], const int16_t b[PICO_SIG]);

/* The alignment pico_signature uses, as Q16, for checking it. */
void pico_align(const int32_t pt[10], int32_t m_q16[6]);
