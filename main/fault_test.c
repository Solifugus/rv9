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

#include <stddef.h>
#include <string.h>

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

#define CELL "/pub0/LATELOOP"

static bool cell_info(const char *name, rv9_pub_info_t *out)
{
    int p = rv9_io_open(name, RV9_MODE_READ);
    if (p < 0) return false;
    rv9_io_err_t err = rv9_io_getstat(p, RV9_PUB_GS_INFO, out);
    rv9_io_close(p);
    return err == RV9_IO_OK;
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

    rv9_pub_info_t info;
    check(cell_info(CELL, &info) && info.reserved_by == pid && info.held &&
          info.seq > 0, "and publishes into the cell its manifest reserved");

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

    check(cell_info(CELL, &info) && info.fault == RV9_FAULT_KILLED &&
          info.reserved_by == 0 && !info.held,
          "its cell says it was killed, and is no longer reserved");
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

    /* The cell persists from the last loop; count from where it stands. */
    rv9_pub_info_t info;
    uint32_t seq0 = cell_info(CELL, &info) ? info.seq : 0;

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

    /*
     * And the fault is published where the loop's values are. Periods 0 to
     * 20 were published -- 20 being the late one -- and then the fault,
     * which is a publication of its own that leaves the value alone.
     */
    struct { rv9_pub_t head; int32_t n; } m = { 0 };
    int r = rv9_io_open(CELL, RV9_MODE_READ);
    size_t done = 0;
    bool got = (r >= 0) &&
               rv9_io_read(r, &m, sizeof(m), &done) == RV9_IO_OK &&
               rv9_io_getstat(r, RV9_PUB_GS_INFO, &info) == RV9_IO_OK;

    check(got && info.fault == RV9_FAULT_DEADLINE, "its cell says DEADLINE");
    check(got && m.n == 20, "and still holds the last value it published");
    check(got && info.seq == seq0 + 22,
          "21 publications and then the fault, which is one more");

    /* A watcher that had seen the last value sees the fault as a change. */
    rv9_pub_wait_t w = { .seq = seq0 + 21, .timeout_ms = 0 };
    check(r >= 0 && rv9_io_getstat(r, RV9_PUB_GS_WAIT, &w) == RV9_IO_OK &&
          w.seq == seq0 + 22,
          "a watcher that saw the last value is told something changed");
    if (r >= 0) rv9_io_close(r);

    int again = rv9_io_open(CELL, RV9_MODE_WRITE);
    check(again >= 0 && cell_info(CELL, &info) && info.fault == 0,
          "and opening it to publish again clears the fault");
    if (again >= 0) rv9_io_close(again);
}

/* ------------------------------------------------------------------ */

/*
 * A module that is nothing but a manifest and a return.
 *
 * Built here and added to the directory from memory, because what is being
 * tested is admission: whether a declaration is accepted or refused before
 * any code runs. Two of these would otherwise be modules in the store that
 * exist only to be refused.
 */
static bool make_module(const char *name, uint16_t tag, const char *value)
{
    static uint8_t buf[160];
    memset(buf, 0, sizeof(buf));

    uint32_t len = RV9_MODULE_HDR_LEN;
    size_t nl = strlen(name) + 1;
    memcpy(buf + len, name, nl);
    len += (uint32_t)nl;
    len += (4 - (len % 4)) % 4;

    uint32_t manifest_off = len;
    rv9_mod_tlv_t e = { .tag = tag, .len = (uint16_t)strlen(value) };
    memcpy(buf + len, &e, sizeof(e));
    len += sizeof(e);
    memcpy(buf + len, value, e.len);
    len += e.len;
    len += (4 - (len % 4)) % 4;
    len += sizeof(rv9_mod_tlv_t);           /* RV9_MTAG_END, zeroed */

    /* jalr zero, 0(ra): return. It is run, after all, once admitted. */
    uint32_t entry_off = len;
    static const uint8_t ret[4] = { 0x67, 0x80, 0x00, 0x00 };
    memcpy(buf + len, ret, sizeof(ret));
    len += sizeof(ret);

    rv9_mod_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic           = RV9_MODULE_MAGIC;
    h.header_len      = RV9_MODULE_HDR_LEN;
    h.abi_version     = 1;
    h.module_len      = len;
    h.name_offset     = RV9_MODULE_HDR_LEN;
    h.entry_offset    = entry_off;
    h.type            = RV9_MOD_PROGRAM;
    h.revision        = 1;
    h.manifest_offset = manifest_off;
    memcpy(buf, &h, sizeof(h));

    uint32_t crc = rv9_crc32(0, buf, len);
    memcpy(buf + offsetof(rv9_mod_header_t, crc32), &crc, sizeof(crc));

    return rv9_mod_register_image(buf, len) == RV9_MOD_OK;
}

static void declared_publications(void)
{
    ESP_LOGI(TAG, "--- publications declared in the manifest ---");

    rv9_pid_t a = 0, b = 0;
    check(rv9_proc_fork_rt("control", 0, NULL, &a) == RV9_PROC_OK,
          "control is admitted, declaring /pub0/CONTROL");
    rv9_task_delay_ms(100);

    rv9_pub_info_t info;
    check(cell_info("/pub0/CONTROL", &info) && info.reserved_by == a,
          "the cell is reserved for it");

    int w = rv9_io_open("/pub0/CONTROL", RV9_MODE_WRITE);
    check(w == -RV9_IO_ERR_BUSY, "nothing else may publish into it");
    if (w >= 0) rv9_io_close(w);

    check(rv9_proc_fork_rt("control", 0, NULL, &b) == RV9_PROC_ERR_BUSY,
          "a second copy declaring it too is refused at fork");

    check(rv9_proc_kill(a) == RV9_PROC_OK, "stop control");
    check(cell_info("/pub0/CONTROL", &info) &&
          info.fault == RV9_FAULT_KILLED && info.reserved_by == 0,
          "its cell says it was killed and is free");
    check(rv9_io_remove("/pub0/CONTROL") == RV9_IO_OK,
          "and can be cleared, leaving nothing called CONTROL at all");

    /*
     * Watching. With the cell gone, what admits a watcher of CONTROL is
     * that a program on this machine declares it -- control's manifest, in
     * the store and not running.
     */
    rv9_pid_t c = 0;
    int status = 0;
    check(make_module("st-watch-control", RV9_MTAG_WATCHES, "/pub0/CONTROL")
          && rv9_proc_fork("st-watch-control", RV9_PRIO_NORMAL, NULL, &c)
             == RV9_PROC_OK,
          "a program watching CONTROL is admitted: control declares it");
    if (c) rv9_proc_wait(c, &status, 1000);

    check(make_module("st-watch-nobody", RV9_MTAG_WATCHES, "/pub0/STNOBODY")
          && rv9_proc_fork("st-watch-nobody", RV9_PRIO_NORMAL, NULL, &c)
             == RV9_PROC_ERR_NOPUB,
          "one watching a cell nothing provides is refused at fork");

    c = 0;
    check(make_module("st-pub-nobody", RV9_MTAG_PUBLISHES, "/pub0/STNOBODY")
          && rv9_proc_fork("st-watch-nobody", RV9_PRIO_NORMAL, NULL, &c)
             == RV9_PROC_OK,
          "and admitted once a program that publishes it is on the machine");
    if (c) rv9_proc_wait(c, &status, 1000);
}

/* ------------------------------------------------------------------ */

/*
 * The loops that never come to wait.
 *
 * Nothing above could stop these: every other path acts when a task comes
 * back to rt_wait. The watchdog has to notice from outside, and the
 * process has to be stopped from outside, where it is -- which is only
 * safe when where it is holds nothing.
 *
 * Each is judged on the same three things as a loop that does wait: it
 * ends, with the right reason, and its pin is parked. And on a fourth: it
 * ends in something like the time the watchdog promises, rather than
 * whenever it happens to stop by itself, which is never.
 */
static void stopped_from_outside(const char *module, const char *arg,
                                 int want_status, int want_fault,
                                 const char *what)
{
    ESP_LOGI(TAG, "--- %s ---", what);

    check(pin_write(1) && pin_read() == 1, "the pin is 1");

    int slots = rv9_rt_slots_used();
    rv9_pid_t pid = 0;
    if (rv9_proc_fork_rt(module, 0, arg, &pid) != RV9_PROC_OK) {
        check(false, "admit it");
        return;
    }

    uint64_t t0 = rv9_time_ms();
    int status = 0;
    bool ended = (rv9_proc_wait(pid, &status, 5000) == RV9_PROC_OK);
    uint32_t took = (uint32_t)(rv9_time_ms() - t0);

    check(ended, "it is stopped, though it never waits again");
    check(status == want_status, "its status says why");

    const rv9_proc_t *p = rv9_proc_get(pid);
    check(p != NULL && p->fault == want_fault, "and so does the table");
    check(pin_read() == 0, "its pin was parked");
    check(rv9_rt_slots_used() == slots, "its release slot is free");
    check(took < 2000, "promptly, not eventually");
    ESP_LOGI(TAG, "  (%lu ms from fork to stopped)", (unsigned long)took);
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

    stopped_from_outside("lateloop", "spin", -RV9_PROC_ERR_DEADLINE,
                         RV9_FAULT_DEADLINE,
                         "a loop that never finishes its activation");
    stopped_from_outside("runaway", NULL, -RV9_PROC_ERR_RUNAWAY,
                         RV9_FAULT_RUNAWAY,
                         "a loop that stops waiting, in its own code");
    stopped_from_outside("runaway", "syscalls", -RV9_PROC_ERR_RUNAWAY,
                         RV9_FAULT_RUNAWAY,
                         "a loop that stops waiting, through system calls");

    declared_publications();

    /* Cells outlive their publishers by design, so a test leaves them
       behind unless it clears them. */
    rv9_io_remove(CELL);

    if (s_failed == 0) {
        ESP_LOGI(TAG, "fault: %d/%d passed", s_passed, s_passed);
    } else {
        ESP_LOGE(TAG, "fault: %d passed, %d FAILED", s_passed, s_failed);
    }
    return s_failed == 0;
}
