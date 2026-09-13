/*
 * How long a process is remembered, tested by forking a great many.
 *
 * The failure this guards against was not a crash but a slope: every
 * process that had ever run kept its descriptor, so the heap went down a
 * little with every command and never came back. Nothing about one fork
 * is wrong; the forty-first is where it shows. So these fork dozens.
 *
 * The children are a module built here in memory that returns at once --
 * what is under test is the table, not anything a child does.
 */
#include "proc_test.h"

#include "rv9/kal.h"
#include "rv9/module.h"
#include "rv9/proc.h"

#include "esp_log.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "proc-test";

static int s_passed;
static int s_failed;

static void check(bool ok, const char *what)
{
    if (ok) { s_passed++; ESP_LOGI(TAG, "  pass  %s", what); }
    else    { s_failed++; ESP_LOGE(TAG, "  FAIL  %s", what); }
}

#define QUICK "st-quick"

/* A header, a name and a return. No manifest: nothing to admit. */
static bool make_quick(void)
{
    static uint8_t buf[64];
    memset(buf, 0, sizeof(buf));

    uint32_t len = RV9_MODULE_HDR_LEN;
    memcpy(buf + len, QUICK, sizeof(QUICK));
    len += sizeof(QUICK);
    len += (4 - (len % 4)) % 4;

    uint32_t entry_off = len;
    static const uint8_t ret[4] = { 0x67, 0x80, 0x00, 0x00 };  /* ret */
    memcpy(buf + len, ret, sizeof(ret));
    len += sizeof(ret);

    rv9_mod_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic        = RV9_MODULE_MAGIC;
    h.header_len   = RV9_MODULE_HDR_LEN;
    h.abi_version  = 1;
    h.module_len   = len;
    h.name_offset  = RV9_MODULE_HDR_LEN;
    h.entry_offset = entry_off;
    h.type         = RV9_MOD_PROGRAM;
    h.revision     = 1;
    memcpy(buf, &h, sizeof(h));

    uint32_t crc = rv9_crc32(0, buf, len);
    memcpy(buf + offsetof(rv9_mod_header_t, crc32), &crc, sizeof(crc));

    return rv9_mod_register_image(buf, len) == RV9_MOD_OK;
}

static int exited_count(void)
{
    static rv9_sys_proc_t recs[64];
    int n = rv9_proc_list(recs, 64);
    if (n > 64) n = 64;

    int exited = 0;
    for (int i = 0; i < n; i++) {
        if (recs[i].state == RV9_PROC_EXITED) exited++;
    }
    return exited;
}

static bool fork_and_wait(rv9_pid_t *out)
{
    rv9_pid_t pid = 0;
    if (rv9_proc_fork(QUICK, RV9_PRIO_NORMAL, NULL, &pid) != RV9_PROC_OK) {
        return false;
    }
    int status = 0;
    if (out) *out = pid;
    return rv9_proc_wait(pid, &status, 2000) == RV9_PROC_OK;
}

/*
 * The slope itself. Forty forks first, so the history is full and every
 * module image and table the system caches has been made; then forty more,
 * which must cost nothing that stays.
 */
static void forking_leaves_no_trace(void)
{
    ESP_LOGI(TAG, "--- forking leaves no trace ---");

    bool all = true;
    for (int i = 0; i < 40; i++) all = fork_and_wait(NULL) && all;
    check(all, "forty processes forked and collected");

    int exited = exited_count();
    check(exited <= 16 + 1, "the remembered dead are bounded, not forty");
    ESP_LOGI(TAG, "  (%d exited processes remembered)", exited);

    size_t before = rv9_heap_free();
    for (int i = 0; i < 40; i++) all = fork_and_wait(NULL) && all;
    size_t after = rv9_heap_free();

    int32_t lost = (int32_t)before - (int32_t)after;
    ESP_LOGI(TAG, "  (40 more forks: heap %u -> %u, %d bytes)",
             (unsigned)before, (unsigned)after, (int)lost);
    check(all && lost < 512,
          "forty more cost nothing that stays (under half a kilobyte)");
}

/*
 * Who is forgotten. A child nobody has collected yet -- its parent, here
 * the system, may still ask -- is kept longer than one already collected,
 * but not for ever.
 */
static void uncollected_are_kept_longer(void)
{
    ESP_LOGI(TAG, "--- the uncollected are kept longer, not for ever ---");

    rv9_pid_t first = 0, last = 0;
    bool all = true;

    for (int i = 0; i < 40; i++) {
        rv9_pid_t pid = 0;
        if (rv9_proc_fork(QUICK, RV9_PRIO_NORMAL, NULL, &pid) != RV9_PROC_OK) {
            all = false;
            break;
        }
        if (i == 0) first = pid;
        last = pid;
        rv9_task_delay_ms(10);          /* let it end before the next fork */
    }
    check(all, "forty children forked and not waited on");

    rv9_task_delay_ms(50);
    int exited = exited_count();
    check(exited > 16 && exited <= 32 + 1,
          "more than sixteen are kept, but no more than thirty-two");
    ESP_LOGI(TAG, "  (%d exited processes remembered)", exited);

    rv9_proc_info_t info;
    check(!rv9_proc_info(first, &info),
          "the oldest uncollected child has been forgotten");

    int status = 1;
    check(rv9_proc_wait(last, &status, 1000) == RV9_PROC_OK,
          "the newest can still be waited on");
    check(rv9_proc_wait(first, &status, 0) == RV9_PROC_ERR_NOTFOUND,
          "and waiting on a forgotten one says so, rather than hanging");

    /*
     * Collect the rest, so they may be forgotten. Uncollected children of
     * the system are exactly what is kept longest, and the first version of
     * this test left thirty of them in the table for the life of the boot
     * -- seven kilobytes the services started afterwards could not have.
     */
    for (rv9_pid_t pid = first; pid <= last; pid++) {
        rv9_proc_wait(pid, &status, 0);
    }
}

/*
 * The wrap. Sixty-five thousand forks cannot be spent in a test, so the
 * allocator is moved to the edge instead, with pids near the bottom of the
 * range still remembered from the tests above.
 */
static void pids_survive_the_wrap(void)
{
    ESP_LOGI(TAG, "--- pids survive the sixteen-bit wrap ---");

    rv9_pid_t a = 0, b = 0, c = 0;
    rv9_proc_set_next_pid(65534);
    bool ok = fork_and_wait(&a) && fork_and_wait(&b) && fork_and_wait(&c);

    check(ok && a == 65534 && b == 65535, "the last two pids are used");
    check(ok && c != 0, "and the next is not zero");

    /* c must not name anything that was already there. */
    rv9_proc_info_t info;
    check(ok && rv9_proc_info(c, &info) && strcmp(info.name, QUICK) == 0,
          "nor a process that already had that number");
    ESP_LOGI(TAG, "  (after 65535 came %u)", (unsigned)c);
}

bool rv9_proc_selftest(void)
{
    s_passed = s_failed = 0;

    if (!make_quick()) {
        ESP_LOGE(TAG, "cannot build the test module");
        return false;
    }

    forking_leaves_no_trace();
    uncollected_are_kept_longer();
    pids_survive_the_wrap();

    if (s_failed == 0) {
        ESP_LOGI(TAG, "proc: %d/%d passed", s_passed, s_passed);
    } else {
        ESP_LOGE(TAG, "proc: %d passed, %d FAILED", s_passed, s_failed);
    }
    return s_failed == 0;
}
