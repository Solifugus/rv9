/*
 * RV-9 processes.
 *
 * A process is a module image, a private static storage area, a task, and a
 * scheduling policy. The module image is shared -- forking the same module
 * twice yields two processes running one copy of the code, which is exactly
 * what the reentrancy rules in rv9/module.h exist to make safe.
 *
 * SCHEDULING
 *
 * The KAL gives us tasks with settable priorities and nothing more. The
 * policy is RV-9's own: priority with aging, so that a low-priority process
 * cannot be starved indefinitely by a busy high-priority one. OS-9 did this
 * and it is the reason its shell stayed responsive under load.
 *
 * Effective priority = base + age, capped. A process that runs has its age
 * reset; processes that are ready but not running age upward until they
 * outrank whatever is hogging the CPU, run, and fall back. The result is a
 * sawtooth: everyone progresses, urgent work still dominates.
 *
 * In phase 7 the native scheduler implements this directly instead of
 * steering FreeRTOS priorities from above. The policy does not change.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rv9/kal.h"
#include "rv9/module.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t rv9_pid_t;

#define RV9_PID_NONE 0

/*
 * Priority hierarchy. Three bands, and the ordering is load-bearing:
 *
 *   RV9_PRIO_AGER    the ager. Must outrank everything it manages, or it
 *                    can be starved by the very processes it exists to
 *                    rescue -- and then nothing gets rescued.
 *   RV9_PRIO_SYSTEM  kernel-side tasks that orchestrate processes. Must
 *                    outrank user processes, or forking a busy child
 *                    preempts the forker before it can create its sibling.
 *   <= RV9_PROC_PRIO_CEILING
 *                    user processes, including any aging boost.
 *
 * Learned the hard way: with the orchestrator below its children, a child
 * ran to completion before its sibling was created, and the scheduler
 * demo silently measured one process running alone.
 */
#define RV9_PRIO_AGER          RV9_PRIO_MAX
#define RV9_PRIO_SYSTEM        (RV9_PRIO_MAX - 1)
#define RV9_PROC_PRIO_CEILING  (RV9_PRIO_MAX - 2)

/* Paths a process may hold open at once. */
#define RV9_MAX_PATHS 8

/*
 * What kind of work a process is.
 *
 * The distinction is not importance, it is consequence. An ordinary
 * process that runs late is slow; a real-time process that runs late is
 * wrong. They are scheduled by different mechanisms for that reason --
 * see rv9/kal.h.
 */
typedef enum {
    RV9_CLASS_NORMAL = 0,
    RV9_CLASS_REALTIME,
} rv9_proc_class_t;

typedef enum {
    RV9_PROC_ACTIVE = 1,   /* runnable or running */
    RV9_PROC_WAITING,      /* blocked on something */
    RV9_PROC_EXITED,       /* finished; exit status available */
} rv9_proc_state_t;

/* Signal bits live in rv9/module.h -- they are part of the module ABI. */

typedef struct rv9_proc {
    rv9_pid_t         pid;
    rv9_pid_t         parent;
    char              name[32];

    rv9_mod_entry_t  *module;
    void             *statics;
    rv9_task_t        task;
    rv9_proc_class_t  cls;
    uint32_t          period_us;      /* real-time processes only */

    int               base_priority;
    int               age;              /* aging counter, 0 when running */
    int               effective_priority;

    rv9_proc_state_t  state;
    int               exit_status;
    uint32_t          signals;          /* pending, cleared when taken */

    uint64_t          started_ms;

    char              arg[64];        /* what fork was given, for env->arg */

    /* Set by rv9_proc_chain; acted on when the module returns. */
    char              chain_to[32];
    bool              chain_pending;

    struct rv9_proc  *next;
} rv9_proc_t;

typedef enum {
    RV9_PROC_OK = 0,
    RV9_PROC_ERR_NOTFOUND,
    RV9_PROC_ERR_NOMEM,
    RV9_PROC_ERR_MODULE,
    RV9_PROC_ERR_TIMEOUT,
    RV9_PROC_ERR_INVAL,
} rv9_proc_err_t;

const char *rv9_proc_strerror(rv9_proc_err_t err);

/* Starts the ager. Call once, after the module directory is up. */
rv9_proc_err_t rv9_proc_init(void);

/*
 * Create a process from a module. Takes a link on the module, allocates and
 * zeroes its static storage, and starts it.
 */
rv9_proc_err_t rv9_proc_fork(const char *module_name, int priority,
                             const char *arg, rv9_pid_t *out_pid);

/*
 * Fork into the real-time class.
 *
 * The process is scheduled preemptively above everything else and released
 * by a hardware timer, so its latency does not depend on the behaviour of
 * any other process. It must declare its period and wait on it; a
 * real-time process that never waits is simply the highest-priority
 * busy loop in the system, which is a way to stop a machine.
 */
rv9_proc_err_t rv9_proc_fork_rt(const char *module_name, uint32_t period_us,
                                const char *arg, rv9_pid_t *out_pid);

/* Block until a process exits. timeout_ms may be RV9_WAIT_FOREVER. */
rv9_proc_err_t rv9_proc_wait(rv9_pid_t pid, int *out_status, uint32_t timeout_ms);

/* Post signals to a process. It sees them next time it asks. */
rv9_proc_err_t rv9_proc_signal(rv9_pid_t pid, uint32_t signals);

/*
 * Ask the calling process to continue as a different module, keeping its
 * pid, priority and open paths. Takes effect when the current module
 * returns from its entry point.
 */
rv9_proc_err_t rv9_proc_chain(const char *module_name);

const rv9_proc_t *rv9_proc_get(rv9_pid_t pid);

/*
 * A process's private storage, or NULL once it has exited and the storage
 * has been released. The kernel owns this memory, so reading it is legal
 * here in a way it will not be from another process once phase 7 adds PMP
 * isolation. Used by the scheduler demonstration to sample progress while
 * a process is still running.
 */
const void *rv9_proc_statics(rv9_pid_t pid);
const rv9_proc_t *rv9_proc_next(const rv9_proc_t *prev);   /* NULL to start */

/*
 * Aging can be turned off, which is useful for exactly one thing:
 * demonstrating that without it, low-priority work starves.
 */
void rv9_proc_aging_set(bool enabled);
bool rv9_proc_aging_get(void);

/* The pid of the calling task, or RV9_PID_NONE if the caller is not a
   process (the boot task, the ager, a driver's own task). */
rv9_pid_t rv9_proc_current_pid(void);

/*
 * Hooks, so that the I/O manager can hang per-process state off processes
 * without the process manager having to know the I/O manager exists. The
 * dependency runs one way: rv9_io knows about rv9_proc, never the reverse.
 */
typedef void (*rv9_proc_fork_hook_t)(rv9_pid_t parent, rv9_pid_t child);
typedef void (*rv9_proc_exit_hook_t)(rv9_pid_t pid);

void rv9_proc_set_hooks(rv9_proc_fork_hook_t on_fork,
                        rv9_proc_exit_hook_t on_exit);

#ifdef __cplusplus
}
#endif
