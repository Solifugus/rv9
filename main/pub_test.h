#pragma once
#include <stdbool.h>

/* Exercises publication cells through the I/O manager: declaring one,
   first publication, the sequence, one writer and many readers, a value
   that outlives its publisher, short buffers, waiting, the directory, and
   removal. True if every check passed. */
bool rv9_pub_selftest(void);
