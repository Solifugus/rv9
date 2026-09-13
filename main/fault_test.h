#pragma once
#include <stdbool.h>

/* Stopping processes from outside and for missed deadlines: a thread
   holding a lock is not stopped, a process that ignores RV9_SIG_STOP is
   killed, a real-time loop is killed between activations, and a loop that
   misses a deadline it declared fatal is stopped with its failsafe
   applied. True if every check passed. */
bool rv9_fault_selftest(void);
