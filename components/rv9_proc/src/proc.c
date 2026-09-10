/*
 * RV-9 process manager.
 *
 * See rv9/proc.h for the scheduling model. The short version: the KAL gives
 * us priorities, RV-9 supplies the aging policy on top of them.
 */
#include "rv9/proc.h"

#include <string.h>

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

#define PROC_DEFAULT_STACK 4096

static rv9_proc_t *s_procs;
static rv9_mutex_t s_lock;
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

static uint32_t env_signals_take(void)
{
    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);
    rv9_proc_t *p = current_locked();
    uint32_t sig = 0;
    if (p) {
        sig = p->signals;
        p->signals = 0;
    }
    rv9_mutex_unlock(s_lock);
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
    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);
    p->task = rv9_task_self();
    rv9_mutex_unlock(s_lock);

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
    env.arg          = p->arg[0] ? p->arg : NULL;

    rc = p->module->entry(&env);

    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);
    bool chaining = p->chain_pending;
    char next_name[32];
    if (chaining) {
        memcpy(next_name, p->chain_to, sizeof(next_name));
        p->chain_pending = false;
    }
    rv9_mutex_unlock(s_lock);

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

    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);
    p->module = next;
    strncpy(p->name, next_name, sizeof(p->name) - 1);
    rv9_mutex_unlock(s_lock);

    ESP_LOGI(TAG, "pid %u chained to '%s'", (unsigned)p->pid, next_name);
    }

    /* Let the I/O manager close whatever this process left open, before we
       mark it dead and someone waiting on it wakes up. */
    if (s_on_exit) s_on_exit(p->pid);

    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);
    p->exit_status = rc;
    p->state       = RV9_PROC_EXITED;
    rv9_mutex_unlock(s_lock);

    ESP_LOGI(TAG, "pid %u ('%s') exited, status %d",
             (unsigned)p->pid, p->name, rc);

    /* Release the module link and the private storage. The descriptor stays
       so that a parent can still wait on it and see the status. */
    rv9_mod_unlink(p->module);
    rv9_free(p->statics);
    p->statics = NULL;

    rv9_sem_give(p->exited);
    rv9_task_delete(NULL);
}

/* ------------------------------------------------------------------ */
/* Aging                                                               */
/* ------------------------------------------------------------------ */

static void ager_task(void *arg)
{
    (void)arg;

    for (;;) {
        rv9_task_delay_ms(AGE_PERIOD_MS);
        if (!s_aging) continue;

        rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);

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

        rv9_mutex_unlock(s_lock);
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
    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);

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

    rv9_mutex_unlock(s_lock);
    return (int)n;
}

static int proc_fork_arg_op(const char *module, int priority, const char *arg)
{
    rv9_pid_t pid = 0;
    rv9_proc_err_t err = rv9_proc_fork(module, priority, arg, &pid);
    return (err == RV9_PROC_OK) ? (int)pid : -(int)err;
}

static int proc_chain_op(const char *module)
{
    return rv9_proc_chain(module) == RV9_PROC_OK ? 0 : -1;
}

static const rv9_mod_proc_ops_t s_mod_proc_ops = {
    .fork  = proc_fork_op,
    .wait  = proc_wait_op,
    .procs = proc_list_op,
    .chain = proc_chain_op,
    .fork_arg = proc_fork_arg_op,
};

rv9_proc_err_t rv9_proc_init(void)
{
    if (s_running) return RV9_PROC_OK;

    if (rv9_mutex_create(&s_lock) != RV9_OK) return RV9_PROC_ERR_NOMEM;

    /* The ager must outrank everything it manages. */
    rv9_err_t err = rv9_task_create(ager_task, "rv9-ager", 2560, NULL,
                                    RV9_PRIO_AGER, NULL);
    if (err != RV9_OK) return RV9_PROC_ERR_NOMEM;

    rv9_mod_set_proc_ops(&s_mod_proc_ops);

    s_running = true;
    ESP_LOGI(TAG, "process manager up (aging every %d ms, max boost %d)",
             AGE_PERIOD_MS, AGE_MAX);
    return RV9_PROC_OK;
}

rv9_pid_t rv9_proc_current_pid(void)
{
    if (s_lock == NULL) return RV9_PID_NONE;

    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);
    rv9_proc_t *p = current_locked();
    rv9_pid_t pid = p ? p->pid : RV9_PID_NONE;
    rv9_mutex_unlock(s_lock);
    return pid;
}

rv9_proc_err_t rv9_proc_fork(const char *module_name, int priority,
                             const char *arg, rv9_pid_t *out_pid)
{
    if (module_name == NULL) return RV9_PROC_ERR_INVAL;

    rv9_mod_entry_t *mod = NULL;
    rv9_mod_err_t merr = rv9_mod_link(module_name, &mod);
    if (merr != RV9_MOD_OK) {
        ESP_LOGE(TAG, "fork '%s': %s", module_name, rv9_mod_strerror(merr));
        return RV9_PROC_ERR_MODULE;
    }

    const rv9_mod_header_t *h = (const rv9_mod_header_t *)mod->image;

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

    if (rv9_sem_create(1, 0, &p->exited) != RV9_OK) {
        rv9_free(p->statics);
        rv9_free(p);
        rv9_mod_unlink(mod);
        return RV9_PROC_ERR_NOMEM;
    }

    rv9_pid_t parent = rv9_proc_current_pid();

    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);

    p->pid                = s_next_pid++;
    p->parent             = parent;
    p->module             = mod;
    p->base_priority      = priority;
    p->age                = 0;
    p->effective_priority = priority;
    p->state              = RV9_PROC_ACTIVE;
    p->started_ms         = rv9_time_ms();
    strncpy(p->name, module_name, sizeof(p->name) - 1);
    if (arg != NULL) strncpy(p->arg, arg, sizeof(p->arg) - 1);

    p->next = s_procs;
    s_procs = p;

    rv9_mutex_unlock(s_lock);

    /* Hand the child whatever the parent had open, before it can run. */
    if (s_on_fork) s_on_fork(parent, p->pid);

    size_t stack = h->stack_size ? h->stack_size : PROC_DEFAULT_STACK;
    rv9_err_t err = rv9_task_create(proc_trampoline, p->name, stack, p,
                                    priority, &p->task);
    if (err != RV9_OK) {
        /* Leave the descriptor in the table marked dead rather than unpick
           the list from here; it costs a few bytes and keeps this simple. */
        rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);
        p->state = RV9_PROC_EXITED;
        p->exit_status = -1;
        rv9_mutex_unlock(s_lock);
        rv9_mod_unlink(mod);
        return RV9_PROC_ERR_NOMEM;
    }

    ESP_LOGI(TAG, "forked pid %u '%s' at priority %d",
             (unsigned)p->pid, p->name, priority);

    if (out_pid) *out_pid = p->pid;
    return RV9_PROC_OK;
}

rv9_proc_err_t rv9_proc_wait(rv9_pid_t pid, int *out_status, uint32_t timeout_ms)
{
    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);
    rv9_proc_t *p = find_locked(pid);
    rv9_mutex_unlock(s_lock);

    if (p == NULL) return RV9_PROC_ERR_NOTFOUND;

    if (p->state != RV9_PROC_EXITED) {
        if (rv9_sem_take(p->exited, timeout_ms) != RV9_OK) {
            return RV9_PROC_ERR_TIMEOUT;
        }
    }

    if (out_status) *out_status = p->exit_status;
    return RV9_PROC_OK;
}

rv9_proc_err_t rv9_proc_chain(const char *module_name)
{
    if (module_name == NULL) return RV9_PROC_ERR_INVAL;

    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);
    rv9_proc_t *p = current_locked();
    if (p != NULL) {
        strncpy(p->chain_to, module_name, sizeof(p->chain_to) - 1);
        p->chain_to[sizeof(p->chain_to) - 1] = '\0';
        p->chain_pending = true;
    }
    rv9_mutex_unlock(s_lock);

    return p ? RV9_PROC_OK : RV9_PROC_ERR_NOTFOUND;
}

rv9_proc_err_t rv9_proc_signal(rv9_pid_t pid, uint32_t signals)
{
    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);
    rv9_proc_t *p = find_locked(pid);
    if (p != NULL && p->state != RV9_PROC_EXITED) {
        p->signals |= signals;
    }
    rv9_mutex_unlock(s_lock);

    return p ? RV9_PROC_OK : RV9_PROC_ERR_NOTFOUND;
}

const rv9_proc_t *rv9_proc_get(rv9_pid_t pid)
{
    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);
    rv9_proc_t *p = find_locked(pid);
    rv9_mutex_unlock(s_lock);
    return p;
}

const void *rv9_proc_statics(rv9_pid_t pid)
{
    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);
    rv9_proc_t *p = find_locked(pid);
    const void *st = p ? p->statics : NULL;
    rv9_mutex_unlock(s_lock);
    return st;
}

const rv9_proc_t *rv9_proc_next(const rv9_proc_t *prev)
{
    return prev ? prev->next : s_procs;
}

void rv9_proc_aging_set(bool enabled)
{
    s_aging = enabled;

    if (!enabled) {
        /* Drop everyone back to their base priority, so the effect of aging
           being off is immediate and unambiguous. */
        rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);
        for (rv9_proc_t *p = s_procs; p; p = p->next) {
            if (p->state != RV9_PROC_ACTIVE) continue;
            p->age = 0;
            p->effective_priority = p->base_priority;
            rv9_task_priority_set(p->task, p->base_priority);
        }
        rv9_mutex_unlock(s_lock);
    }
}

bool rv9_proc_aging_get(void) { return s_aging; }
