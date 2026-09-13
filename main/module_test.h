#pragma once
#include <stdbool.h>

/* Exercises the module manifest against images built in memory, including
   the malformed ones mkmodule.py cannot produce. True if every check
   passed. */
bool rv9_module_selftest(void);
