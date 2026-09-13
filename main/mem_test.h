#pragma once
#include <stdbool.h>

/* Memory classes and budgets: ordinary allocation stops short of the
   real-time reserve; a real-time loop is still admitted and a failsafe
   still applied after ordinary memory is gone; a program that forks
   without end stops at its budget and gives it back. True if every check
   passed. */
bool rv9_mem_selftest(void);
