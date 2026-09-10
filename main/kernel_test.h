#pragma once
#include <stdbool.h>

/* Exercises the native kernel: context switching, priority, aging.
   Returns true if every check passed. */
bool rv9_kernel_selftest(void);
