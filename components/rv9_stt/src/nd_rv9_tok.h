/*
 * The tokenizer's tables, in external memory. Forced into nd_tokenizer.c
 * alone, with -include, so the vendored file stays byte-for-byte.
 *
 * It builds three of them with calloc at load -- the pieces, a hash table
 * and a marker per piece -- and for Whistle's vocabulary that is 160 KB of
 * the chip's internal memory, held for as long as the model is: more than
 * every program RV-9 was running put together. They are read when tokens
 * are turned back into text, which is microseconds a sentence. External
 * memory is plenty fast for that.
 */
#pragma once
#include <stddef.h>
#include <stdlib.h>

void *rv9_stt_calloc_ext(size_t n, size_t size);
void  rv9_stt_free(void *p);

#define calloc(n, s) rv9_stt_calloc_ext((n), (s))
#define free(p)      rv9_stt_free(p)
