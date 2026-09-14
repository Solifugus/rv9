/*
 * Shared between the KAL's own source files. Not part of the KAL contract:
 * nothing above the seam may include this.
 */
#pragma once

#include "rv9/kernel.h"

/*
 * The RV-9 thread the *caller* is, or NULL if the caller is not one.
 *
 * rv9k_self() cannot answer this. It returns the kernel's current thread,
 * which stays set for as long as that thread is running -- and a real-time
 * process is a host task that preempts the kernel's host task mid-thread.
 * Ask rv9k_self() from there and you are told you are the shell.
 *
 * That is not a cosmetic confusion. It hands the caller another process's
 * identity: its pid, its path table, its place in the lock's priority
 * inheritance. A real-time process that borrowed the shell's cached path
 * table and then exited left the shell reading freed memory, which showed
 * up as the kernel jumping into the middle of a string.
 *
 * Only the KAL can tell, because only the KAL knows which host task the
 * kernel runs in. The kernel itself stays ignorant of hosts, which is the
 * arrangement worth keeping.
 */
rv9k_thread_t *rv9_kal_self_thread(void);

/* kal_rt.c: keeps esp_timer's task timers alive; see there. Idempotent. */
void rv9_kal_timer_guard_start(void);
