#pragma once
#include <stdbool.h>

/* Exercises device ownership against the claim table: sharing, exclusion,
   upgrades, fork-time reservations, and what a process's death does and
   does not release. True if every check passed. */
bool rv9_io_selftest(void);
