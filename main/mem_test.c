/*
 * What still works when memory has run out.
 *
 * The tests below use the memory up on purpose, with threads that allocate
 * until they are refused and hold what they got, and then ask the
 * questions that matter on a machine that moves: can a control loop still
 * be admitted? Can a dying one's actuator still be parked? And does a
 * program that forks without end take the machine with it, or stop at its
 * own budget?
 *
 * Runs last at boot, and gives everything back before it returns.
 */
#include "mem_test.h"

#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"
#include "rv9/proc.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "mem-test";

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

/* ---- a thread that spends memory in one class and holds it ---- */

#define HOG_BLOCKS 256
#define HOG_BLOCK  512

typedef struct {
    int    cls;
    void  *block[HOG_BLOCKS];
    int    n;
    bool   full;
    bool   release;
    bool   freed;
} hog_t;

static void hog(void *arg)
{
    hog_t *h = (hog_t *)arg;
    rv9_mem_class_set(h->cls);

    while (h->n < HOG_BLOCKS) {
        void *b = rv9_alloc(HOG_BLOCK);
        if (b == NULL) break;
        h->block[h->n++] = b;
    }
    h->full = true;

    while (!h->release) rv9_task_delay_ms(10);

    for (int i = 0; i < h->n; i++) rv9_free(h->block[i]);
    h->n = 0;
    h->freed = true;

    for (;;) rv9_task_delay_ms(1000);
}

static bool start_hog(hog_t *h, int cls, rv9_task_t *t)
{
    memset(h, 0, sizeof(*h));
    h->cls = cls;
    if (rv9_task_create(hog, "st-hog", 2048, h, RV9_PRIO_NORMAL, t) != RV9_OK) {
        return false;
    }
    for (int i = 0; i < 200 && !h->full; i++) rv9_task_delay_ms(10);
    return h->full;
}

static void stop_hog(hog_t *h, rv9_task_t t)
{
    h->release = true;
    for (int i = 0; i < 200 && !h->freed; i++) rv9_task_delay_ms(10);
    if (t) rv9_task_delete(t);

    /* A deleted thread's stack goes back when the scheduler next runs, not
       at the call. Measuring before that counts it as lost. */
    rv9_task_delay_ms(50);
}

/* ---- the reserves ---- */

static void reserves(void)
{
    ESP_LOGI(TAG, "--- when ordinary memory is gone ---");

    size_t heap0 = rv9_heap_free();

    /* A loop holding the pin, started while there is room for it. */
    check(pin_write(1) && pin_read() == 1, "the pin is 1");
    rv9_pid_t loop = 0;
    check(rv9_proc_fork_rt("lateloop", 0, "ontime", &loop) == RV9_PROC_OK,
          "a loop holding the pin is running");
    rv9_task_delay_ms(100);

    static hog_t general;
    rv9_task_t tg = NULL;
    check(start_hog(&general, RV9_MEM_GENERAL, &tg),
          "ordinary work spends memory until it is refused");
    ESP_LOGI(TAG, "  (took %d x %d bytes; %u left above the floor)",
             general.n, HOG_BLOCK, (unsigned)rv9_heap_available());

    check(rv9_heap_available() + HOG_BLOCK >= rv9_heap_rt_reserve(),
          "and was refused with the real-time reserve still there");

    /* An ordinary fork, as an ordinary caller. */
    int prev = rv9_mem_class_set(RV9_MEM_GENERAL);
    rv9_pid_t pid = 0;
    rv9_proc_err_t e = rv9_proc_fork("deaf", RV9_PRIO_NORMAL, NULL, &pid);
    check(e == RV9_PROC_ERR_NOMEM, "an ordinary program cannot start now");
    if (e == RV9_PROC_OK) rv9_proc_kill(pid);

    /* A control loop, asked for by the same ordinary caller. */
    rv9_pid_t ctl = 0;
    e = rv9_proc_fork_rt("control", 0, NULL, &ctl);
    rv9_mem_class_set(prev);
    check(e == RV9_PROC_OK, "but a control loop is still admitted");
    if (e == RV9_PROC_OK) {
        rv9_task_delay_ms(200);
        int status = 0;
        rv9_proc_kill(ctl);
        rv9_proc_wait(ctl, &status, 1000);
    }

    /* Now the reserve too: nothing left above the floor at all. */
    static hog_t rt;
    rv9_task_t tr = NULL;
    check(start_hog(&rt, RV9_MEM_REALTIME, &tr),
          "real-time work spends the reserve until it is refused");
    ESP_LOGI(TAG, "  (took %d x %d more; %u left above the floor)",
             rt.n, HOG_BLOCK, (unsigned)rv9_heap_available());

    /* And the loop dies. Its failsafe must still be applied. */
    int status = 0;
    check(rv9_proc_kill(loop) == RV9_PROC_OK &&
          rv9_proc_wait(loop, &status, 1000) == RV9_PROC_OK,
          "the loop is stopped with every reserve above the floor gone");
    check(pin_read() == 0, "and its pin was still parked");

    stop_hog(&rt, tr);
    stop_hog(&general, tg);

    int32_t lost = (int32_t)heap0 - (int32_t)rv9_heap_free();
    ESP_LOGI(TAG, "  (heap %d bytes lower than before)", (int)lost);
    check(lost < 1024, "and everything is given back");
}

/* ---- budgets ---- */

static int run_forkbomb(void)
{
    rv9_pid_t pid = 0;
    if (rv9_proc_fork("forkbomb", RV9_PRIO_NORMAL, NULL, &pid) != RV9_PROC_OK) {
        return -100;
    }
    int status = -100;
    rv9_proc_wait(pid, &status, 10000);
    return status;
}

static void budgets(void)
{
    ESP_LOGI(TAG, "--- a program that forks without end ---");

    int first = run_forkbomb();
    check(first > 0, "is stopped by its budget, not by the machine running out");
    ESP_LOGI(TAG, "  (started %d children before the budget refused one)",
             first);

    /* Measured from after the first run, not before it: every process
       started leaves a remembered descriptor, and the history is bounded
       rather than empty. The second run must cost nothing more. */
    rv9_task_delay_ms(50);
    size_t heap1 = rv9_heap_free();

    int second = run_forkbomb();
    check(second == first, "and gets exactly as far again: the budget was "
                           "given back");

    rv9_task_delay_ms(50);
    int32_t lost = (int32_t)heap1 - (int32_t)rv9_heap_free();
    ESP_LOGI(TAG, "  (second run: heap %d bytes lower)", (int)lost);
    check(lost < 1024, "with nothing more left allocated");
}

bool rv9_mem_selftest(void)
{
    s_passed = s_failed = 0;

    reserves();
    budgets();

    if (s_failed == 0) {
        ESP_LOGI(TAG, "mem: %d/%d passed", s_passed, s_passed);
    } else {
        ESP_LOGE(TAG, "mem: %d passed, %d FAILED", s_passed, s_failed);
    }
    return s_failed == 0;
}
