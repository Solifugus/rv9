/*
 * RV-9 process manager.
 *
 * See rv9/proc.h for the scheduling model. The short version: the KAL gives
 * us priorities, RV-9 supplies the aging policy on top of them.
 */
#include "rv9/proc.h"

#include <stdio.h>
#include <string.h>

/* Modules learn about fork failures as these, negated; the values are ABI
   and declared separately in rv9/module.h, which a module may include and
   this header may not be. */
_Static_assert((int)RV9_PROC_ERR_NOTFOUND == RV9_PE_NOTFOUND, "ABI drift");
_Static_assert((int)RV9_PROC_ERR_NOMEM    == RV9_PE_NOMEM,    "ABI drift");
_Static_assert((int)RV9_PROC_ERR_MODULE   == RV9_PE_MODULE,   "ABI drift");
_Static_assert((int)RV9_PROC_ERR_TIMEOUT  == RV9_PE_TIMEOUT,  "ABI drift");
_Static_assert((int)RV9_PROC_ERR_INVAL    == RV9_PE_INVAL,    "ABI drift");
_Static_assert((int)RV9_PROC_ERR_FAULT    == RV9_PE_FAULT,    "ABI drift");
_Static_assert((int)RV9_PROC_ERR_NOSLOT   == RV9_PE_NOSLOT,   "ABI drift");
_Static_assert((int)RV9_PROC_ERR_CONTRACT == RV9_PE_CONTRACT, "ABI drift");
_Static_assert((int)RV9_PROC_ERR_UTILISATION == RV9_PE_UTILISATION, "ABI drift");
_Static_assert((int)RV9_PROC_ERR_NODEV    == RV9_PE_NODEV,    "ABI drift");
_Static_assert((int)RV9_PROC_ERR_BUSY     == RV9_PE_BUSY,     "ABI drift");
_Static_assert((int)RV9_PROC_ERR_KILLED   == RV9_PE_KILLED,   "ABI drift");
_Static_assert((int)RV9_PROC_ERR_DEADLINE == RV9_PE_DEADLINE, "ABI drift");
_Static_assert((int)RV9_PROC_ERR_RUNAWAY  == RV9_PE_RUNAWAY,  "ABI drift");
_Static_assert((int)RV9_PROC_ERR_NOPUB    == RV9_PE_NOPUB,    "ABI drift");
_Static_assert((int)RV9_PROC_ERR_UNSCHEDULABLE == RV9_PE_UNSCHEDULABLE,
               "ABI drift");
_Static_assert((int)RV9_PROC_ERR_BUDGET   == RV9_PE_BUDGET,   "ABI drift");

/* The kernel's fault codes travel straight into the process table. */
_Static_assert(RV9_TASK_FAULT_STACK  == RV9_FAULT_STACK,  "ABI drift");
_Static_assert(RV9_TASK_FAULT_KILLED == RV9_FAULT_KILLED, "ABI drift");

#include "esp_log.h"

static const char *TAG = "rv9-proc";

/* Aging parameters.
 *
 * A process at RV9_PRIO_LOW (4) must be able to climb past one at
 * RV9_PRIO_HIGH (12), so AGE_MAX has to exceed that gap. The ceiling stays
 * below RV9_PRIO_MAX so the ager itself always preempts what it manages --
 * an ager that can be starved is not an ager.
 */
#define AGE_PERIOD_MS   20
#define AGE_MAX         10
#define PRIO_CEILING    RV9_PROC_PRIO_CEILING

/*
 * Interrupts land on whichever stack is current.
 *
 * Under RV-9's own kernel a process runs on a heap stack the host knows
 * nothing about, and the host's interrupt frames -- WiFi's especially --
 * are pushed onto it like anyone else's. A stack sized for the process's
 * own needs is not sized for that, and the failure is a corrupted saved
 * context: the thread resumes into a null return address, a long way from
 * whatever actually overflowed.
 */
#define PROC_DEFAULT_STACK 8192

static rv9_proc_t *s_procs;
static rv9_lock_t s_lock;
static rv9_pid_t   s_next_pid = 1;
static bool        s_aging = true;
static bool        s_running;

static rv9_proc_fork_hook_t  s_on_fork;
static rv9_proc_exit_hook_t  s_on_exit;
static rv9_proc_claim_hook_t s_on_claim;
static rv9_proc_ended_hook_t s_on_ended;

void rv9_proc_set_ended_hook(rv9_proc_ended_hook_t hook)
{
    s_on_ended = hook;
}

/* Undo whatever admission reserved for a process that will not run. Both
   hooks, because reservations inside file managers are released by the
   second. */
static void undo_fork(rv9_pid_t pid)
{
    if (s_on_exit)  s_on_exit(pid);
    if (s_on_ended) s_on_ended(pid, RV9_FAULT_NONE);
}

/* And its memory charge, for a refusal before the descriptor holds it. */
struct charge;
static void refuse_fork(rv9_pid_t pid, const struct charge *c);

void rv9_proc_set_hooks(rv9_proc_fork_hook_t on_fork,
                        rv9_proc_exit_hook_t on_exit)
{
    s_on_fork = on_fork;
    s_on_exit = on_exit;
}

void rv9_proc_set_claim_hook(rv9_proc_claim_hook_t hook)
{
    s_on_claim = hook;
}

const char *rv9_proc_strerror(rv9_proc_err_t err)
{
    switch (err) {
    case RV9_PROC_OK:           return "ok";
    case RV9_PROC_ERR_NOTFOUND: return "no such process";
    case RV9_PROC_ERR_NOMEM:    return "out of memory";
    case RV9_PROC_ERR_MODULE:   return "module error";
    case RV9_PROC_ERR_TIMEOUT:  return "timed out";
    case RV9_PROC_ERR_INVAL:    return "invalid argument";
    case RV9_PROC_ERR_FAULT:    return "stopped by the scheduler";
    case RV9_PROC_ERR_NOSLOT:   return "no real-time slot free";
    case RV9_PROC_ERR_CONTRACT: return "its declaration contradicts itself";
    case RV9_PROC_ERR_UTILISATION: return "the CPU is already promised";
    case RV9_PROC_ERR_NODEV:    return "it needs a device this machine lacks";
    case RV9_PROC_ERR_BUSY:     return "a device it needs alone is owned";
    case RV9_PROC_ERR_KILLED:   return "killed";
    case RV9_PROC_ERR_DEADLINE: return "missed its deadline";
    case RV9_PROC_ERR_RUNAWAY:  return "stopped waiting for its releases";
    case RV9_PROC_ERR_NOPUB:    return "it watches something nothing publishes";
    case RV9_PROC_ERR_UNSCHEDULABLE:
        return "no placement meets every real-time deadline";
    case RV9_PROC_ERR_BUDGET:   return "over its memory budget";
    default:                    return "unknown error";
    }
}

static rv9_proc_t *find_locked(rv9_pid_t pid)
{
    for (rv9_proc_t *p = s_procs; p; p = p->next) {
        if (p->pid == pid) return p;
    }
    return NULL;
}

/* Which process is the caller? Used by the environment callbacks, which run
   on the process's own task. */
static rv9_proc_t *current_locked(void)
{
    rv9_task_t self = rv9_task_self();
    for (rv9_proc_t *p = s_procs; p; p = p->next) {
        if (p->task == self) return p;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* How long a process is remembered                                    */
/* ------------------------------------------------------------------ */

/*
 * A descriptor outlives its process so that somebody can still ask how it
 * ended. For a long time nothing ever decided that nobody would ask again,
 * and every process that had ever run kept one: about 250 bytes each, for
 * as long as the machine was up. A shell session that forked a few dozen
 * commands was enough to leave `procs` unable to start, and on a machine
 * meant to run for months that is the wrong shape entirely.
 *
 * So the dead are remembered in a bounded history, and forgotten in an
 * order that costs the least:
 *
 *   beyond PROC_HISTORY, a process whose status has been collected, or
 *   whose parent has gone and so never will collect it, oldest first;
 *
 *   beyond PROC_HISTORY_MAX, any exited process, oldest first -- a
 *   background job nobody waits on is still remembered for a while, but
 *   not for ever.
 *
 * Nothing is forgotten while anybody holds it (refs), and a waiter holds
 * what it waits on. Pruning happens when a process is forked, which is the
 * only thing that makes the table grow.
 */
#define PROC_HISTORY      16
#define PROC_HISTORY_MAX  32

static uint32_t s_ended_seq;

static void hold_locked(rv9_proc_t *p) { p->refs++; }

static void put(rv9_proc_t *p)
{
    rv9_lock_acquire(s_lock);
    if (p->refs > 0) p->refs--;
    rv9_lock_release(s_lock);
}

/* Give an ended process's footprint back to everyone it was charged to. */
static void uncharge_locked(const uint32_t *ancestors, int n, uint32_t bytes)
{
    for (int i = 0; i < n; i++) {
        for (rv9_proc_t *q = s_procs; q; q = q->next) {
            if (q->serial != ancestors[i]) continue;
            q->held = (q->held > bytes) ? q->held - bytes : 0;
            break;
        }
    }
}

static void ended_locked(rv9_proc_t *p)
{
    p->state     = RV9_PROC_EXITED;
    p->ended_seq = ++s_ended_seq;

    /* Once, and on every way out: every ending passes through here. */
    uncharge_locked(p->ancestors, p->n_ancestors, p->footprint);
    p->n_ancestors = 0;
    p->held = 0;
}

/* The system itself (pid 0) counts as a parent that may yet ask. */
static bool parent_may_wait_locked(const rv9_proc_t *p)
{
    if (p->parent == RV9_PID_NONE) return true;
    const rv9_proc_t *q = find_locked(p->parent);
    return q != NULL && q->state != RV9_PROC_EXITED;
}

static void prune_locked(void)
{
    for (;;) {
        unsigned exited = 0;
        rv9_proc_t *oldest_any = NULL, *oldest_done = NULL;

        for (rv9_proc_t *p = s_procs; p; p = p->next) {
            if (p->state != RV9_PROC_EXITED) continue;
            exited++;
            if (p->refs != 0 || p->collecting) continue;

            if (oldest_any == NULL || p->ended_seq < oldest_any->ended_seq) {
                oldest_any = p;
            }
            if ((p->waited || !parent_may_wait_locked(p)) &&
                (oldest_done == NULL ||
                 p->ended_seq < oldest_done->ended_seq)) {
                oldest_done = p;
            }
        }

        rv9_proc_t *victim = NULL;
        if (exited > PROC_HISTORY_MAX)  victim = oldest_any;
        else if (exited > PROC_HISTORY) victim = oldest_done;
        if (victim == NULL) return;

        for (rv9_proc_t **pp = &s_procs; *pp; pp = &(*pp)->next) {
            if (*pp == victim) { *pp = victim->next; break; }
        }
        rv9_free(victim);
    }
}

/*
 * The next free pid.
 *
 * Sixteen bits is 65535 forks, which a machine running a job every second
 * spends in eighteen hours. Wrapping is fine; handing out a number that
 * still names a process -- running, or remembered -- is not, because a
 * wait or a kill would then land on the wrong one. So a pid in use is
 * skipped, and 0 is never issued.
 */
static rv9_pid_t next_pid_locked(void)
{
    for (uint32_t tries = 0; tries <= 0xFFFFu; tries++) {
        rv9_pid_t pid = s_next_pid;
        s_next_pid = (s_next_pid >= 0xFFFFu) ? 1 : (rv9_pid_t)(s_next_pid + 1);
        if (pid != RV9_PID_NONE && find_locked(pid) == NULL) return pid;
    }
    return RV9_PID_NONE;
}

/* ------------------------------------------------------------------ */
/* Budgets                                                             */
/* ------------------------------------------------------------------ */

/*
 * How much memory a process, and everything it starts, may hold.
 *
 * Modules cannot allocate: everything a process costs is spent on its
 * behalf by RV-9, and nearly all of it at fork -- the stack, the statics,
 * the descriptor. So a process's footprint is known before it runs, and
 * charging it to the process and to every ancestor turns "a program that
 * forks without end" into "a program that stops at its own budget", while
 * the programs beside it keep working.
 *
 * Budgets nest. A shell's includes the commands it runs; `sshd`'s includes
 * each session's shell and whatever that starts. The default is sized so a
 * shell can hold its largest ordinary command -- `ed`, 17 KB with its
 * statics -- with room left; a program that needs more says so with
 * RV9_MTAG_MEM_MAX.
 *
 * Only the nearest eight ancestors are charged. Deeper than that is not a
 * tree anyone has built here, and a fixed array is what keeps accounting
 * off the heap it is accounting for.
 */
#define PROC_BUDGET_DEFAULT  32768
#define PROC_ANCESTORS       8

/* What RV-9 keeps about a process besides its stack and statics: the
   descriptor, its path table, and the allocator's own headers. */
#define PROC_OVERHEAD        (sizeof(rv9_proc_t) + 96)

static uint32_t s_next_serial = 1;

typedef struct charge {
    uint32_t serial;
    uint32_t bytes;
    uint32_t ancestors[PROC_ANCESTORS];
    uint8_t  n;
} charge_t;

static void refuse_fork(rv9_pid_t pid, const struct charge *c)
{
    rv9_lock_acquire(s_lock);
    uncharge_locked(c->ancestors, c->n, c->bytes);
    rv9_lock_release(s_lock);
    undo_fork(pid);
}

/*
 * Charge a new process to the process forking it and that one's ancestors.
 * Refuses, charging nothing, if any of them -- or the new process itself --
 * would go over budget. `whose` names the one that would.
 */
static bool charge_locked(charge_t *c, uint32_t bytes, uint32_t budget,
                          char *whose, size_t whose_len)
{
    memset(c, 0, sizeof(*c));
    c->bytes = bytes;

    if (bytes > budget) {
        snprintf(whose, whose_len, "its own");
        return false;
    }

    rv9_proc_t *parent = current_locked();
    if (parent != NULL && parent->state != RV9_PROC_EXITED) {
        c->ancestors[c->n++] = parent->serial;
        for (int i = 0; i < parent->n_ancestors && c->n < PROC_ANCESTORS; i++) {
            c->ancestors[c->n++] = parent->ancestors[i];
        }
    }

    for (int i = 0; i < c->n; i++) {
        for (rv9_proc_t *q = s_procs; q; q = q->next) {
            if (q->serial != c->ancestors[i]) continue;
            if (q->state != RV9_PROC_EXITED && q->held + bytes > q->budget) {
                snprintf(whose, whose_len, "pid %u ('%s')'s",
                         (unsigned)q->pid, q->name);
                return false;
            }
            break;
        }
    }

    for (int i = 0; i < c->n; i++) {
        for (rv9_proc_t *q = s_procs; q; q = q->next) {
            if (q->serial == c->ancestors[i]) { q->held += bytes; break; }
        }
    }

    c->serial = s_next_serial++;
    return true;
}

/* ------------------------------------------------------------------ */
/* The environment handed to a process's module                        */
/* ------------------------------------------------------------------ */

/* Real-time services, refused to processes that are not in that class:
   declaring a period from an ordinary process would promise a guarantee
   the scheduler underneath it cannot make. */
/*
 * Zero means "the period I was admitted at".
 *
 * The rate a control law runs at is a property of the control law, so it
 * belongs in the module's manifest rather than in a constant the module
 * carries and an operator retypes. fork_common has already resolved it --
 * from what the caller asked for, or failing that from the manifest -- and
 * this is how the module reads it back rather than inventing its own.
 */
/*
 * Hold the declared task to its deadline, and say how seriously.
 *
 * With no deadline declared the period is the deadline, which is the usual
 * meaning of a periodic task and costs nothing to check. A deadline longer
 * than the interval actually declared -- possible when a module overrides
 * the period it was admitted at -- is cut to the interval, because work
 * still running when the next release arrives is late whatever was written
 * down.
 */
static void hold_to_deadline(const char *name, uint32_t deadline,
                             uint32_t interval, bool fatal)
{
    if (deadline == 0 || (interval != 0 && deadline > interval)) {
        deadline = interval;
    }
    (void)rv9_rt_deadline(deadline, fatal);

    if (fatal && deadline != 0) {
        ESP_LOGI(TAG, "'%s' is held to %lu us: a miss stops it", name,
                 (unsigned long)deadline);
    } else if (fatal) {
        ESP_LOGW(TAG, "'%s' asks that a missed deadline stop it, but "
                      "declares none and has no bound on its releases",
                 name);
    }
}

static int env_rt_declare(uint32_t period_us)
{
    uint32_t deadline = 0;
    bool fatal = false;
    char name[32] = "";
    bool urgent = true;
    uint32_t bound = 0;

    rv9_lock_acquire(s_lock);
    rv9_proc_t *p = current_locked();
    bool ok = (p != NULL && p->cls == RV9_CLASS_REALTIME);
    if (ok) {
        if (period_us == 0) period_us = p->period_us;
        else                p->period_us = period_us;
        deadline = p->deadline_us;
        fatal    = (p->on_deadline == RV9_ON_DEADLINE_FAULT);
        memcpy(name, p->name, sizeof(name));
        urgent   = p->rt_urgent;
        bound    = p->rt_bound_us;
    }
    rv9_lock_release(s_lock);

    if (!ok) return -1;

    /* Nobody said, and the module did not declare one. A real-time process
       with no period is not a real-time process. */
    if (period_us == 0) return -4;

    if (rv9_rt_declare(period_us) != RV9_OK) return -2;

    hold_to_deadline(name, deadline, period_us, fatal);

    /* Record the placement admission made, now there is a slot to hold
       it; the priority was already set when the task was made. */
    rv9_rt_set_class(rv9_task_self(), urgent, bound);
    return 0;
}

/*
 * Released by an event instead of a period.
 *
 * The process manager never learns which device produced the event -- it
 * takes a number and hands it to the KAL. That is what keeps rv9_proc from
 * having to depend on rv9_io: the module does the introduction, asking the
 * device for its event id through getstat and passing it here.
 */
static int env_rt_declare_event(int event_id, uint32_t min_interval_us)
{
    uint32_t deadline = 0;
    bool fatal = false;
    char name[32] = "";
    bool urgent = true;
    uint32_t bound = 0;

    rv9_lock_acquire(s_lock);
    rv9_proc_t *p = current_locked();
    bool ok = (p != NULL && p->cls == RV9_CLASS_REALTIME);
    if (ok) {
        p->period_us = min_interval_us;
        deadline = p->deadline_us;
        fatal    = (p->on_deadline == RV9_ON_DEADLINE_FAULT);
        memcpy(name, p->name, sizeof(name));
        urgent   = p->rt_urgent;
        bound    = p->rt_bound_us;
    }
    rv9_lock_release(s_lock);

    if (!ok) return -1;

    rv9_event_t ev = rv9_event_by_id(event_id);
    if (ev == NULL) return -3;      /* nothing armed, or a stale id */

    if (rv9_rt_declare_event(ev, min_interval_us) != RV9_OK) return -2;

    hold_to_deadline(name, deadline, min_interval_us, fatal);
    rv9_rt_set_class(rv9_task_self(), urgent, bound);
    return 0;
}

static void end_here(int why);

/*
 * Two answers from the KAL are not for the module.
 *
 * A missed deadline the program declared fatal, and a request from outside
 * to stop, both mean the same thing here: there is no next activation.
 * Returning either to the loop would hand the decision to the code that
 * has just been judged, so the process ends inside this call and the
 * module never sees it return.
 */
static RV9_RT_CODE int env_rt_wait(void)
{
    int r = rv9_rt_wait();
    if (r == RV9_RT_DEADLINE || r == RV9_RT_STOPPED || r == RV9_RT_RUNAWAY) {
        end_here(r);
    }
    return r;
}

static int env_rt_stats(rv9_rt_report_t *out)
{
    if (out == NULL) return -1;

    rv9_rt_stats_t st;
    if (rv9_rt_stats(&st) != RV9_OK) return -1;

    out->period_us     = st.period_us;
    out->activations   = (uint32_t)st.activations;
    out->overruns      = (uint32_t)st.overruns;
    out->max_jitter_us = st.max_jitter_us;
    out->max_exec_us   = st.max_exec_us;
    out->last_exec_us  = st.last_exec_us;
    return 0;
}

static uint32_t env_signals_take(void)
{
    rv9_preempt_point();

    rv9_lock_acquire(s_lock);
    rv9_proc_t *p = current_locked();
    uint32_t sig = 0;
    if (p) {
        sig = p->signals;
        p->signals = 0;
    }
    rv9_lock_release(s_lock);
    return sig;
}

/* ------------------------------------------------------------------ */
/* Process body                                                        */
/* ------------------------------------------------------------------ */

/*
 * The end of a process, on its own task, however it came to end.
 *
 * The order is R9 §15.1's and it is a requirement: the release source goes
 * first, so nothing can run another activation; the exit hook second,
 * which closes paths and applies failsafes; and only then does the process
 * table say it has stopped and why. Anything watching the table and
 * reacting to a fault must find the actuators already parked.
 *
 * Does not return.
 */
static void finish(rv9_proc_t *p, int rc, int fault,
                   const rv9_rt_stats_t *timing)
{
    if (p->cls == RV9_CLASS_REALTIME) rv9_rt_release();

    /* Let the I/O manager close whatever this process left open, before we
       mark it dead and someone waiting on it wakes up. */
    if (s_on_exit) s_on_exit(p->pid);

    rv9_lock_acquire(s_lock);
    p->exit_status = rc;
    p->fault       = fault;
    ended_locked(p);
    rv9_lock_release(s_lock);

    if (fault == RV9_FAULT_DEADLINE && timing != NULL) {
        ESP_LOGE(TAG, "pid %u ('%s') missed its %lu us deadline: answered "
                      "in %lu, %lu releases skipped; stopped and not "
                      "released again", (unsigned)p->pid, p->name,
                 (unsigned long)timing->deadline_us,
                 (unsigned long)timing->last_response_us,
                 (unsigned long)timing->overruns);
        if (timing->overruns > 0) {
            ESP_LOGE(TAG, "pid %u ('%s') worst stall: its release interrupt "
                          "ran %lu us late, then it waited %lu us for the CPU",
                     (unsigned)p->pid, p->name,
                     (unsigned long)timing->stall_isr_late_us,
                     (unsigned long)timing->stall_sched_late_us);
        }
    } else if (fault == RV9_FAULT_DEADLINE) {
        ESP_LOGE(TAG, "pid %u ('%s') missed its deadline; stopped",
                 (unsigned)p->pid, p->name);
    } else if (fault == RV9_FAULT_RUNAWAY) {
        ESP_LOGE(TAG, "pid %u ('%s') came back to wait only after the "
                      "watchdog had flagged it; stopped",
                 (unsigned)p->pid, p->name);
    } else if (fault == RV9_FAULT_KILLED) {
        ESP_LOGW(TAG, "pid %u ('%s') killed between activations",
                 (unsigned)p->pid, p->name);
    }

    /* Third in R9's order: published, now that the table says it. */
    if (s_on_ended) s_on_ended(p->pid, fault);

    /*
     * How deep this stack went, measured here because nothing later can:
     * the stack goes with the task. Last of the work done on it, so the
     * exit hook, the failsafes and the publication are counted -- they
     * ran on this stack and a stack cut from the figure must hold them.
     */
    size_t size = 0, unused = 0;
    uint32_t used = 0;
    if (rv9_task_stack(rv9_task_self(), &size, &unused) == RV9_OK) {
        if (size == 0) size = p->stack_bytes;      /* a host task: see stacks */
        used = (size > unused) ? (uint32_t)(size - unused) : 0;
        rv9_mod_note_stack(p->module, (uint32_t)size, used);
    }
    if (fault == RV9_FAULT_NONE) {
        ESP_LOGI(TAG, "pid %u ('%s') exited, status %d, stack %lu of %u",
                 (unsigned)p->pid, p->name, rc, (unsigned long)used,
                 (unsigned)size);
    }

    /* Release the module link and the private storage. The descriptor stays
       so that a parent can still wait on it and see the status -- for a
       while: see "How long a process is remembered". */
    rv9_mod_unlink(p->module);
    rv9_free(p->statics);
    p->statics = NULL;

    /* The task's own hold, last: nothing below touches the descriptor. */
    put(p);

    rv9_mem_task_forget(rv9_task_self());
    rv9_task_delete(NULL);
}

/*
 * A real-time process ending inside rt_wait.
 *
 * Deep in the module's call stack, which is fine: nothing returns into it.
 * The stack goes with the task, and the module's code is not needed to
 * leave -- the release, the paths and the failsafes are all RV-9's.
 *
 * R9 §15.4: this never runs the program's own cleanup. A component that
 * missed a deadline is not trusted with more code, and one that was killed
 * was asked first and did not stop.
 */
static void end_here(int why)
{
    rv9_lock_acquire(s_lock);
    rv9_proc_t *p = current_locked();
    rv9_lock_release(s_lock);

    if (p == NULL) {                /* not a process; nothing to record */
        rv9_rt_release();
        rv9_task_delete(NULL);
        return;
    }

    if (why == RV9_RT_DEADLINE) {
        rv9_rt_stats_t st;
        bool have = (rv9_rt_stats(&st) == RV9_OK);
        finish(p, -RV9_PROC_ERR_DEADLINE, RV9_FAULT_DEADLINE,
               have ? &st : NULL);
    } else if (why == RV9_RT_RUNAWAY) {
        finish(p, -RV9_PROC_ERR_RUNAWAY, RV9_FAULT_RUNAWAY, NULL);
    } else {
        finish(p, -RV9_PROC_ERR_KILLED, RV9_FAULT_KILLED, NULL);
    }
}

static void proc_trampoline(void *arg)
{
    rv9_proc_t *p = (rv9_proc_t *)arg;

    /*
     * Claim our own identity before doing anything else.
     *
     * A process is recognised by matching task handles, and fork() cannot
     * record the handle until rv9_task_create returns -- by which time the
     * child may already be running. In that window the child has no pid, so
     * its path table lookups miss and every write fails. Setting it here,
     * from inside the task itself, closes the window: whoever asks is asking
     * after this line has run. fork() writes the same value again later,
     * which is harmless.
     */
    rv9_lock_acquire(s_lock);
    p->task = rv9_task_self();
    rv9_lock_release(s_lock);

    /* A real-time loop's set-up may spend the real-time reserve. An
       ordinary process allocates as ordinary, which is every thread's
       default. */
    if (p->cls == RV9_CLASS_REALTIME) rv9_mem_class_set(RV9_MEM_REALTIME);

    /*
     * A process may outlive the module it started as: chain() swaps the
     * module underneath it, keeping the pid, priority and open paths. So
     * this is a loop, not a single call.
     */
    int rc = 0;
    for (;;) {
    const rv9_mod_header_t *h = (const rv9_mod_header_t *)p->module->image;

    /* One place builds the environment, so a module cannot tell whether it
       was started by rv9_mod_run or by fork. Signals are the exception --
       they are per-process, so the process manager supplies its own. */
    rv9_mod_env_t env;
    rv9_mod_env_init(&env, p->statics, h->static_size, p->pid);
    env.signals_take = env_signals_take;
    env.rt_declare   = env_rt_declare;
    env.rt_declare_event = env_rt_declare_event;
    env.rt_wait      = env_rt_wait;
    env.rt_stats     = env_rt_stats;
    env.arg          = p->arg[0] ? p->arg : NULL;

    rc = p->module->entry(&env);

    rv9_lock_acquire(s_lock);
    bool chaining = p->chain_pending;
    char next_name[32];
    if (chaining) {
        memcpy(next_name, p->chain_to, sizeof(next_name));
        p->chain_pending = false;
    }
    rv9_lock_release(s_lock);

    if (!chaining) break;

    rv9_mod_entry_t *next = NULL;
    if (rv9_mod_link(next_name, &next) != RV9_MOD_OK) {
        ESP_LOGE(TAG, "pid %u: cannot chain to '%s'",
                 (unsigned)p->pid, next_name);
        rc = -1;
        break;
    }

    /*
     * The module changes; the promise does not.
     *
     * Chaining keeps the pid, so whatever this process already owns stays
     * owned -- claiming it again is a second reference to the same record
     * and costs nothing. What must not happen is a module arriving through
     * the back door with declarations nobody checked: a program could
     * otherwise start as something harmless and continue as something that
     * wants the actuator.
     */
    if (s_on_claim != NULL) {
        int refused = s_on_claim(p->pid, next->image, next_name);
        if (refused != RV9_PROC_OK) {
            ESP_LOGE(TAG, "pid %u: refused to chain to '%s': %s",
                     (unsigned)p->pid, next_name,
                     rv9_proc_strerror((rv9_proc_err_t)refused));
            rv9_mod_unlink(next);
            rc = -refused;
            break;
        }
    }

    /* Swap the module out. Paths stay open, which is the point. */
    rv9_mod_unlink(p->module);
    rv9_free(p->statics);
    p->statics = NULL;

    const rv9_mod_header_t *nh = (const rv9_mod_header_t *)next->image;
    if (nh->static_size > 0) {
        p->statics = rv9_calloc(1, nh->static_size);
        if (p->statics == NULL) {
            rv9_mod_unlink(next);
            rc = -1;
            break;
        }
    }

    rv9_lock_acquire(s_lock);
    p->module = next;
    strncpy(p->name, next_name, sizeof(p->name) - 1);
    rv9_lock_release(s_lock);

    ESP_LOGI(TAG, "pid %u chained to '%s'", (unsigned)p->pid, next_name);
    }

    /* A real-time process gives its period back in finish(), or the next
       one cannot declare: the timer and its release semaphore belong to
       the slot, not to the module that borrowed it. */
    finish(p, rc, RV9_FAULT_NONE, NULL);
}

/* ------------------------------------------------------------------ */
/* Collecting the killed                                               */
/* ------------------------------------------------------------------ */

/*
 * A process normally ends by returning through proc_trampoline, which is
 * where its paths are closed, its module released and its status recorded.
 *
 * A task the scheduler killed -- for running off the bottom of its stack,
 * so far the only way -- never returns, so none of that happens. Its paths
 * stay open, its module stays linked, and anybody in wait() waits forever
 * for a status that will never be written. The kernel cannot do the
 * clearing up itself: it is below the seam, it has never heard of a
 * process, and calling up into this layer from inside reschedule() would
 * take the process lock from the scheduler, which is exactly the deadlock
 * we spent an evening on.
 *
 * So the funeral is held by a passer-by. Anybody who looks at a process --
 * a waiter, the ager -- may notice the task is gone and finish the job.
 * Whoever gets there first claims it with `collecting` and does the work
 * outside the lock, because closing paths reaches into the I/O manager and
 * the two locks must never be held in the same order twice.
 */
static bool claim_faulted_locked(rv9_proc_t *p)
{
    if (p->state == RV9_PROC_EXITED || p->collecting) return false;
    if (p->task == NULL || rv9_task_alive(p->task))   return false;

    /* Not p->fault yet: the reason is published after the failsafes, and
       the corpse keeps it until then. */
    p->collecting = true;
    return true;
}

static void collect_faulted(rv9_proc_t *p)
{
    int fault = rv9_task_fault(p->task);

    if (p->cls == RV9_CLASS_REALTIME) rv9_rt_release();
    if (s_on_exit) s_on_exit(p->pid);

    rv9_mod_entry_t *mod = p->module;
    void            *st  = p->statics;
    rv9_task_t       tk  = p->task;

    rv9_lock_acquire(s_lock);
    p->statics = NULL;
    p->task    = NULL;
    /* Negative, and not a status any module returns, so a parent can tell
       "it failed" from "it succeeded and returned zero". */
    p->exit_status = (fault == RV9_FAULT_KILLED) ? -RV9_PROC_ERR_KILLED
                                                 : -RV9_PROC_ERR_FAULT;
    p->fault       = fault ? fault : RV9_FAULT_STACK;
    ended_locked(p);
    p->collecting  = false;
    rv9_lock_release(s_lock);

    if (fault == RV9_FAULT_KILLED) {
        ESP_LOGW(TAG, "pid %u ('%s') killed", (unsigned)p->pid, p->name);
    } else {
        ESP_LOGE(TAG, "pid %u ('%s') killed: %s", (unsigned)p->pid, p->name,
                 fault == RV9_FAULT_STACK ? "stack overflow" : "faulted");
    }

    if (s_on_ended) s_on_ended(p->pid, p->fault);

    if (mod) rv9_mod_unlink(mod);
    rv9_free(st);
    rv9_task_reap(tk);      /* the slot may be reused now */

    /* The dead task's hold, dropped on its behalf. The caller's own hold,
       if it has one, is the caller's to drop. */
    put(p);
}

/* Sweep every process. Called from places that are already looking. */
static void collect_faulted_all(void)
{
    for (;;) {
        rv9_proc_t *victim = NULL;

        rv9_lock_acquire(s_lock);
        for (rv9_proc_t *p = s_procs; p; p = p->next) {
            if (claim_faulted_locked(p)) { victim = p; break; }
        }
        rv9_lock_release(s_lock);

        if (victim == NULL) return;
        collect_faulted(victim);
    }
}

/* ------------------------------------------------------------------ */
/* A real-time process that will not come to wait                      */
/* ------------------------------------------------------------------ */

/*
 * Called by the KAL's watchdog, on its own task, for a real-time process
 * flagged while still inside an activation: past a deadline it declared
 * fatal, or holding the CPU without waiting at all.
 *
 * The KAL decides *whether it can be touched* -- only when it is in its
 * own module's code, and so holding nothing. The process manager decides
 * what follows, and it is finish() from outside: releases gone, the exit
 * hook's failsafes, then the table. The task is deleted before its module
 * is unlinked, since its program counter is in that module.
 *
 * Returns false to be asked again, which is what happens while the process
 * is in the middle of a system call: it has been lowered below everything
 * that matters, and will be stopped as soon as it is back in its own code,
 * or will end itself at rt_wait if that is where it goes instead.
 */
static bool rt_overrun(rv9_task_t task, int why)
{
    rv9_proc_t *p = NULL;
    const void *code = NULL;
    size_t len = 0;
    bool noted = false;
    uint64_t since = 0;

    rv9_lock_acquire(s_lock);
    for (rv9_proc_t *q = s_procs; q; q = q->next) {
        if (q->task == task && q->cls == RV9_CLASS_REALTIME &&
            q->state != RV9_PROC_EXITED) {
            p = q;
            break;
        }
    }
    if (p != NULL) hold_locked(p);
    if (p != NULL && p->module != NULL) {
        code  = p->module->image;
        len   = p->module->size;
        noted = p->overrun_noted;
        if (p->overrun_since_ms == 0) p->overrun_since_ms = rv9_time_ms();
        since = p->overrun_since_ms;
    }
    rv9_lock_release(s_lock);

    if (p == NULL) return true;     /* not a process; nothing of ours to do */

    /*
     * Lowered only once being late has become something worse.
     *
     * The first version lowered every flagged loop it could not seize, and
     * the boot log showed what that costs: `lateloop`, 3 ms past its
     * deadline in the middle of reading the clock, dropped to idle priority
     * and finished its last milliseconds -- and reached its failsafe --
     * whenever nothing else wanted the CPU. A loop flagged for its deadline
     * is usually about to come to wait and end itself. One still going a
     * runaway's worth of time later is a runaway, whatever it was flagged
     * for.
     */
    bool lower = (why == RV9_RT_RUNAWAY) ||
                 (rv9_time_ms() - since >= RV9_RT_RUNAWAY_MS);

    rv9_err_t err = rv9_rt_seize(task, code, len, lower);
    if (err == RV9_ERR_BUSY) {
        if (lower && !noted) {
            rv9_lock_acquire(s_lock);
            p->overrun_noted = true;
            rv9_lock_release(s_lock);
            ESP_LOGW(TAG, "pid %u ('%s') %s, inside a system call: lowered, "
                          "and stopped when it is back in its own code",
                     (unsigned)p->pid, p->name,
                     (why == RV9_RT_DEADLINE)
                         ? "is far past its deadline and still running"
                         : "has held the CPU without waiting");
        }
        put(p);
        return false;
    }
    if (err != RV9_OK) {                /* it ended itself meanwhile */
        put(p);
        return true;
    }

    rv9_rt_stats_t st;
    bool have = (rv9_rt_stats_for(task, &st) == RV9_OK);

    rv9_rt_release_task(task);
    if (s_on_exit) s_on_exit(p->pid);

    int fault  = (why == RV9_RT_DEADLINE) ? RV9_FAULT_DEADLINE
               : (why == RV9_RT_RUNAWAY)  ? RV9_FAULT_RUNAWAY
               :                            RV9_FAULT_KILLED;
    int status = (why == RV9_RT_DEADLINE) ? -RV9_PROC_ERR_DEADLINE
               : (why == RV9_RT_RUNAWAY)  ? -RV9_PROC_ERR_RUNAWAY
               :                            -RV9_PROC_ERR_KILLED;

    rv9_lock_acquire(s_lock);
    rv9_mod_entry_t *mod = p->module;
    void *statics        = p->statics;
    p->statics     = NULL;
    p->task        = NULL;
    p->exit_status = status;
    p->fault       = fault;
    ended_locked(p);
    rv9_lock_release(s_lock);

    if (why == RV9_RT_DEADLINE) {
        ESP_LOGE(TAG, "pid %u ('%s') %s %lu us deadline: stopped from "
                      "outside, not released again", (unsigned)p->pid,
                 p->name, "was still running past its",
                 (unsigned long)(have ? st.deadline_us : 0));
    } else {
        ESP_LOGE(TAG, "pid %u ('%s') held the CPU for %d ms without "
                      "waiting: stopped from outside, not released again",
                 (unsigned)p->pid, p->name, RV9_RT_RUNAWAY_MS);
    }

    if (s_on_ended) s_on_ended(p->pid, fault);

    rv9_mem_task_forget(task);
    rv9_task_delete(task);
    if (mod) rv9_mod_unlink(mod);
    rv9_free(statics);

    put(p);      /* ours */
    put(p);      /* and the deleted task's, which it cannot drop itself */
    return true;
}

/* ------------------------------------------------------------------ */
/* Aging                                                               */
/* ------------------------------------------------------------------ */

static void ager_task(void *arg)
{
    (void)arg;

    for (;;) {
        rv9_task_delay_ms(AGE_PERIOD_MS);

        /* The ager is the one thing that looks at every process on a
           timer, so it is where a killed process with nobody waiting on
           it gets collected. */
        collect_faulted_all();

        if (!s_aging) continue;

        rv9_lock_acquire(s_lock);

        /* Whoever currently ranks highest is the one getting the CPU, so it
           is the one whose age we reset. Everyone else climbs. */
        rv9_proc_t *top = NULL;
        for (rv9_proc_t *p = s_procs; p; p = p->next) {
            if (p->state != RV9_PROC_ACTIVE) continue;
            if (top == NULL || p->effective_priority > top->effective_priority) {
                top = p;
            }
        }

        for (rv9_proc_t *p = s_procs; p; p = p->next) {
            if (p->state != RV9_PROC_ACTIVE) continue;

            if (p == top) {
                p->age = 0;
            } else if (p->age < AGE_MAX) {
                p->age++;
            }

            int eff = p->base_priority + p->age;
            if (eff > PRIO_CEILING) eff = PRIO_CEILING;

            if (eff != p->effective_priority) {
                p->effective_priority = eff;
                rv9_task_priority_set(p->task, eff);
            }
        }

        rv9_lock_release(s_lock);
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Adapter so a module can start other modules without rv9_module having to
   know the process manager exists. */
static int proc_fork_op(const char *module, int priority)
{
    rv9_pid_t pid = 0;
    rv9_proc_err_t err = rv9_proc_fork(module, priority, NULL, &pid);
    return (err == RV9_PROC_OK) ? (int)pid : -(int)err;
}

static int proc_wait_op(int pid, int *status, uint32_t timeout_ms)
{
    rv9_proc_err_t err = rv9_proc_wait((rv9_pid_t)pid, status, timeout_ms);
    return (err == RV9_PROC_OK) ? 0 : -(int)err;
}

/* Fill rv9_sys_proc_t records. buf==NULL just counts, which is how
   sysinfo(RV9_SYS_MEM) learns the process count. */
static int proc_list_op(void *buf, uint32_t len)
{
    rv9_lock_acquire(s_lock);

    uint32_t max = buf ? len / sizeof(rv9_sys_proc_t) : 0;
    rv9_sys_proc_t *out = (rv9_sys_proc_t *)buf;
    uint32_t n = 0;

    for (rv9_proc_t *p = s_procs; p; p = p->next) {
        if (buf == NULL) { n++; continue; }
        if (n >= max) break;

        memset(&out[n], 0, sizeof(out[n]));
        out[n].pid                = p->pid;
        out[n].parent             = p->parent;
        strncpy(out[n].name, p->name, sizeof(out[n].name) - 1);
        out[n].state              = (uint8_t)p->state;
        out[n].base_priority      = (int8_t)p->base_priority;
        out[n].effective_priority = (int8_t)p->effective_priority;
        out[n].status             = p->exit_status;
        out[n].fault              = (uint8_t)p->fault;
        n++;
    }

    rv9_lock_release(s_lock);
    return (int)n;
}

static int proc_fork_arg_op(const char *module, int priority, const char *arg)
{
    rv9_pid_t pid = 0;
    rv9_proc_err_t err = rv9_proc_fork(module, priority, arg, &pid);
    return (err == RV9_PROC_OK) ? (int)pid : -(int)err;
}

static int proc_fork_rt_op(const char *module, uint32_t period_us,
                           const char *arg)
{
    rv9_pid_t pid = 0;
    rv9_proc_err_t err = rv9_proc_fork_rt(module, period_us, arg, &pid);
    return (err == RV9_PROC_OK) ? (int)pid : -(int)err;
}

static int proc_chain_op(const char *module)
{
    return rv9_proc_chain(module) == RV9_PROC_OK ? 0 : -1;
}

/*
 * Stacks, per live process.
 *
 * Only the living: a dead process has released its stack, and reporting a
 * measurement of memory that no longer exists would be worse than
 * reporting nothing.
 */
static int proc_stacks_op(void *buf, uint32_t len)
{
    rv9_lock_acquire(s_lock);

    uint32_t max = buf ? len / sizeof(rv9_sys_stack_t) : 0;
    rv9_sys_stack_t *out = (rv9_sys_stack_t *)buf;
    uint32_t n = 0;

    for (rv9_proc_t *p = s_procs; p; p = p->next) {
        if (p->state == RV9_PROC_EXITED || p->task == NULL) continue;
        if (buf == NULL) { n++; continue; }
        if (n >= max) break;

        size_t size = 0, unused = 0;
        rv9_task_stack(p->task, &size, &unused);

        /* The KAL answers 0 for a host task, which a real-time process
           is. We know what we asked for, so say that rather than nothing. */
        if (size == 0) size = p->stack_bytes;

        memset(&out[n], 0, sizeof(out[n]));
        out[n].pid          = (uint16_t)p->pid;
        strncpy(out[n].name, p->name, sizeof(out[n].name) - 1);
        out[n].stack_size   = (uint32_t)size;
        out[n].stack_unused = (uint32_t)unused;
        n++;
    }

    rv9_lock_release(s_lock);
    return (int)n;
}

/* One record, filled to whatever length the caller knew about. */
static int proc_rt_load_op(void *buf, uint32_t len)
{
    if (buf == NULL || len < sizeof(uint32_t) * 2) return -1;

    rv9_proc_rt_load_t l;
    rv9_proc_rt_load(&l);

    rv9_sys_admit_t a = {
        .used_permille    = l.used_permille,
        .ceiling_permille = l.ceiling_permille,
        .declared         = l.declared,
        .measured         = l.measured,
        .unaccounted      = l.unaccounted,
        .slots_used       = l.slots_used,
        .slots_total      = l.slots_total,
    };

    memcpy(buf, &a, len < sizeof(a) ? len : sizeof(a));
    return 1;
}

static int proc_signal_op(int pid, uint32_t signals)
{
    if (pid <= 0) return -RV9_PE_INVAL;
    return rv9_proc_signal((rv9_pid_t)pid, signals) == RV9_PROC_OK
           ? 0 : -RV9_PE_NOTFOUND;
}

static int proc_kill_op(int pid)
{
    if (pid <= 0) return -RV9_PE_INVAL;
    rv9_proc_err_t err = rv9_proc_kill((rv9_pid_t)pid);
    return (err == RV9_PROC_OK) ? 0 : -(int)err;
}

/* The limits a toolchain may plan against, read from where each lives. */
static int proc_limits_op(void *buf, uint32_t len)
{
    if (buf == NULL || len < sizeof(uint32_t)) return -1;

    rv9_rt_limits_t rt;
    rv9_rt_limits(&rt);

    rv9_proc_rt_load_t load;
    rv9_proc_rt_load(&load);

    rv9_sys_limits_t l;
    memset(&l, 0, sizeof(l));
    l.module_abi               = RV9_MODULE_ABI;
    l.rt_slots                 = rt.slots;
    l.rt_util_ceiling_permille = load.ceiling_permille;
    l.rt_runaway_ms            = rt.runaway_ms;
    l.rt_watchdog_us           = rt.watchdog_us;
    l.prio_urgent              = (uint8_t)rt.prio_urgent;
    l.prio_routine             = (uint8_t)rt.prio_routine;
    l.prio_radio               = (uint8_t)rt.prio_radio;
    l.prio_kernel              = (uint8_t)rt.prio_kernel;
    l.heap_floor               = (uint32_t)rv9_heap_floor();
    l.proc_history             = PROC_HISTORY;
    l.proc_history_max         = PROC_HISTORY_MAX;
    l.max_paths                = RV9_MAX_PATHS;
    l.rt_reserve               = (uint32_t)rv9_heap_rt_reserve();
    l.budget_default           = PROC_BUDGET_DEFAULT;

    memcpy(buf, &l, len < sizeof(l) ? len : sizeof(l));
    return 1;
}

/* Every live process's charge and budget, or a count with no buffer. */
static int proc_budgets_op(void *buf, uint32_t len)
{
    uint32_t max = buf ? len / sizeof(rv9_sys_budget_t) : 0;
    rv9_sys_budget_t *out = (rv9_sys_budget_t *)buf;
    int n = 0;

    rv9_lock_acquire(s_lock);
    for (rv9_proc_t *p = s_procs; p; p = p->next) {
        if (p->state == RV9_PROC_EXITED) continue;
        if (out != NULL && (uint32_t)n < max) {
            memset(&out[n], 0, sizeof(out[n]));
            out[n].pid       = p->pid;
            out[n].parent    = p->parent;
            strncpy(out[n].name, p->name, sizeof(out[n].name) - 1);
            out[n].footprint = p->footprint;
            out[n].held      = p->held;
            out[n].budget    = p->budget;
        }
        n++;
    }
    rv9_lock_release(s_lock);
    return n;
}

static const rv9_mod_proc_ops_t s_mod_proc_ops = {
    .budgets = proc_budgets_op,
    .limits = proc_limits_op,
    .signal = proc_signal_op,
    .kill   = proc_kill_op,
    .fork  = proc_fork_op,
    .wait  = proc_wait_op,
    .procs = proc_list_op,
    .stacks = proc_stacks_op,
    .chain = proc_chain_op,
    .fork_arg = proc_fork_arg_op,
    .fork_rt  = proc_fork_rt_op,
    .rt_load  = proc_rt_load_op,
};

rv9_proc_err_t rv9_proc_init(void)
{
    if (s_running) return RV9_PROC_OK;

    if (rv9_lock_create(&s_lock) != RV9_OK) return RV9_PROC_ERR_NOMEM;

    /*
     * Only run an ager if the scheduler underneath does not age for us.
     *
     * This one exists because FreeRTOS schedules strictly by priority.
     * RV-9's own kernel implements the same policy directly, and running
     * both means each undoes the other: setting a priority resets the age
     * the kernel just applied.
     */
    if (!rv9_sched_ages()) {
        rv9_err_t err = rv9_task_create(ager_task, "rv9-ager", 2560, NULL,
                                        RV9_PRIO_AGER, NULL);
        if (err != RV9_OK) return RV9_PROC_ERR_NOMEM;
    } else {
        ESP_LOGI(TAG, "the kernel ages threads itself; no ager needed");
    }

    rv9_mod_set_proc_ops(&s_mod_proc_ops);
    rv9_rt_set_overrun_handler(rt_overrun);

    s_running = true;
    ESP_LOGI(TAG, "process manager up (aging every %d ms, max boost %d)",
             AGE_PERIOD_MS, AGE_MAX);
    return RV9_PROC_OK;
}

rv9_pid_t rv9_proc_current_pid(void)
{
    if (s_lock == NULL) return RV9_PID_NONE;

    rv9_lock_acquire(s_lock);
    rv9_proc_t *p = current_locked();
    rv9_pid_t pid = p ? p->pid : RV9_PID_NONE;
    rv9_lock_release(s_lock);
    return pid;
}

/* ------------------------------------------------------------------ */
/* Admission                                                           */
/* ------------------------------------------------------------------ */

/*
 * How much of the CPU real-time work may promise itself.
 *
 * This is not a schedulability theorem, and saying so matters: whether
 * every loop meets its deadline is decided by the response-time analysis
 * below, which a set using 8% of the CPU can fail. What this headroom is
 * for is everything that is not in either sum at all: WiFi, the panel, the
 * SPI driver, RV-9's own kernel and every ordinary process. Admitting
 * real-time work up to the last percent starves the system the real-time
 * work depends on.
 */
#define RT_UTIL_MAX_PERMILLE 700

/*
 * Utilisation in parts per thousand. There is no floating point worth
 * spending here, and a control loop's duty is a number like "26
 * microseconds in every 1000" -- 26 permille -- which integer arithmetic
 * reports exactly.
 */
static uint32_t permille(uint32_t cost_us, uint32_t interval_us)
{
    if (interval_us == 0) return 0;
    uint64_t u = ((uint64_t)cost_us * 1000u) / interval_us;
    return (u > 1000u) ? 1000u : (uint32_t)u;
}

/*
 * What the already-admitted real-time work costs.
 *
 * Three kinds of answer, and they are not the same kind of thing:
 *
 *   declared  the module said, and the sum is a promise being kept.
 *   measured  it did not say, but it has run, so the worst execution seen
 *             so far stands in. That is a floor on its true worst case,
 *             never a bound, and admission must not pretend otherwise.
 *   unknown   it did not say and has not run. Nothing can be counted.
 *
 * Keeping the three apart is the point. A total of "18%" hides whether
 * the other 82% is actually free.
 */
static void rt_load(uint32_t *out_permille, uint32_t *out_declared,
                    uint32_t *out_measured, uint32_t *out_unknown)
{
    uint32_t total = 0, declared = 0, measured = 0, unknown = 0;

    rv9_lock_acquire(s_lock);
    for (rv9_proc_t *p = s_procs; p; p = p->next) {
        if (p->state == RV9_PROC_EXITED)  continue;
        if (p->cls != RV9_CLASS_REALTIME) continue;

        if (p->period_us != 0 && p->wcet_us != 0) {
            total += permille(p->wcet_us, p->period_us);
            declared++;
            continue;
        }

        rv9_rt_stats_t st;
        if (p->period_us != 0 && p->task != NULL &&
            rv9_rt_stats_for(p->task, &st) == RV9_OK && st.activations > 0) {
            total += permille(st.max_exec_us, p->period_us);
            measured++;
        } else {
            unknown++;
        }
    }
    rv9_lock_release(s_lock);

    if (out_permille) *out_permille = total;
    if (out_declared) *out_declared = declared;
    if (out_measured) *out_measured = measured;
    if (out_unknown)  *out_unknown  = unknown;
}

/* ------------------------------------------------------------------ */
/* Priority, derived                                                   */
/* ------------------------------------------------------------------ */

/*
 * Which real-time task outranks which.
 *
 * R9 §13 asks that nobody choose these numbers: RV-9 should derive them
 * from the periods, deadlines and execution bounds of everything admitted.
 * Until this existed every real-time task ran at one host priority and
 * time-sliced with the others, which was harmless while a late loop was
 * only counted and is not now that a late loop can be stopped. A 1 kHz
 * loop with a tight deadline, released while a slow loop was in the middle
 * of 20 ms of work, waited for the next tick to share the CPU -- and was
 * stopped for a deadline the scheduler had missed on its behalf.
 *
 * The host offers two useful priorities (see rv9_task_create_rt), so the
 * derivation is a placement, not a ranking:
 *
 *   everything starts urgent. A workload whose urgent tasks all meet their
 *   deadlines together stays there, which is every workload of one loop.
 *
 *   while some urgent task cannot, the least urgent of them -- the longest
 *   deadline -- moves to routine, where the urgent ones no longer wait on
 *   it.
 *
 *   then everything routine must meet its deadline too, with every urgent
 *   task and every other routine one counted against it. Tasks sharing a
 *   priority time-slice, so each is charged for all of its peers: the
 *   analysis is pessimistic there on purpose.
 *
 * Meeting a deadline is decided by response-time analysis:
 *
 *     R = C + sum over tasks j that can run ahead of it of ceil(R/T_j) C_j
 *
 * iterated to a fixed point or until it passes the deadline. C is the
 * declared worst-case execution, or the worst yet measured when nothing
 * was declared -- a floor, as rt_load says. Tasks with no release bound
 * (an event source with no minimum interval) cannot be analysed: they stay
 * urgent, count against nobody, and are reported as unaccounted.
 *
 * What the analysis does not see is the host's own work. Routine tasks run
 * under the radio, and their bounds are bounds on RV-9's workload only.
 */
#define RT_MAX_ANALYSED 8

typedef struct {
    rv9_proc_t *p;          /* NULL for the task being admitted */
    uint32_t    c, t, d;
    bool        bounded;    /* has a release interval, so can be analysed */
    bool        urgent;
    uint8_t     pin;        /* RV9_PLACE_*: DERIVED may be moved, others not */
    uint32_t    bound;
} rt_item_t;

static bool s_rt_derive = true;

void rv9_proc_rt_derive(bool on) { s_rt_derive = on; }

static uint32_t response_bound(const rt_item_t *items, int n, int i)
{
    const rt_item_t *me = &items[i];
    uint64_t r = me->c;

    for (int iter = 0; iter < 64; iter++) {
        uint64_t next = me->c;
        for (int j = 0; j < n; j++) {
            const rt_item_t *o = &items[j];
            if (j == i || !o->bounded) continue;
            if (me->urgent && !o->urgent) continue;   /* below it: no wait */
            uint64_t window = (r == 0) ? 1 : r;
            next += ((window + o->t - 1) / o->t) * o->c;
        }
        if (next == r) break;
        r = next;
        if (r > me->d) break;
    }
    return (r > UINT32_MAX) ? UINT32_MAX : (uint32_t)r;
}

/* Every bounded task at this level meets its deadline. Fills bounds; on
   failure leaves the first that does not in *bad. */
static bool level_meets(rt_item_t *items, int n, bool urgent, int *bad)
{
    for (int i = 0; i < n; i++) {
        if (!items[i].bounded || items[i].urgent != urgent) continue;
        items[i].bound = response_bound(items, n, i);
        if (items[i].bound > items[i].d) {
            if (bad) *bad = i;
            return false;
        }
    }
    return true;
}

/* Place every task. Returns the index of one that cannot meet its
   deadline under any placement this makes, or -1. */
static int place_rt(rt_item_t *items, int n)
{
    for (int i = 0; i < n; i++) {
        items[i].urgent = (items[i].pin != RV9_PLACE_ROUTINE);
    }

    int bad = -1;
    while (!level_meets(items, n, true, &bad)) {
        int victim = -1, urgent_bounded = 0;
        for (int i = 0; i < n; i++) {
            if (!items[i].bounded || !items[i].urgent) continue;
            urgent_bounded++;
            /* A pin is kept, not weighed: if only pinned tasks could move,
               the placement fails and admission says so. */
            if (items[i].pin != RV9_PLACE_DERIVED) continue;
            if (victim < 0 || items[i].d > items[victim].d) victim = i;
        }
        /* One task alone that cannot meet its deadline will not meet it
           anywhere else either. */
        if (victim < 0 || urgent_bounded <= 1) return bad;
        items[victim].urgent = false;
    }

    if (!level_meets(items, n, false, &bad)) return bad;
    return -1;
}

/*
 * Decide whether the machine can honour what this program says it needs,
 * before anything is allocated on its behalf.
 *
 * The compiler describes; RV-9 decides. Everything checked here is
 * checkable cheaply and now, which is what makes it admission rather than
 * validation: a refusal costs nothing, and a control loop admitted when it
 * should not have been costs a deadline.
 *
 * `interval_us` may be zero. An event-driven process has no period to give
 * at fork -- it declares its minimum inter-arrival from inside, once it
 * knows which device is releasing it -- so a zero here is not a fault. It
 * means this process cannot be accounted for yet, which is reported rather
 * than assumed away.
 */
static rv9_proc_err_t admit_rt(const void *image, const char *name,
                               uint32_t interval_us, size_t stack,
                               uint32_t *out_wcet, uint32_t *out_deadline,
                               bool *out_urgent, uint32_t *out_bound,
                               uint8_t *out_place)
{
    uint8_t place = RV9_PLACE_DERIVED;
    (void)rv9_mod_manifest_u8(image, RV9_MTAG_PLACEMENT, &place);
    if (place > RV9_PLACE_ROUTINE) {
        ESP_LOGE(TAG, "admit '%s': placement %u is not one this system knows",
                 name, (unsigned)place);
        return RV9_PROC_ERR_CONTRACT;
    }

    if (out_place)  *out_place  = place;
    if (out_urgent) *out_urgent = (place != RV9_PLACE_ROUTINE);
    if (out_bound)  *out_bound  = 0;

    if (rv9_rt_slots_used() >= rv9_rt_slot_count()) {
        ESP_LOGE(TAG, "admit '%s': all %d real-time slots are taken",
                 name, rv9_rt_slot_count());
        return RV9_PROC_ERR_NOSLOT;
    }

    uint32_t deadline = 0, wcet = 0, heap_max = 0;
    bool has_deadline = rv9_mod_manifest_u32(image, RV9_MTAG_DEADLINE_US,
                                             &deadline);
    bool has_wcet     = rv9_mod_manifest_u32(image, RV9_MTAG_WCET_US, &wcet);
    bool has_heap     = rv9_mod_manifest_u32(image, RV9_MTAG_HEAP_MAX,
                                             &heap_max);

    if (out_wcet)     *out_wcet     = has_wcet     ? wcet     : 0;
    if (out_deadline) *out_deadline = has_deadline ? deadline : 0;

    /*
     * Check the claim against itself first. This is the cheap half of a
     * resource certificate: a compiler's arithmetic is not to be trusted
     * where trusting it costs nothing to avoid.
     */
    if (has_deadline && interval_us != 0 && deadline > interval_us) {
        ESP_LOGE(TAG, "admit '%s': deadline %lu us is longer than its "
                      "%lu us release interval", name,
                 (unsigned long)deadline, (unsigned long)interval_us);
        return RV9_PROC_ERR_CONTRACT;
    }
    if (has_wcet) {
        uint32_t finish_in = has_deadline ? deadline : interval_us;
        if (finish_in != 0 && wcet > finish_in) {
            ESP_LOGE(TAG, "admit '%s': %lu us of work cannot finish in "
                          "%lu us", name,
                     (unsigned long)wcet, (unsigned long)finish_in);
            return RV9_PROC_ERR_CONTRACT;
        }
    }

    /* Memory, before any is spent: the stack, plus whatever heap it says
       it will take. Asked against `available` rather than `free`, so
       real-time work is refused up front instead of starting and finding
       the reserve gone. */
    size_t wants = stack + (has_heap ? heap_max : 0);
    if (wants > rv9_heap_available()) {
        ESP_LOGE(TAG, "admit '%s': wants %u bytes, %u available",
                 name, (unsigned)wants, (unsigned)rv9_heap_available());
        return RV9_PROC_ERR_NOMEM;
    }

    uint32_t used = 0, declared = 0, measured = 0, unknown = 0;
    rt_load(&used, &declared, &measured, &unknown);

    uint32_t mine = (has_wcet && interval_us != 0)
                        ? permille(wcet, interval_us) : 0;

    if (used + mine > RT_UTIL_MAX_PERMILLE) {
        ESP_LOGE(TAG, "admit '%s': wants %lu permille, %lu already promised, "
                      "ceiling %d", name,
                 (unsigned long)mine, (unsigned long)used,
                 RT_UTIL_MAX_PERMILLE);
        return RV9_PROC_ERR_UTILISATION;
    }

    if (mine == 0) {
        ESP_LOGW(TAG, "admit '%s': admitted without an accountable cost -- "
                      "%s", name,
                 has_wcet ? "no release interval yet"
                          : "no declared execution time");
    }
    if (measured > 0 || unknown > 0) {
        ESP_LOGW(TAG, "real-time load %lu permille is a floor: %lu declared, "
                      "%lu measured, %lu unaccounted",
                 (unsigned long)(used + mine), (unsigned long)declared,
                 (unsigned long)measured, (unsigned long)unknown);
    }

    /*
     * Where it goes, and whether everything already admitted still meets
     * its deadlines with it there. Moving tasks that are already running
     * is part of admitting this one: the placement is of the whole set.
     */
    if (s_rt_derive && interval_us != 0) {
        rt_item_t items[RT_MAX_ANALYSED];
        int n = 0;

        rv9_lock_acquire(s_lock);
        for (rv9_proc_t *q = s_procs; q && n < RT_MAX_ANALYSED - 1; q = q->next) {
            if (q->state == RV9_PROC_EXITED || q->cls != RV9_CLASS_REALTIME) {
                continue;
            }
            rt_item_t *it = &items[n++];
            memset(it, 0, sizeof(*it));
            it->p       = q;
            it->pin     = q->rt_place;
            it->t       = q->period_us;
            it->bounded = (it->t != 0);
            it->d       = (q->deadline_us != 0 && q->deadline_us < it->t)
                          ? q->deadline_us : it->t;
            it->c       = q->wcet_us;
            if (it->c == 0 && q->task != NULL) {
                rv9_rt_stats_t st;
                if (rv9_rt_stats_for(q->task, &st) == RV9_OK) {
                    it->c = st.max_exec_us;
                }
            }
        }

        rt_item_t *me = &items[n++];
        memset(me, 0, sizeof(*me));
        me->t       = interval_us;
        me->bounded = true;
        me->d       = (has_deadline && deadline < interval_us) ? deadline
                                                               : interval_us;
        me->c       = has_wcet ? wcet : 0;
        me->pin     = place;

        bool pinned = false;
        for (int i = 0; i < n; i++) pinned |= (items[i].pin != RV9_PLACE_DERIVED);

        int bad = place_rt(items, n);
        if (bad >= 0) {
            const rt_item_t *b = &items[bad];
            char who[48];
            if (b->p == NULL) {
                snprintf(who, sizeof(who), "it");
            } else {
                snprintf(who, sizeof(who), "pid %u ('%s')",
                         (unsigned)b->p->pid, b->p->name);
            }
            rv9_lock_release(s_lock);

            ESP_LOGE(TAG, "admit '%s': with it admitted, %s would answer in "
                          "%lu us against a %lu us deadline, however the "
                          "real-time work is placed%s", name, who,
                     (unsigned long)b->bound, (unsigned long)b->d,
                     pinned ? " around the declared placements" : "");
            return RV9_PROC_ERR_UNSCHEDULABLE;
        }

        for (int i = 0; i < n - 1; i++) {
            rt_item_t *it = &items[i];
            bool moved = (it->p->rt_urgent != it->urgent);
            if (!moved && it->p->rt_bound_us == it->bound) continue;

            it->p->rt_urgent   = it->urgent;
            it->p->rt_bound_us = it->bound;
            if (it->p->task != NULL) {
                rv9_rt_set_class(it->p->task, it->urgent, it->bound);
            }
            if (moved) {
                ESP_LOGI(TAG, "pid %u ('%s') now runs %s, bound %lu us",
                         (unsigned)it->p->pid, it->p->name,
                         it->urgent ? "urgent" : "routine",
                         (unsigned long)it->bound);
            }
        }

        if (out_urgent) *out_urgent = me->urgent;
        if (out_bound)  *out_bound  = me->bound;
        rv9_lock_release(s_lock);

        ESP_LOGI(TAG, "admit '%s': %s%s, response bound %lu us against %lu",
                 name, me->urgent ? "urgent" : "routine",
                 place != RV9_PLACE_DERIVED ? " (declared)" : "",
                 (unsigned long)me->bound, (unsigned long)me->d);
    }

    ESP_LOGI(TAG, "admit '%s': %lu us interval, %lu permille, %lu of %d "
                  "promised", name,
             (unsigned long)interval_us, (unsigned long)mine,
             (unsigned long)(used + mine), RT_UTIL_MAX_PERMILLE);
    return RV9_PROC_OK;
}

/*
 * What real-time work the machine has promised, for anyone reporting it.
 */
void rv9_proc_rt_load(rv9_proc_rt_load_t *out)
{
    if (out == NULL) return;

    memset(out, 0, sizeof(*out));
    rt_load(&out->used_permille, &out->declared, &out->measured,
            &out->unaccounted);
    out->ceiling_permille = RT_UTIL_MAX_PERMILLE;
    out->slots_used       = (uint32_t)rv9_rt_slots_used();
    out->slots_total      = (uint32_t)rv9_rt_slot_count();
}

static rv9_proc_err_t fork_common(const char *module_name, int priority,
                                  const char *arg, rv9_proc_class_t cls,
                                  uint32_t period_us, rv9_pid_t *out_pid);

rv9_proc_err_t rv9_proc_fork(const char *module_name, int priority,
                             const char *arg, rv9_pid_t *out_pid)
{
    return fork_common(module_name, priority, arg, RV9_CLASS_NORMAL, 0,
                       out_pid);
}

rv9_proc_err_t rv9_proc_fork_rt(const char *module_name, uint32_t period_us,
                                const char *arg, rv9_pid_t *out_pid)
{
    /*
     * A period of zero is allowed, and means the process will say for
     * itself what releases it -- an event-driven one has no period to give
     * here. What fork_rt decides is the class, which is the part that has
     * to be settled before the task exists; the release source is the
     * process's own business and is declared from inside it.
     */
    return fork_common(module_name, RV9_PRIO_MAX, arg, RV9_CLASS_REALTIME,
                       period_us, out_pid);
}

static rv9_proc_err_t fork_inner(const char *module_name, int priority,
                                 const char *arg, rv9_proc_class_t cls,
                                 uint32_t period_us, rv9_pid_t *out_pid);

/*
 * Forking a real-time process spends the real-time reserve.
 *
 * Everything a child costs is allocated here, in the parent's context --
 * so it is the parent's memory class that would decide whether a control
 * loop can be admitted, and the parent is usually a shell. For a real-time
 * child the class is raised for the length of the fork, never lowered: a
 * system caller keeps what it had.
 */
static rv9_proc_err_t fork_common(const char *module_name, int priority,
                                  const char *arg, rv9_proc_class_t cls,
                                  uint32_t period_us, rv9_pid_t *out_pid)
{
    int prev = rv9_mem_class_get();
    bool raise = (cls == RV9_CLASS_REALTIME && prev < RV9_MEM_REALTIME);
    if (raise) rv9_mem_class_set(RV9_MEM_REALTIME);

    rv9_proc_err_t err = fork_inner(module_name, priority, arg, cls,
                                    period_us, out_pid);

    if (raise) rv9_mem_class_set(prev);
    return err;
}

static rv9_proc_err_t fork_inner(const char *module_name, int priority,
                                 const char *arg, rv9_proc_class_t cls,
                                 uint32_t period_us, rv9_pid_t *out_pid)
{
    if (module_name == NULL) return RV9_PROC_ERR_INVAL;

    rv9_mod_entry_t *mod = NULL;
    rv9_mod_err_t merr = rv9_mod_link(module_name, &mod);
    if (merr != RV9_MOD_OK) {
        ESP_LOGE(TAG, "fork '%s': %s", module_name, rv9_mod_strerror(merr));

        /* Out of room is not the same as unloadable, and the caller can
           only act sensibly on the difference: one says free something,
           the other says the module is wrong. */
        return (merr == RV9_MOD_ERR_NOMEM) ? RV9_PROC_ERR_NOMEM
                                           : RV9_PROC_ERR_MODULE;
    }

    const rv9_mod_header_t *h = (const rv9_mod_header_t *)mod->image;

    /*
     * What the module says about itself, where the caller did not say.
     *
     * A period belongs to the program, not to whoever typed its name: the
     * compiler knows the control law's rate and the shell does not. So
     * `rt <module>` with no period runs it at the rate the module
     * declared, and an explicit period still wins -- an operator
     * overriding a program's own figure is a deliberate act.
     */
    if (cls == RV9_CLASS_REALTIME && period_us == 0) {
        uint32_t declared = 0;
        if (rv9_mod_manifest_u32(mod->image, RV9_MTAG_PERIOD_US, &declared)) {
            period_us = declared;
        }
    }

    /* The manifest's figure is the compiler's; the header's is the build's.
       Either beats the 8 KB default, which is a guess nobody made. */
    size_t stack = h->stack_size;
    if (stack == 0) {
        uint32_t declared = 0;
        if (rv9_mod_manifest_u32(mod->image, RV9_MTAG_STACK, &declared)) {
            stack = declared;
        }
    }
    if (stack == 0) stack = PROC_DEFAULT_STACK;

    /*
     * The pid comes first, because admission needs somebody to admit.
     *
     * A device reserved from the manifest is reserved *for* a process, and
     * the reservation has to exist before the process does -- otherwise the
     * check and the claim are two separate moments and two programs can
     * both pass the check. So the number is issued here and the descriptor
     * is built around it later. A refusal spends a pid, which is the right
     * way round: the alternative is a claim with nothing to release it.
     */
    /* Its footprint, and what it may hold with everything it starts. */
    uint32_t footprint = (uint32_t)(stack + h->static_size + PROC_OVERHEAD);
    uint32_t budget = PROC_BUDGET_DEFAULT;
    (void)rv9_mod_manifest_u32(mod->image, RV9_MTAG_MEM_MAX, &budget);

    charge_t charge;
    char whose[48] = "";

    rv9_lock_acquire(s_lock);
    prune_locked();
    rv9_pid_t pid = next_pid_locked();
    bool charged = (pid != RV9_PID_NONE) &&
                   charge_locked(&charge, footprint, budget, whose,
                                 sizeof(whose));
    rv9_lock_release(s_lock);

    if (pid == RV9_PID_NONE) {      /* every pid names somebody */
        rv9_mod_unlink(mod);
        return RV9_PROC_ERR_NOMEM;
    }
    if (!charged) {
        ESP_LOGW(TAG, "fork '%s': %lu bytes would take it past %s budget",
                 module_name, (unsigned long)footprint, whose);
        rv9_mod_unlink(mod);
        return RV9_PROC_ERR_BUDGET;
    }

    /*
     * Admission, before anything is allocated on this program's behalf.
     *
     * Ordinary processes are not admitted for *timing*: they are late if
     * they are late, and nothing else depends on it. But every process is
     * admitted for what it says it must own, real-time or not -- two
     * programs driving one output is wrong at any priority, and the
     * scheduler has nothing to do with it.
     */
    if (s_on_claim != NULL) {
        int refused = s_on_claim(pid, mod->image, module_name);
        if (refused != RV9_PROC_OK) {
            refuse_fork(pid, &charge);      /* undo a partial claim */
            rv9_mod_unlink(mod);
            return (rv9_proc_err_t)refused;
        }
    }

    uint32_t wcet_us = 0, deadline_us = 0;
    uint8_t on_deadline = RV9_ON_DEADLINE_REPORT;
    bool rt_urgent = true;
    uint32_t rt_bound = 0;
    uint8_t rt_place = RV9_PLACE_DERIVED;
    if (cls == RV9_CLASS_REALTIME) {
        /*
         * What a miss means, read once here and not trusted beyond what is
         * understood. A value from the future is a policy RV-9 cannot
         * apply, and running the program under a different one -- most
         * likely the lenient one -- is agreeing to a contract nobody
         * offered.
         */
        if (rv9_mod_manifest_u8(mod->image, RV9_MTAG_ON_DEADLINE,
                                &on_deadline) &&
            on_deadline > RV9_ON_DEADLINE_FAULT) {
            ESP_LOGE(TAG, "admit '%s': on_deadline %u is not a policy this "
                          "system knows", module_name, (unsigned)on_deadline);
            refuse_fork(pid, &charge);
            rv9_mod_unlink(mod);
            return RV9_PROC_ERR_CONTRACT;
        }

        uint32_t interval = period_us;
        if (interval == 0) {
            (void)rv9_mod_manifest_u32(mod->image, RV9_MTAG_MIN_INTER_US,
                                       &interval);
        }

        rv9_proc_err_t adm = admit_rt(mod->image, module_name, interval,
                                      stack, &wcet_us, &deadline_us,
                                      &rt_urgent, &rt_bound, &rt_place);
        if (adm != RV9_PROC_OK) {
            refuse_fork(pid, &charge);
            rv9_mod_unlink(mod);
            return adm;
        }
    }

    rv9_proc_t *p = rv9_calloc(1, sizeof(*p));
    if (p == NULL) {
        refuse_fork(pid, &charge);
        rv9_mod_unlink(mod);
        return RV9_PROC_ERR_NOMEM;
    }

    if (h->static_size > 0) {
        p->statics = rv9_calloc(1, h->static_size);
        if (p->statics == NULL) {
            rv9_free(p);
            refuse_fork(pid, &charge);
            rv9_mod_unlink(mod);
            return RV9_PROC_ERR_NOMEM;
        }
    }

    rv9_pid_t parent = rv9_proc_current_pid();

    rv9_lock_acquire(s_lock);

    p->pid                = pid;
    p->parent             = parent;
    p->module             = mod;
    p->base_priority      = priority;
    p->age                = 0;
    p->effective_priority = priority;
    p->state              = RV9_PROC_ACTIVE;
    p->started_ms         = rv9_time_ms();
    p->cls                = cls;
    p->period_us          = period_us;
    p->wcet_us            = wcet_us;
    p->deadline_us        = deadline_us;
    p->on_deadline        = on_deadline;
    p->rt_urgent          = (cls == RV9_CLASS_REALTIME) ? rt_urgent : false;
    p->rt_bound_us        = rt_bound;
    p->rt_place           = rt_place;
    p->stack_bytes        = (uint32_t)stack;
    p->refs               = 1;      /* the task's, dropped at the end of finish() */
    p->serial             = charge.serial;
    p->footprint          = footprint;
    p->held               = footprint;
    p->budget             = budget;
    p->n_ancestors        = charge.n;
    memcpy(p->ancestors, charge.ancestors, sizeof(p->ancestors));
    strncpy(p->name, module_name, sizeof(p->name) - 1);
    if (arg != NULL) strncpy(p->arg, arg, sizeof(p->arg) - 1);

    p->next = s_procs;
    s_procs = p;

    rv9_lock_release(s_lock);

    /* Hand the child whatever the parent had open, before it can run. */
    if (s_on_fork) s_on_fork(parent, p->pid);

    /* A real-time process is not an RV-9 thread: it runs preemptively
       above everything, because its latency must not depend on anyone
       else's manners. See rv9/kal.h. */
    rv9_err_t err = (cls == RV9_CLASS_REALTIME)
        ? rv9_task_create_rt(proc_trampoline, p->name, stack, p, p->rt_urgent,
                             &p->task)
        : rv9_task_create(proc_trampoline, p->name, stack, p, priority,
                          &p->task);
    if (err != RV9_OK) {
        /* Leave the descriptor in the table marked dead rather than unpick
           the list from here; pruning forgets it like any other. There is
           no task to hold it. */
        rv9_lock_acquire(s_lock);
        p->exit_status = -1;
        p->refs        = 0;
        ended_locked(p);
        rv9_lock_release(s_lock);

        /* The fork hook has already run, so this process owns a path table
           and whatever it reserved. Nothing else will ever tell the I/O
           manager it is gone -- the trampoline that normally does so is
           exactly the thing that failed to start. */
        undo_fork(pid);

        rv9_mod_unlink(mod);
        return RV9_PROC_ERR_NOMEM;
    }

    if (cls == RV9_CLASS_REALTIME) {
        ESP_LOGI(TAG, "forked pid %u '%s' real-time, %lu us period",
                 (unsigned)p->pid, p->name, (unsigned long)period_us);
    } else {
        ESP_LOGI(TAG, "forked pid %u '%s' at priority %d",
                 (unsigned)p->pid, p->name, priority);
    }

    if (out_pid) *out_pid = p->pid;
    return RV9_PROC_OK;
}

/*
 * Wait by watching, not by blocking on a semaphore.
 *
 * A real-time process runs on the host's scheduler and an ordinary one on
 * RV-9's, so a semaphore handed between them would be signalled in one
 * world and waited on in the other. Polling a word costs a few wakeups and
 * works whichever scheduler either party belongs to.
 */
#define WAIT_POLL_MS 5

rv9_proc_err_t rv9_proc_wait(rv9_pid_t pid, int *out_status, uint32_t timeout_ms)
{
    /* Held, so it cannot be forgotten while this is looking at it. */
    rv9_lock_acquire(s_lock);
    rv9_proc_t *p = find_locked(pid);
    if (p != NULL) hold_locked(p);
    rv9_lock_release(s_lock);

    if (p == NULL) return RV9_PROC_ERR_NOTFOUND;

    uint64_t deadline = rv9_time_ms() + timeout_ms;
    rv9_proc_err_t result = RV9_PROC_OK;

    while (p->state != RV9_PROC_EXITED) {
        if (timeout_ms != RV9_WAIT_FOREVER && rv9_time_ms() >= deadline) {
            result = RV9_PROC_ERR_TIMEOUT;
            break;
        }
        rv9_task_delay_ms(WAIT_POLL_MS);

        /* Waiting forever on a child the scheduler already killed is the
           failure this looks for. Nobody else will write its status. */
        bool mine;
        rv9_lock_acquire(s_lock);
        mine = claim_faulted_locked(p);
        rv9_lock_release(s_lock);
        if (mine) collect_faulted(p);
    }

    rv9_lock_acquire(s_lock);
    if (result == RV9_PROC_OK) {
        if (out_status) *out_status = p->exit_status;
        p->waited = true;       /* collected: the first to be forgotten */
    }
    if (p->refs > 0) p->refs--;
    rv9_lock_release(s_lock);

    return result;
}

rv9_proc_err_t rv9_proc_chain(const char *module_name)
{
    if (module_name == NULL) return RV9_PROC_ERR_INVAL;

    rv9_lock_acquire(s_lock);
    rv9_proc_t *p = current_locked();
    if (p != NULL) {
        strncpy(p->chain_to, module_name, sizeof(p->chain_to) - 1);
        p->chain_to[sizeof(p->chain_to) - 1] = '\0';
        p->chain_pending = true;
    }
    rv9_lock_release(s_lock);

    return p ? RV9_PROC_OK : RV9_PROC_ERR_NOTFOUND;
}

rv9_proc_err_t rv9_proc_signal(rv9_pid_t pid, uint32_t signals)
{
    rv9_lock_acquire(s_lock);
    rv9_proc_t *p = find_locked(pid);
    bool live = (p != NULL && p->state != RV9_PROC_EXITED);
    if (live) p->signals |= signals;
    rv9_lock_release(s_lock);

    /* A signal to the dead is not delivered, and saying it was would let a
       `kill` report stopping something that had already gone. */
    return live ? RV9_PROC_OK : RV9_PROC_ERR_NOTFOUND;
}

/*
 * How long kill looks for a moment it can act in.
 *
 * Long against a lock held for microseconds, short against a person
 * waiting at a prompt. A real-time process is normally released within a
 * period of being asked; one that is not has stopped waiting for releases,
 * which is a different problem and should be reported rather than waited
 * out.
 */
#define KILL_WAIT_MS 500

static rv9_proc_err_t kill_held(rv9_proc_t *p, rv9_pid_t pid,
                                rv9_proc_class_t cls, rv9_task_t task);

rv9_proc_err_t rv9_proc_kill(rv9_pid_t pid)
{
    rv9_lock_acquire(s_lock);
    rv9_proc_t *p  = find_locked(pid);
    rv9_proc_t *me = current_locked();
    bool live = (p != NULL && p->state != RV9_PROC_EXITED);
    rv9_proc_class_t cls  = live ? p->cls  : RV9_CLASS_NORMAL;
    rv9_task_t       task = live ? p->task : NULL;
    if (live && p != me) hold_locked(p);
    rv9_lock_release(s_lock);

    if (!live)   return RV9_PROC_ERR_NOTFOUND;
    if (p == me) return RV9_PROC_ERR_INVAL;

    rv9_proc_err_t err = kill_held(p, pid, cls, task);
    put(p);
    return err;
}

/* The body of kill, with the descriptor held by the caller throughout. */
static rv9_proc_err_t kill_held(rv9_proc_t *p, rv9_pid_t pid,
                                rv9_proc_class_t cls, rv9_task_t task)
{
    uint64_t until = rv9_time_ms() + KILL_WAIT_MS;

    /*
     * A real-time process ends itself, in rt_wait, where it is known to
     * hold nothing: see end_here. All that can be done from here is to ask
     * the KAL to make its next wait the last, and to watch.
     */
    if (cls == RV9_CLASS_REALTIME) {
        if (task == NULL || rv9_rt_stop(task) != RV9_OK) {
            ESP_LOGW(TAG, "pid %u has declared no release, so there is no "
                          "safe moment to stop it at", (unsigned)pid);
            return RV9_PROC_ERR_INVAL;
        }
        while (p->state != RV9_PROC_EXITED) {
            if (rv9_time_ms() >= until) return RV9_PROC_ERR_TIMEOUT;
            rv9_task_delay_ms(WAIT_POLL_MS);
        }
        return RV9_PROC_OK;
    }

    /*
     * An ordinary process is a thread parked at a switch point, and is
     * stopped there unless it holds a lock. Checked and stopped under the
     * process lock, so it cannot exit between the two and have its thread
     * slot handed to somebody else, who would then be the one stopped.
     */
    for (;;) {
        rv9_lock_acquire(s_lock);
        bool ended = (p->state == RV9_PROC_EXITED || p->collecting ||
                      p->task == NULL);
        rv9_err_t err = ended ? RV9_OK : rv9_task_kill(p->task);
        rv9_lock_release(s_lock);

        if (ended || err == RV9_OK) break;
        if (err != RV9_ERR_BUSY) {
            ESP_LOGW(TAG, "pid %u cannot be stopped from outside here: %s",
                     (unsigned)pid, rv9_strerror(err));
            return RV9_PROC_ERR_INVAL;
        }
        if (rv9_time_ms() >= until) {
            /* Given up, so the request to give up is withdrawn: a process
               left marked cancelled would find every later wait of its own
               ending early, for no reason it could see. */
            rv9_lock_acquire(s_lock);
            if (p->task != NULL) rv9_task_uncancel(p->task);
            rv9_lock_release(s_lock);

            ESP_LOGW(TAG, "pid %u was inside something it could not be "
                          "stopped in for %d ms; not stopped",
                     (unsigned)pid, KILL_WAIT_MS);
            return RV9_PROC_ERR_TIMEOUT;
        }
        rv9_task_delay_ms(1);
    }

    /*
     * Hold the funeral now rather than leave it to a passer-by. Under
     * RV-9's own kernel nothing walks the table on a timer, and a killed
     * process nobody waits on would otherwise keep its devices -- which is
     * the opposite of what killing it was for.
     */
    bool mine;
    rv9_lock_acquire(s_lock);
    mine = claim_faulted_locked(p);
    rv9_lock_release(s_lock);
    if (mine) collect_faulted(p);

    /* Somebody else may be holding it; it is theirs to finish. */
    while (p->state != RV9_PROC_EXITED) {
        if (rv9_time_ms() >= until) return RV9_PROC_ERR_TIMEOUT;
        rv9_task_delay_ms(WAIT_POLL_MS);
    }
    return RV9_PROC_OK;
}

bool rv9_proc_info(rv9_pid_t pid, rv9_proc_info_t *out)
{
    if (out == NULL) return false;

    rv9_lock_acquire(s_lock);
    const rv9_proc_t *p = find_locked(pid);
    if (p != NULL) {
        memset(out, 0, sizeof(*out));
        out->pid         = p->pid;
        out->parent      = p->parent;
        memcpy(out->name, p->name, sizeof(out->name));
        out->state       = p->state;
        out->exit_status = p->exit_status;
        out->fault       = p->fault;
        out->waited      = p->waited;
        out->rt_urgent   = p->rt_urgent;
        out->rt_bound_us = p->rt_bound_us;
        out->footprint   = p->footprint;
        out->held        = p->held;
        out->budget      = p->budget;
    }
    rv9_lock_release(s_lock);
    return p != NULL;
}

int rv9_proc_list(rv9_sys_proc_t *out, int max)
{
    if (out == NULL || max <= 0) return proc_list_op(NULL, 0);
    return proc_list_op(out, (uint32_t)max * sizeof(rv9_sys_proc_t));
}

void rv9_proc_set_next_pid(rv9_pid_t pid)
{
    rv9_lock_acquire(s_lock);
    s_next_pid = (pid == RV9_PID_NONE) ? 1 : pid;
    rv9_lock_release(s_lock);
}

const void *rv9_proc_statics(rv9_pid_t pid)
{
    rv9_lock_acquire(s_lock);
    rv9_proc_t *p = find_locked(pid);
    const void *st = p ? p->statics : NULL;
    rv9_lock_release(s_lock);
    return st;
}

void rv9_proc_aging_set(bool enabled)
{
    /* When the kernel ages, aging is not ours to switch off. */
    if (rv9_sched_ages()) return;

    s_aging = enabled;

    if (!enabled) {
        /* Drop everyone back to their base priority, so the effect of aging
           being off is immediate and unambiguous. */
        rv9_lock_acquire(s_lock);
        for (rv9_proc_t *p = s_procs; p; p = p->next) {
            if (p->state != RV9_PROC_ACTIVE) continue;
            p->age = 0;
            p->effective_priority = p->base_priority;
            rv9_task_priority_set(p->task, p->base_priority);
        }
        rv9_lock_release(s_lock);
    }
}

bool rv9_proc_aging_get(void) { return rv9_sched_ages() ? true : s_aging; }
