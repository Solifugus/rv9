/*
 * Publication cells, tested through the I/O manager.
 *
 * Deliberately not against PFM's internals. Ownership (io_test.c) is a
 * table and is tested as one; a publication is a *protocol* between two
 * processes, and almost everything that could be wrong with it -- a
 * sequence that does not advance, a set that arrives half old, a second
 * writer that is allowed in, a wait that sleeps through a change -- is
 * only wrong on the far side of open(), read() and write(). So these go
 * through the same calls a module makes.
 *
 * The one thing that cannot be arranged here is a real race: this runs on
 * one task at boot, before there is a scheduler's worth of contention. The
 * seqlock's contended path is exercised on the machine instead, by
 * `control` publishing at 1 kHz while `watch` reads -- see docs/design.md.
 *
 * Cells are removed at the end. They are not freed when a publisher exits
 * (that is the point of them), so a test that left two behind would leave
 * them in `pubs` until the next reboot.
 */
#include "pub_test.h"

#include "rv9/io.h"
#include "rv9/kal.h"
#include "rv9/module.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "pub-test";

static int s_passed;
static int s_failed;

static void check(bool ok, const char *what)
{
    if (ok) { s_passed++; ESP_LOGI(TAG, "  pass  %s", what); }
    else    { s_failed++; ESP_LOGE(TAG, "  FAIL  %s", what); }
}

#define CELL_A "/pub0/TESTONE"
#define CELL_B "/pub0/TESTTWO"

/* Head plus a comfortable set of values, the shape every caller uses. */
typedef struct {
    rv9_pub_t head;
    int32_t   v[8];
} msg_t;

static int publish(int p, int32_t a, int32_t b, int32_t c, uint64_t stamp)
{
    msg_t m;
    memset(&m, 0, sizeof(m));
    m.head.len      = 3 * sizeof(int32_t);
    m.head.stamp_us = stamp;
    m.v[0] = a; m.v[1] = b; m.v[2] = c;

    size_t done = 0;
    rv9_io_err_t err = rv9_io_write(p, &m, sizeof(rv9_pub_t) + m.head.len,
                                    &done);
    return (err == RV9_IO_OK) ? (int)done : -(int)err;
}

static void naming(void)
{
    int p = rv9_io_open("/pub0/no-such-thing", RV9_MODE_READ);
    check(p == -RV9_IO_ERR_NOTFOUND,
          "reading a cell nobody publishes is refused, not invented");

    p = rv9_io_open("/pub0/has a space", RV9_MODE_READ);
    check(p < 0, "a name with a space in it is not a publication");

    p = rv9_io_open("/pub0/THIS_NAME_IS_MUCH_TOO_LONG_TO_FIT", RV9_MODE_WRITE);
    check(p == -RV9_IO_ERR_INVAL, "and neither is one that will not fit");
}

static void first_publication(void)
{
    int w = rv9_io_open(CELL_A, RV9_MODE_WRITE);
    if (w < 0) { check(false, "a publisher may declare a cell"); return; }
    check(true, "a publisher may declare a cell");

    int r = rv9_io_open(CELL_A, RV9_MODE_READ);
    check(r >= 0, "and an observer may then open it");
    if (r < 0) { rv9_io_close(w); return; }

    /*
     * R9 §18: before the first `expose` a value is not yet externally
     * available. It is not zero, and it is not an error -- it is a
     * readable cell that says so.
     */
    msg_t got;
    size_t done = 0;
    memset(&got, 0xEE, sizeof(got));
    check(rv9_io_read(r, &got, sizeof(got), &done) == RV9_IO_OK,
          "an unpublished cell reads without error");
    check(done == sizeof(rv9_pub_t) && got.head.seq == 0 && got.head.len == 0,
          "and says so: sequence zero, nothing in it");

    check(publish(w, 10, 20, 30, 0) == (int)(sizeof(rv9_pub_t) + 12),
          "the first publication is accepted");

    check(rv9_io_read(r, &got, sizeof(got), &done) == RV9_IO_OK &&
          got.head.seq == 1, "and the sequence becomes one");
    check(got.head.len == 12 && got.v[0] == 10 && got.v[1] == 20 &&
          got.v[2] == 30, "with the whole set, as published");
    check(got.head.stamp_us != 0,
          "a publisher that gave no time is stamped with now");

    publish(w, 11, 21, 31, 0);
    publish(w, 12, 22, 32, 0);
    check(rv9_io_read(r, &got, sizeof(got), &done) == RV9_IO_OK &&
          got.head.seq == 3 && got.v[0] == 12,
          "the sequence counts publications, and the value is the latest");

    check(publish(w, 1, 2, 3, 990000ULL) > 0 &&
          rv9_io_read(r, &got, sizeof(got), &done) == RV9_IO_OK &&
          got.head.stamp_us == 990000ULL,
          "a stated observation time is kept, not overwritten");

    rv9_io_close(r);
    rv9_io_close(w);
}

static void one_writer(void)
{
    int w = rv9_io_open(CELL_A, RV9_MODE_WRITE);
    check(w >= 0, "reopen for writing after the last publisher left");

    int second = rv9_io_open(CELL_A, RV9_MODE_WRITE);
    check(second == -RV9_IO_ERR_BUSY,
          "a second publisher is refused: one value, one author");

    int r1 = rv9_io_open(CELL_A, RV9_MODE_READ);
    int r2 = rv9_io_open(CELL_A, RV9_MODE_READ);
    check(r1 >= 0 && r2 >= 0,
          "but any number of observers may read it at once");

    rv9_pub_info_t info;
    check(rv9_io_getstat(r1, RV9_PUB_GS_INFO, &info) == RV9_IO_OK &&
          info.readers == 2 && info.held,
          "and the cell can say who has it");

    if (r1 >= 0) rv9_io_close(r1);
    if (r2 >= 0) rv9_io_close(r2);
    if (w >= 0)  rv9_io_close(w);

    int again = rv9_io_open(CELL_A, RV9_MODE_WRITE);
    check(again >= 0, "the writer's exit hands publication back");
    if (again >= 0) rv9_io_close(again);
}

/*
 * The value survives the publisher.
 *
 * This is the property that makes a publication worth having after a
 * failure rather than only during normal running: whatever investigates a
 * stopped machine reads the last thing the stopped component said, and
 * when it said it.
 */
static void outlives_its_publisher(void)
{
    int r = rv9_io_open(CELL_A, RV9_MODE_READ);
    if (r < 0) { check(false, "the cell is still there"); return; }

    msg_t got;
    size_t done = 0;
    check(rv9_io_read(r, &got, sizeof(got), &done) == RV9_IO_OK &&
          got.head.seq == 4 && got.v[0] == 1 && got.head.stamp_us == 990000ULL,
          "with nobody publishing, the last publication is still readable");

    rv9_pub_info_t info;
    check(rv9_io_getstat(r, RV9_PUB_GS_INFO, &info) == RV9_IO_OK &&
          !info.held,
          "and the cell reports that nobody is publishing it");

    rv9_io_close(r);
}

static void limits(void)
{
    int w = rv9_io_open(CELL_B, RV9_MODE_WRITE);
    if (w < 0) { check(false, "declare a second cell"); return; }
    check(true, "declare a second cell");

    /* Sixty-four bytes per cell, from the descriptor. */
    uint8_t big[sizeof(rv9_pub_t) + 128];
    memset(big, 0, sizeof(big));
    size_t done = 0;
    check(rv9_io_write(w, big, sizeof(big), &done) == RV9_IO_ERR_INVAL,
          "publishing more than the cell holds is refused, not truncated");

    rv9_pub_t head_only = { 0, 0, 0 };
    check(rv9_io_write(w, &head_only, sizeof(head_only), &done) == RV9_IO_OK,
          "a publication with no values in it is still a publication");

    publish(w, 7, 8, 9, 0);

    /*
     * A reader whose buffer is too small gets a coherent prefix and the
     * true length, so it can tell. Coherence is never what gets cut.
     */
    int r = rv9_io_open(CELL_B, RV9_MODE_READ);
    uint8_t small[sizeof(rv9_pub_t) + sizeof(int32_t)];
    check(rv9_io_read(r, small, sizeof(small), &done) == RV9_IO_OK,
          "a short buffer still reads");

    rv9_pub_t h;
    memcpy(&h, small, sizeof(h));
    int32_t first;
    memcpy(&first, small + sizeof(h), sizeof(first));
    check(done == sizeof(small) && h.len == 12 && first == 7,
          "and is told the whole length while being given what fits");

    check(rv9_io_read(r, &h, sizeof(rv9_pub_t) - 1, &done) == RV9_IO_ERR_INVAL,
          "a buffer too small even for the head is refused");

    rv9_io_close(r);
    rv9_io_close(w);
}

/*
 * Waiting. R9 §21 asks that a watcher re-evaluate when a value changes
 * rather than poll, so the wait is against a sequence and not an edge: a
 * publication that lands while nobody is waiting must still be noticed.
 */
static void waiting(void)
{
    int w = rv9_io_open(CELL_B, RV9_MODE_WRITE);
    int r = rv9_io_open(CELL_B, RV9_MODE_READ);
    if (w < 0 || r < 0) { check(false, "open a cell both ways"); return; }

    rv9_pub_wait_t wait = { .seq = 0, .timeout_ms = 0 };
    check(rv9_io_getstat(r, RV9_PUB_GS_WAIT, &wait) == RV9_IO_OK &&
          wait.seq == 2,
          "a wait from behind returns at once with where the cell is");

    check(rv9_io_getstat(r, RV9_PUB_GS_WAIT, &wait) == RV9_IO_ERR_TIMEOUT,
          "and a wait from the current sequence has nothing to report");

    /* Published while nobody was waiting. An edge would have been lost. */
    publish(w, 1, 1, 1, 0);
    wait.timeout_ms = 0;
    check(rv9_io_getstat(r, RV9_PUB_GS_WAIT, &wait) == RV9_IO_OK &&
          wait.seq == 3,
          "a publication nobody was waiting for is still seen afterwards");

    uint64_t t0 = rv9_time_ms();
    wait.timeout_ms = 60;
    rv9_io_err_t err = rv9_io_getstat(r, RV9_PUB_GS_WAIT, &wait);
    uint64_t waited = rv9_time_ms() - t0;
    check(err == RV9_IO_ERR_TIMEOUT && waited >= 50,
          "a blocking wait with nothing to see blocks, then gives up");

    rv9_io_close(r);
    rv9_io_close(w);
}

static void directory(void)
{
    int d = rv9_io_open("/pub0", RV9_MODE_READ);
    if (d < 0) { check(false, "the device is its own directory"); return; }
    check(true, "the device is its own directory");

    check(rv9_io_open("/pub0", RV9_MODE_WRITE) == -RV9_IO_ERR_MODE,
          "which cannot be written to");

    rv9_dirent_t ents[8];
    size_t done = 0;
    rv9_io_read(d, ents, sizeof(ents), &done);
    int n = (int)(done / sizeof(rv9_dirent_t));

    bool found_a = false, found_b = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(ents[i].name, "TESTONE") == 0) found_a = true;
        if (strcmp(ents[i].name, "TESTTWO") == 0) found_b = true;
    }
    check(found_a && found_b, "and lists every cell in use by name");

    rv9_io_close(d);
}

/*
 * Cells persist on purpose, so clearing one has to be possible on purpose
 * too -- otherwise a component that will never run again keeps a cell
 * until the machine is restarted.
 */
static void removal(void)
{
    int r = rv9_io_open(CELL_A, RV9_MODE_READ);
    check(rv9_io_remove(CELL_A) == RV9_IO_ERR_BUSY,
          "a cell somebody has open cannot be taken away");
    if (r >= 0) rv9_io_close(r);

    check(rv9_io_remove(CELL_A) == RV9_IO_OK, "and can be once they let go");
    check(rv9_io_remove(CELL_B) == RV9_IO_OK, "as can the other");
    check(rv9_io_remove(CELL_A) == RV9_IO_ERR_NOTFOUND,
          "removing it twice says so");
    check(rv9_io_open(CELL_A, RV9_MODE_READ) == -RV9_IO_ERR_NOTFOUND,
          "and the name is free again");
}

bool rv9_pub_selftest(void)
{
    s_passed = s_failed = 0;
    ESP_LOGI(TAG, "publication");

    naming();
    first_publication();
    one_writer();
    outlives_its_publisher();
    limits();
    waiting();
    directory();
    removal();

    if (s_failed == 0) {
        ESP_LOGI(TAG, "publication: %d/%d", s_passed, s_passed);
    } else {
        ESP_LOGE(TAG, "publication: %d passed, %d FAILED", s_passed, s_failed);
    }
    return s_failed == 0;
}
