#pragma once

#include "rv9/io.h"

/* The `stt` driver: Whistle behind /stt0. See src/drv_stt.c. */
rv9_io_err_t rv9_drv_stt_register(void);
