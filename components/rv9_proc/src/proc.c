/*
 * RV-9 process manager.
 *
 * See rv9/proc.h for the scheduling model. The short version: the KAL gives
 * us priorities, RV-9 supplies the aging policy on top of them.
 */
#include "rv9/proc.h"

#include <string.h>

/* Modules learn about fork failures as these, negated; the values are ABI
   and declared separately in rv9/module.h, which a module may include and
   this header may not be. */
_Static_assert((int)RV9_PROC_ERR_NOTFOUND == RV9_PE_NOTFOUND, "ABI drift");
_Static_assert((int)RV9_PROC_ERR_NOMEM    == RV9_PE_NOMEM,    "ABI drift");
_Static_assert((int)RV9_PROC_ERR_MODULE   == RV9_PE_MODULE,   "ABI drift");
_Static_assert((int)RV9_PROC_ERR_TIMEOUT  == RV9_PE_TIMEOUT,  "ABI drift");
_Static_assert((int)RV9_PROC_ERR_INVAL    == RV9_PE_INVAL,    "ABI drift");

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

static rv9_proc_fork_hook_t s_on_fork;
static rv9_proc_exit_hook_t s_on_exit;

void rv9_proc_set_hooks(rv9_proc_fork_hook_t on_fork,
                        rv9_proc_exit_hook_t on_exit)
{
    s_on_fork = on_fork;
    s_on_exit = on_exit;
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
static int env_rt_declare(uint32_t period_us)
{
    rv9_lock_acquire(s_lock);
    rv9_proc_t *p = current_locked();
    bool ok = (p != NULL && p->cls == RV9_CLASS_REALTIME);
    if (ok) {
        if (period_us == 0) period_us = p->period_us;
        else                p->period_us = period_us;
    }
    rv9_lock_release(s_lock);

    if (!ok) return -1;

    /* Nobody said, and the module did not declare one. A real-time process
       with no period is not a real-time process. */
    if (period_us == 0) return -4;

    return rv9_rt_declare(period_us) == RV9_OK ? 0 : -2;
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
    rv9_lock_acquire(s_lock);
    rv9_proc_t *p = current_locked();
    bool ok = (p != NULL && p->cls == RV9_CLASS_REALTIME);
    if (ok) p->period_us = min_interval_us;
    rv9_lock_release(s_lock);

    if (!ok) return -1;

    rv9_event_t ev = rv9_event_by_id(event_id);
    if (ev == NULL) return -3;      /* nothing armed, or a stale id */

    return rv9_rt_declare_event(ev, min_interval_us) == RV9_OK ? 0 : -2;
}

static RV9_RT_CODE int env_rt_wait(void)
{
    return rv9_rt_wait();
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

    /* A real-time process gives its period back, or the next one cannot
       declare: the timer and its release semaphore belong to the slot, not
       to the module that borrowed it. */
    if (p->cls == RV9_CLASS_REALTIME) rv9_rt_release();

    /* Let the I/O manager close whatever this process left open, before we
       mark it dead and someone waiting on it wakes up. */
    if (s_on_exit) s_on_exit(p->pid);

    rv9_lock_acquire(s_lock);
    p->exit_status = rc;
    p->state       = RV9_PROC_EXITED;
    rv9_lock_release(s_lock);

    ESP_LOGI(TAG, "pid %u ('%s') exited, status %d",
             (unsigned)p->pid, p->name, rc);

    /* Release the module link and the private storage. The descriptor stays
       so that a parent can still wait on it and see the status. */
    rv9_mod_unlink(p->module);
    rv9_free(p->statics);
    p->statics = NULL;

    rv9_task_delete(NULL);
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

    p->fault      = rv9_task_fault(p->task);
    p->collecting = true;
    return true;
}

static void collect_faulted(rv9_proc_t *p)
{
    ESP_LOGE(TAG, "pid %u ('%s') killed: %s", (unsigned)p->pid, p->name,
             p->fault == RV9_TASK_FAULT_STACK ? "stack overflow" : "faulted");

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
    p->exit_status = -RV9_PROC_ERR_FAULT;
    p->state       = RV9_PROC_EXITED;
    p->collecting  = false;
    rv9_lock_release(s_lock);

    if (mod) rv9_mod_unlink(mod);
    rv9_free(st);
    rv9_task_reap(tk);      /* the slot may be reused now */
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

static const rv9_mod_proc_ops_t s_mod_proc_ops = {
    .fork  = proc_fork_op,
    .wait  = proc_wait_op,
    .procs = proc_list_op,
    .stacks = proc_stacks_op,
    .chain = proc_chain_op,
    .fork_arg = proc_fork_arg_op,
    .fork_rt  = proc_fork_rt_op,
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

static rv9_proc_err_t fork_common(const char *module_name, int priority,
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

    rv9_proc_t *p = rv9_calloc(1, sizeof(*p));
    if (p == NULL) {
        rv9_mod_unlink(mod);
        return RV9_PROC_ERR_NOMEM;
    }

    if (h->static_size > 0) {
        p->statics = rv9_calloc(1, h->static_size);
        if (p->statics == NULL) {
            rv9_free(p);
            rv9_mod_unlink(mod);
            return RV9_PROC_ERR_NOMEM;
        }
    }

    rv9_pid_t parent = rv9_proc_current_pid();

    rv9_lock_acquire(s_lock);

    p->pid                = s_next_pid++;
    p->parent             = parent;
    p->module             = mod;
    p->base_priority      = priority;
    p->age                = 0;
    p->effective_priority = priority;
    p->state              = RV9_PROC_ACTIVE;
    p->started_ms         = rv9_time_ms();
    p->cls                = cls;
    p->period_us          = period_us;
    strncpy(p->name, module_name, sizeof(p->name) - 1);
    if (arg != NULL) strncpy(p->arg, arg, sizeof(p->arg) - 1);

    p->next = s_procs;
    s_procs = p;

    rv9_lock_release(s_lock);

    /* Hand the child whatever the parent had open, before it can run. */
    if (s_on_fork) s_on_fork(parent, p->pid);

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

    /* A real-time process is not an RV-9 thread: it runs preemptively
       above everything, because its latency must not depend on anyone
       else's manners. See rv9/kal.h. */
    rv9_err_t err = (cls == RV9_CLASS_REALTIME)
        ? rv9_task_create_rt(proc_trampoline, p->name, stack, p, &p->task)
        : rv9_task_create(proc_trampoline, p->name, stack, p, priority,
                          &p->task);
    if (err != RV9_OK) {
        /* Leave the descriptor in the table marked dead rather than unpick
           the list from here; it costs a few bytes and keeps this simple. */
        rv9_lock_acquire(s_lock);
        p->state = RV9_PROC_EXITED;
        p->exit_status = -1;
        rv9_lock_release(s_lock);
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
    rv9_lock_acquire(s_lock);
    rv9_proc_t *p = find_locked(pid);
    rv9_lock_release(s_lock);

    if (p == NULL) return RV9_PROC_ERR_NOTFOUND;

    uint64_t deadline = rv9_time_ms() + timeout_ms;

    while (p->state != RV9_PROC_EXITED) {
        if (timeout_ms != RV9_WAIT_FOREVER && rv9_time_ms() >= deadline) {
            return RV9_PROC_ERR_TIMEOUT;
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

    if (out_status) *out_status = p->exit_status;
    return RV9_PROC_OK;
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
    if (p != NULL && p->state != RV9_PROC_EXITED) {
        p->signals |= signals;
    }
    rv9_lock_release(s_lock);

    return p ? RV9_PROC_OK : RV9_PROC_ERR_NOTFOUND;
}

const rv9_proc_t *rv9_proc_get(rv9_pid_t pid)
{
    rv9_lock_acquire(s_lock);
    rv9_proc_t *p = find_locked(pid);
    rv9_lock_release(s_lock);
    return p;
}

const void *rv9_proc_statics(rv9_pid_t pid)
{
    rv9_lock_acquire(s_lock);
    rv9_proc_t *p = find_locked(pid);
    const void *st = p ? p->statics : NULL;
    rv9_lock_release(s_lock);
    return st;
}

const rv9_proc_t *rv9_proc_next(const rv9_proc_t *prev)
{
    return prev ? prev->next : s_procs;
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
