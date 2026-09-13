#pragma once
#include <stdbool.h>

/* How long a process is remembered: forking many leaves the table bounded
   and the heap where it was, collected processes are forgotten before
   uncollected ones, nothing is forgotten while it is waited on, and pids
   stay unique across the sixteen-bit wrap. True if every check passed. */
bool rv9_proc_selftest(void);
