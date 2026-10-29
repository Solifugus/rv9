/*
 * What the engine's allocation macros mean under RV-9. Forced into every
 * needle/ source with -include, so the vendored files stay byte-for-byte.
 *
 * ND_ALLOC_FAST asks for internal memory for scratch that every row reads
 * -- 54 KB for an MLP's weights -- and does not check the answer. Alone on
 * a chip it always got it. Under RV-9 the internal heap is everybody's,
 * and the second sentence of a session was refused it and stored to
 * address zero. External memory is slower and is not a panic.
 */
#pragma once
#include <stddef.h>

void *rv9_stt_alloc_fast(size_t n);
void  rv9_stt_free(void *p);

#define ND_ALLOC_FAST(n) rv9_stt_alloc_fast(n)
#define ND_FREE_FAST(p)  rv9_stt_free(p)
