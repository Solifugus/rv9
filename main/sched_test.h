#pragma once
#include <stdbool.h>

/* Derived real-time priority: a fast loop beside a heavy one meets its
   deadlines, and without the derivation is stopped for one the scheduler
   missed; admission refuses a set that fits the CPU but not the deadlines
   and places one that fits. True if every check passed. */
bool rv9_sched_selftest(void);
