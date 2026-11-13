/*
 * Keeping well, unattended: the boot record, the count of bad ends, and
 * safe mode. See health.c and design section 82.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Early in boot, after the log is started and before /f0's modules are
   loaded: record this boot and decide whether it is in safe mode. */
bool rv9_health_boot(void);

/* Whether this run is in safe mode. */
bool rv9_health_safe(void);

/* Once /f0 is mounted: keep the previous run's log on /f0 if it ended
   badly, and say how it ended. */
void rv9_health_keep_blackbox(void);

/* From init's loop: proving the run healthy, and safe mode's way out. */
void rv9_health_tick(void);

/* RV9_SYS_HEALTH. */
int rv9_health_op(void *buf, uint32_t len);
