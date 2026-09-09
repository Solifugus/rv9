#pragma once
#include <stdbool.h>

/* Exercises every KAL primitive. Returns true if all checks passed.
   In phase 7 this becomes the conformance test the native kernel must
   satisfy -- same tests, different backend. */
bool rv9_kal_selftest(void);
