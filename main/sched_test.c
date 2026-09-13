/*
 * Derived real-time priority, tested with the workload that needs it.
 *
 * `fastloop` has 500 us to answer each 5 ms release, and a miss stops it.
 * `heavyloop` spends 20 ms of every 100 working. At one priority they
 * time-slice, and the fast loop, released in the middle of that work,
 * waits for a tick -- and is stopped for it. Placed by deadline, it
 * preempts the heavy loop at once.
 *
 * The test runs the pair both ways. The run with the derivation switched
 * off is the control: if the fast loop survived that too, the other run
 * would prove nothing.
 *
 * Admission is tested with modules built in memory, because what is under
 * test is the refusal, before anything runs.
 */
#include "sched_test.h"

#include "rv9/kal.h"
#include "rv9/module.h"
#include "rv9/proc.h"

#include "esp_log.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "sched-test";

static int s_passed;
static int s_failed;

static void check(bool ok, const char *what)
{
    if (ok) { s_passed++; ESP_LOGI(TAG, "  pass  %s", what); }
    else    { s_failed++; ESP_LOGE(TAG, "  FAIL  %s", what); }
}

static bool fork_rt(const char *name, rv9_pid_t *pid)
{
    return rv9_proc_fork_rt(name, 0, NULL, pid) == RV9_PROC_OK;
}

static void pair(bool derive)
{
    rv9_proc_rt_derive(derive);

    rv9_pid_t heavy = 0, fast = 0;
    bool ok = fork_rt("heavyloop", &heavy);
    rv9_task_delay_ms(30);
    ok = fork_rt("fastloop", &fast) && ok;
    check(ok, derive ? "the pair is admitted"
                     : "the pair is admitted with every loop at one priority");
    if (!ok) { rv9_proc_rt_derive(true); return; }

    rv9_task_delay_ms(50);
    rv9_proc_info_t hi, fi;
    bool placed = rv9_proc_info(heavy, &hi) && rv9_proc_info(fast, &fi);

    if (derive) {
        check(placed && fi.rt_urgent && !hi.rt_urgent,
              "admitting the fast loop moved the running heavy loop below it");
        ESP_LOGI(TAG, "  (bounds: fast %lu us, heavy %lu us)",
                 (unsigned long)fi.rt_bound_us, (unsigned long)hi.rt_bound_us);
    }

    int fs = 1, hs = 1;
    rv9_proc_wait(fast, &fs, 5000);
    rv9_proc_wait(heavy, &hs, 5000);

    if (derive) {
        check(fs == 0, "the fast loop met every deadline beside the heavy one");
    } else {
        check(fs == -RV9_PROC_ERR_DEADLINE,
              "without derivation it is stopped for a deadline the scheduler "
              "missed");
    }
    check(hs == 0, "and the heavy loop finished its work");

    rv9_proc_rt_derive(true);
}

/* A realtime module with a period, a deadline and a cost, and a return. */
static bool make_rt_module(const char *name, uint32_t period, uint32_t deadline,
                           uint32_t wcet)
{
    static uint8_t buf[128];
    memset(buf, 0, sizeof(buf));

    uint32_t len = RV9_MODULE_HDR_LEN;
    size_t nl = strlen(name) + 1;
    memcpy(buf + len, name, nl);
    len += (uint32_t)nl;
    len += (4 - (len % 4)) % 4;

    uint32_t manifest_off = len;
    const struct { uint16_t tag; uint32_t v; } tags[] = {
        { RV9_MTAG_PERIOD_US,   period   },
        { RV9_MTAG_DEADLINE_US, deadline },
        { RV9_MTAG_WCET_US,     wcet     },
    };
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        rv9_mod_tlv_t e = { .tag = tags[i].tag, .len = sizeof(uint32_t) };
        memcpy(buf + len, &e, sizeof(e));
        len += sizeof(e);
        memcpy(buf + len, &tags[i].v, sizeof(uint32_t));
        len += sizeof(uint32_t);
    }
    len += sizeof(rv9_mod_tlv_t);           /* RV9_MTAG_END */

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

/*
 * Two candidates beside a running fastloop, each using 3.5% of the CPU.
 * One has a 400 us deadline: whichever of the two tight loops goes first,
 * the other waits for it, and 350 + 200 does not fit in 500 or 400. The
 * other has 2 ms, and fits underneath.
 */
static void admission(void)
{
    rv9_pid_t fast = 0;
    if (!fork_rt("fastloop", &fast)) {
        check(false, "start fastloop");
        return;
    }
    rv9_task_delay_ms(30);

    rv9_pid_t pid = 0;
    check(make_rt_module("st-rt-tight", 10000, 400, 350) &&
          rv9_proc_fork_rt("st-rt-tight", 0, NULL, &pid)
              == RV9_PROC_ERR_UNSCHEDULABLE,
          "a loop that fits the CPU but not the deadlines is refused");

    pid = 0;
    check(make_rt_module("st-rt-fits", 10000, 2000, 350) &&
          rv9_proc_fork_rt("st-rt-fits", 0, NULL, &pid) == RV9_PROC_OK,
          "one whose deadline leaves room is admitted");

    rv9_proc_info_t info;
    check(pid != 0 && rv9_proc_info(pid, &info) && !info.rt_urgent &&
          info.rt_bound_us == 550,
          "below the fast loop, answering within 350 + 200 us");

    int status = 0;
    if (pid) rv9_proc_wait(pid, &status, 1000);
    rv9_proc_wait(fast, &status, 5000);
    check(status == 0, "and the fast loop was not disturbed");
}

bool rv9_sched_selftest(void)
{
    s_passed = s_failed = 0;

    ESP_LOGI(TAG, "--- a fast loop beside a heavy one, priority derived ---");
    pair(true);
    ESP_LOGI(TAG, "--- the same, at one priority: the control ---");
    pair(false);
    ESP_LOGI(TAG, "--- admission by deadlines, not utilisation ---");
    admission();

    if (s_failed == 0) {
        ESP_LOGI(TAG, "sched: %d/%d passed", s_passed, s_passed);
    } else {
        ESP_LOGE(TAG, "sched: %d passed, %d FAILED", s_passed, s_failed);
    }
    return s_failed == 0;
}
