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

static int env_print(const char *s)
{
    if (s == NULL) return -1;

    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);
    rv9_proc_t *p = current_locked();
    rv9_pid_t pid = p ? p->pid : 0;
    rv9_mutex_unlock(s_lock);

    ESP_LOGI("proc", "[%u] %s", (unsigned)pid, s);
    return 0;
}

static uint64_t env_time_ms(void)        { return rv9_time_ms(); }
static void     env_yield(void)          { rv9_task_yield(); }
static void     env_sleep_ms(uint32_t m) { rv9_task_delay_ms(m); }

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

    const rv9_mod_header_t *h = (const rv9_mod_header_t *)p->module->image;

    rv9_mod_env_t env = {
        .abi_version  = RV9_MODULE_ABI,
        .statics      = p->statics,
        .statics_size = h->static_size,
        .print        = env_print,
        .time_ms      = env_time_ms,
        .pid          = p->pid,
        .arg          = NULL,
        .yield        = env_yield,
        .sleep_ms     = env_sleep_ms,
        .signals_take = env_signals_take,
    };

    int rc = p->module->entry(&env);

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

rv9_proc_err_t rv9_proc_init(void)
{
    if (s_running) return RV9_PROC_OK;

    if (rv9_mutex_create(&s_lock) != RV9_OK) return RV9_PROC_ERR_NOMEM;

    /* The ager must outrank everything it manages. */
    rv9_err_t err = rv9_task_create(ager_task, "rv9-ager", 2560, NULL,
                                    RV9_PRIO_AGER, NULL);
    if (err != RV9_OK) return RV9_PROC_ERR_NOMEM;

    s_running = true;
    ESP_LOGI(TAG, "process manager up (aging every %d ms, max boost %d)",
             AGE_PERIOD_MS, AGE_MAX);
    return RV9_PROC_OK;
}

rv9_proc_err_t rv9_proc_fork(const char *module_name, int priority,
                             const char *arg, rv9_pid_t *out_pid)
{
    (void)arg;   /* argument passing lands with the shell in phase 4 */

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

    rv9_mutex_lock(s_lock, RV9_WAIT_FOREVER);

    p->pid                = s_next_pid++;
    p->parent             = RV9_PID_NONE;
    p->module             = mod;
    p->base_priority      = priority;
    p->age                = 0;
    p->effective_priority = priority;
    p->state              = RV9_PROC_ACTIVE;
    p->started_ms         = rv9_time_ms();
    strncpy(p->name, module_name, sizeof(p->name) - 1);

    p->next = s_procs;
    s_procs = p;

    rv9_mutex_unlock(s_lock);

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
