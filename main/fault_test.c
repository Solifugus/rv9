/*
 * Stopping processes, tested on the processes themselves.
 *
 * Everything here is a claim about what happens to something that is not
 * the test: a thread it started, a process that will not listen, a control
 * loop that is late. So it forks real modules -- `deaf` and `lateloop` --
 * and judges them from outside, the way anything supervising them would.
 *
 * The pin is the evidence for the failsafe, as it is for `hold crash`. It
 * is driven to 1 before each loop starts so that reading 0 afterwards
 * means RV-9 wrote 0, not that the pin was 0 anyway.
 */
#include "fault_test.h"

#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"
#include "rv9/proc.h"

#include "esp_log.h"

static const char *TAG = "fault-test";

static int s_passed;
static int s_failed;

static void check(bool ok, const char *what)
{
    if (ok) { s_passed++; ESP_LOGI(TAG, "  pass  %s", what); }
    else    { s_failed++; ESP_LOGE(TAG, "  FAIL  %s", what); }
}

#define PIN "/gpio/2"

static int pin_read(void)
{
    int p = rv9_io_open(PIN, RV9_MODE_READ);
    if (p < 0) return p;
    uint32_t v = 0xFFFFFFFFu;
    size_t done = 0;
    rv9_io_err_t err = rv9_io_read(p, &v, sizeof(v), &done);
    rv9_io_close(p);
    return (err == RV9_IO_OK && done == sizeof(v)) ? (int)v : -1;
}

static bool pin_write(uint32_t v)
{
    int p = rv9_io_open(PIN, RV9_MODE_WRITE);
    if (p < 0) return false;
    size_t done = 0;
    rv9_io_err_t err = rv9_io_write(p, &v, sizeof(v), &done);
    rv9_io_close(p);
    return err == RV9_IO_OK;
}

/* ------------------------------------------------------------------ */

typedef struct {
    rv9_lock_t lock;
    bool       holding;
} holder_arg_t;

static void lock_holder(void *arg)
{
    holder_arg_t *h = (holder_arg_t *)arg;

    rv9_lock_acquire(h->lock);
    h->holding = true;
    rv9_task_delay_ms(300);          /* parked at a switch point, holding it */
    h->holding = false;
    rv9_lock_release(h->lock);

    for (;;) rv9_task_delay_ms(1000);
}

/*
 * The rule everything else rests on. A thread stopped while holding a lock
 * takes the lock with it; the kernel has to refuse, and then stop it once
 * it has let go.
 */
static void not_while_holding(void)
{
    static holder_arg_t h;
    if (rv9_lock_create(&h.lock) != RV9_OK) {
        check(false, "create a lock");
        return;
    }

    rv9_task_t t = NULL;
    if (rv9_task_create(lock_holder, "holder", 2048, &h, RV9_PRIO_NORMAL,
                        &t) != RV9_OK) {
        check(false, "start a thread to hold it");
        return;
    }

    rv9_task_delay_ms(50);
    check(h.holding, "a thread is holding a lock and asleep");
    check(rv9_task_kill(t) == RV9_ERR_BUSY,
          "it is not stopped while it holds the lock");
    check(rv9_task_alive(t), "and is still alive");

    rv9_task_delay_ms(400);
    check(!h.holding, "it lets go");
    check(rv9_task_kill(t) == RV9_OK, "and then it is stopped");

    rv9_task_delay_ms(10);
    check(!rv9_task_alive(t) &&
          rv9_task_fault(t) == RV9_TASK_FAULT_KILLED,
          "the corpse says it was killed");
    rv9_task_reap(t);

    /* If the refusal were wrong this would never return, and the boot
       would stop here -- a louder failure than a FAIL line, but a fair one. */
    rv9_lock_acquire(h.lock);
    rv9_lock_release(h.lock);
    check(true, "the lock it held is free");
    rv9_lock_destroy(h.lock);

    check(rv9_task_kill(rv9_task_self()) != RV9_OK,
          "a thread cannot stop itself this way");
}

/* ------------------------------------------------------------------ */

static void a_process_that_will_not_listen(void)
{
    rv9_pid_t pid = 0;
    if (rv9_proc_fork("deaf", RV9_PRIO_NORMAL, NULL, &pid) != RV9_PROC_OK) {
        check(false, "start deaf");
        return;
    }
    rv9_task_delay_ms(100);

    int status = 0;
    check(rv9_proc_signal(pid, RV9_SIG_STOP) == RV9_PROC_OK,
          "it is asked to stop");
    check(rv9_proc_wait(pid, &status, 300) == RV9_PROC_ERR_TIMEOUT,
          "and does not");

    check(rv9_proc_kill(pid) == RV9_PROC_OK, "so it is killed");
    check(rv9_proc_wait(pid, &status, 0) == RV9_PROC_OK &&
          status == -RV9_PROC_ERR_KILLED,
          "its status says killed, which no module returns");

    const rv9_proc_t *p = rv9_proc_get(pid);
    check(p != NULL && p->fault == RV9_FAULT_KILLED,
          "and so does the process table");

    check(rv9_proc_kill(pid) == RV9_PROC_ERR_NOTFOUND,
          "killing it twice finds nothing");
    check(rv9_proc_signal(pid, RV9_SIG_STOP) == RV9_PROC_ERR_NOTFOUND,
          "and a signal to the dead is not delivered");
}

/* ------------------------------------------------------------------ */

static void a_loop_killed_between_activations(void)
{
    check(pin_write(1) && pin_read() == 1,
          "the pin holds what it was last given");

    int slots = rv9_rt_slots_used();
    rv9_pid_t pid = 0;

    /* Late at period 1000, ten seconds away: this one is killed first. */
    if (rv9_proc_fork_rt("lateloop", 0, "1000", &pid) != RV9_PROC_OK) {
        check(false, "admit lateloop");
        return;
    }
    rv9_task_delay_ms(300);
    check(rv9_rt_slots_used() == slots + 1, "the loop holds a release slot");

    uint64_t t0 = rv9_time_ms();
    rv9_proc_err_t err = rv9_proc_kill(pid);
    uint32_t took = (uint32_t)(rv9_time_ms() - t0);

    check(err == RV9_PROC_OK, "it is killed");
    check(took < 50, "within a few of its 10 ms periods, not at leisure");

    int status = 0;
    check(rv9_proc_wait(pid, &status, 0) == RV9_PROC_OK &&
          status == -RV9_PROC_ERR_KILLED, "its status says killed");
    check(rv9_rt_slots_used() == slots, "and its release is gone");
    check(pin_read() == 0, "its failsafe was applied, as for any exit");
}

/* ------------------------------------------------------------------ */

/*
 * R9 §15.1: failsafe, then the reason, then never again.
 *
 * The last of the three is shown by lateloop itself: had it been released
 * after the late period, it would have run on and returned 1 instead.
 */
static void a_loop_that_misses_its_deadline(void)
{
    check(pin_write(1) && pin_read() == 1, "the pin is 1 again");

    int slots = rv9_rt_slots_used();
    rv9_pid_t pid = 0;

    if (rv9_proc_fork_rt("lateloop", 0, "20", &pid) != RV9_PROC_OK) {
        check(false, "admit lateloop");
        return;
    }

    int status = 0;
    check(rv9_proc_wait(pid, &status, 3000) == RV9_PROC_OK,
          "a loop late on its 20th period ends by itself");
    check(status == -RV9_PROC_ERR_DEADLINE,
          "its status says it missed a deadline, not that it returned");

    const rv9_proc_t *p = rv9_proc_get(pid);
    check(p != NULL && p->fault == RV9_FAULT_DEADLINE,
          "the process table says DEADLINE");
    check(pin_read() == 0, "its pin was parked at 0");
    check(rv9_rt_slots_used() == slots, "it will not be released again");
}

bool rv9_fault_selftest(void)
{
    s_passed = s_failed = 0;

    /* Idempotent, and needed: processes are forked here before the
       demonstrations that would otherwise have started the manager. */
    if (rv9_proc_init() != RV9_PROC_OK) {
        ESP_LOGE(TAG, "no process manager");
        return false;
    }

    ESP_LOGI(TAG, "--- not while holding a lock ---");
    not_while_holding();
    ESP_LOGI(TAG, "--- a process that will not listen ---");
    a_process_that_will_not_listen();
    ESP_LOGI(TAG, "--- a loop killed between activations ---");
    a_loop_killed_between_activations();
    ESP_LOGI(TAG, "--- a loop that misses its deadline ---");
    a_loop_that_misses_its_deadline();

    if (s_failed == 0) {
        ESP_LOGI(TAG, "fault: %d/%d passed", s_passed, s_passed);
    } else {
        ESP_LOGE(TAG, "fault: %d passed, %d FAILED", s_passed, s_failed);
    }
    return s_failed == 0;
}
