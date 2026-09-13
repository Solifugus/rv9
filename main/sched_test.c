/*
 * Derived real-time priority, tested with the workload that needs it.
 *
 * `fastloop` does 2 ms of work in each 5 ms and must answer within 3; a
 * miss stops it. `heavyloop` spends 15 ms of every 100 working. At one
 * priority, a heavy release that lands while the fast loop is working --
 * two times in five -- is let in ahead of it, and the fast loop answers
 * 15 ms late and is stopped. Placed by deadline, the heavy loop cannot run
 * ahead of it.
 *
 * What the control run shows is measured, not assumed. The first version
 * of this test expected the fast loop to be *stopped* at one priority, and
 * that was true on some boots and not others: this host serves a release
 * of equal priority promptly most of the time, and the false DEADLINE
 * fault it causes is real but rare. What is reliably true is that the
 * fast loop answers later when it can be made to wait -- hundreds of
 * microseconds against a dozen -- so that is what is checked. A fault in
 * the control run counts as later too.
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

/*
 * The fast loop's worst response so far, read from the live real-time
 * statistics while it still has a slot. Found by its period, which nothing
 * else running at the time shares.
 */
static uint32_t fast_worst_response(void)
{
    for (int i = 0; i < rv9_rt_slot_count(); i++) {
        rv9_rt_stats_t st;
        bool valid = false;
        if (rv9_rt_stats_by_index(i, &st, &valid) != RV9_OK || !valid) continue;
        if (st.period_us == 5000) return st.max_response_us;
    }
    return 0;
}

/* Worst response of the fast loop in each run: [0] derived, [1] one
   priority. UINT32_MAX when it was stopped before it could be read. */
static uint32_t s_worst[2];

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

    /* Near the end of the fast loop's two seconds, while it is still there
       to be asked. */
    rv9_task_delay_ms(1800);
    uint32_t worst = fast_worst_response();

    int fs = 1, hs = 1;
    rv9_proc_wait(fast, &fs, 5000);
    rv9_proc_wait(heavy, &hs, 5000);

    if (fs == -RV9_PROC_ERR_DEADLINE) worst = UINT32_MAX;
    s_worst[derive ? 0 : 1] = worst;
    ESP_LOGI(TAG, "  (fast loop's worst response %lu us, status %d)",
             (unsigned long)worst, fs);

    if (derive) {
        check(fs == 0, "the fast loop met every deadline beside the heavy one");
    } else {
        check(s_worst[1] > s_worst[0],
              "at one priority it answers later than when placed by deadline");
    }
    check(hs == 0, "and the heavy loop finished its work");

    rv9_proc_rt_derive(true);
}

/* A realtime module with a period, a deadline and a cost, and a return.
   `placement` is written only when it is not negative. */
static bool make_rt_module(const char *name, uint32_t period, uint32_t deadline,
                           uint32_t wcet, int placement)
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
    if (placement >= 0) {
        rv9_mod_tlv_t e = { .tag = RV9_MTAG_PLACEMENT, .len = sizeof(uint8_t) };
        memcpy(buf + len, &e, sizeof(e));
        len += sizeof(e);
        buf[len] = (uint8_t)placement;
        len += 4;                           /* entries are 4-byte aligned */
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
 * Two candidates beside a running fastloop (2.5 ms declared, due in 3 ms),
 * each using 6% of the CPU -- so the pair sits well under the ceiling.
 * One has a 2.8 ms deadline: whichever of the two goes first, the other
 * waits 600 or 2500 us for it, and 2500 + 600 does not fit in 3000, nor
 * 600 + 2500 in 2800. The other has 6 ms, and fits underneath in 3100.
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
    check(make_rt_module("st-rt-tight", 10000, 2800, 600, -1) &&
          rv9_proc_fork_rt("st-rt-tight", 0, NULL, &pid)
              == RV9_PROC_ERR_UNSCHEDULABLE,
          "a loop that fits the CPU but not the deadlines is refused");

    pid = 0;
    check(make_rt_module("st-rt-fits", 10000, 6000, 600, -1) &&
          rv9_proc_fork_rt("st-rt-fits", 0, NULL, &pid) == RV9_PROC_OK,
          "one whose deadline leaves room is admitted");

    rv9_proc_info_t info;
    check(pid != 0 && rv9_proc_info(pid, &info) && !info.rt_urgent &&
          info.rt_bound_us == 3100,
          "below the fast loop, answering within 600 + 2500 us");

    int status = 0;
    if (pid) rv9_proc_wait(pid, &status, 1000);
    rv9_proc_wait(fast, &status, 5000);
    check(status == 0, "and the fast loop was not disturbed");
}

/*
 * R9's escape hatch: a declared placement constrains the analysis and
 * never overrides it. Pinned routine, a loop that would have been urgent
 * alone is not. Pinned urgent, the candidate the derivation admitted
 * below the fast loop above is refused, because above it the fast loop
 * would wait 600 us and answer in 3100 against 3000.
 */
static void placements(void)
{
    rv9_pid_t pid = 0;
    int status = 0;
    rv9_proc_info_t info;

    check(make_rt_module("st-rt-pinr", 10000, 9000, 600, RV9_PLACE_ROUTINE) &&
          rv9_proc_fork_rt("st-rt-pinr", 0, NULL, &pid) == RV9_PROC_OK &&
          rv9_proc_info(pid, &info) && !info.rt_urgent,
          "pinned routine, a loop alone runs routine");
    if (pid) rv9_proc_wait(pid, &status, 1000);

    rv9_pid_t fast = 0;
    if (!fork_rt("fastloop", &fast)) {
        check(false, "start fastloop");
        return;
    }
    rv9_task_delay_ms(30);

    pid = 0;
    check(make_rt_module("st-rt-pinu", 10000, 6000, 600, RV9_PLACE_URGENT) &&
          rv9_proc_fork_rt("st-rt-pinu", 0, NULL, &pid)
              == RV9_PROC_ERR_UNSCHEDULABLE,
          "pinned urgent where that would make the fast loop late: refused");

    pid = 0;
    check(make_rt_module("st-rt-badpin", 10000, 6000, 600, 7) &&
          rv9_proc_fork_rt("st-rt-badpin", 0, NULL, &pid)
              == RV9_PROC_ERR_CONTRACT,
          "a placement this system does not know is refused");

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
    ESP_LOGI(TAG, "--- a declared placement: constraint, not override ---");
    placements();

    if (s_failed == 0) {
        ESP_LOGI(TAG, "sched: %d/%d passed", s_passed, s_passed);
    } else {
        ESP_LOGE(TAG, "sched: %d passed, %d FAILED", s_passed, s_failed);
    }
    return s_failed == 0;
}
