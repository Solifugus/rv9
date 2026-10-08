/*
 * Shared between the KAL's own source files. Not part of the KAL contract:
 * nothing above the seam may include this.
 */
#pragma once

#include "rv9/kernel.h"

#include "sdkconfig.h"

/*
 * WHICH CORE RV-9 RUNS ON. One, and it was measured rather than assumed.
 *
 * RV-9's scheduler has one run queue, one s_current, and mutual exclusion
 * in rv9_kernel that is "interrupts off on this core" -- irq_save() in
 * sched.c, in half a dozen places, with rv9k_priority_boost not guarded at
 * all. Sound on one core and meaningless across two. So every task RV-9
 * creates is pinned together, including its real-time tasks.
 *
 * The P4's second core was turned on and measured: a 2 ms control loop with
 * a 3 ms deadline, run while 2 MB came down over the radio, answered with
 * 40 us of worst jitter on two cores and 43 us on one. No difference, and
 * 11 KB of internal memory for the privilege. The reason is that the second
 * core can only move ESP-IDF's work, and ESP-IDF's work here is not
 * CPU-hungry -- the radio tops out near 1 Mbit/s so lwIP is nearly idle,
 * and RV-9's real-time tasks outrank all of it anyway. What competes for
 * cycles is RV-9's own work, and none of that can move until the mutual
 * exclusion above becomes a real lock.
 *
 * So this is deliberately still 0, and the pinning is explicit rather than
 * incidental: a task left unpinned on a future dual-core build would
 * wander, and the assumption it would break is not written down anywhere
 * it could be noticed.
 */
#define RV9_CORE  0

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
