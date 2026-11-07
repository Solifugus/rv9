/*
 * What RV-9 adds to Flite, and what it needs from it that Flite's own
 * headers leave to a program's generated sources.
 */
#pragma once
#include <stddef.h>
#include "flite.h"

/* Opening `path` read-only reads these bytes; NULL forgets. flite_file.c. */
void rv9_flite_memfile(const char *path, void *buf, size_t len);

/* US English and the CMU lexicon: the "eng" language a voice file names. */
void usenglish_init(cst_voice *v);
cst_lexicon *cmulex_init(void);
