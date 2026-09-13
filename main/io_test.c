/*
 * Device ownership, tested against the claim table directly.
 *
 * Every interesting case here is a conflict between two processes, and
 * arranging two real processes to race over a real pin at boot is both
 * slow and unreliable. The table is the thing that decides, so the table
 * is what gets asked -- with pids that do not exist and resource names no
 * device answers to, which is safe precisely because ownership is decided
 * above the drivers and never consults one.
 *
 * The names below start with "/test-" so that a leaked claim shows up in
 * `owns` as obviously test wreckage rather than a plausible device.
 */
#include "io_test.h"

#include "rv9/io.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "io-test";

static int s_passed;
static int s_failed;

static void check(bool ok, const char *what)
{
    if (ok) { s_passed++; ESP_LOGI(TAG, "  pass  %s", what); }
    else    { s_failed++; ESP_LOGE(TAG, "  FAIL  %s", what); }
}

#define RES_A "/test-a/1"
#define RES_B "/test-a/2"

/* Two pids that cannot collide with anything real: fork issues them from
   one upward and will not have reached here. */
#define PID_X 40001
#define PID_Y 40002

static void resource_names(void)
{
    char buf[RV9_CLAIM_NAME_MAX];

    rv9_claim_resource(buf, sizeof(buf), "/gpio", "2");
    check(strcmp(buf, "/gpio/2") == 0, "a unit joins its device with a slash");

    rv9_claim_resource(buf, sizeof(buf), "/term", "");
    check(strcmp(buf, "/term") == 0, "a device on its own gains no slash");

    /* A name longer than the buffer must be cut, not written past. */
    char small[8];
    rv9_claim_resource(small, sizeof(small), "/verylongdevice", "andmore");
    check(strlen(small) == sizeof(small) - 1, "an over-long name is truncated");
}

static void sharing(void)
{
    struct rv9_claim *x = NULL, *y = NULL;

    check(rv9_claim_take(RES_A, PID_X, false, &x) == RV9_IO_OK,
          "the first process may share a resource");
    check(rv9_claim_take(RES_A, PID_Y, false, &y) == RV9_IO_OK,
          "so may the second");

    struct rv9_claim *z = NULL;
    check(rv9_claim_take(RES_A, PID_Y, true, &z) == RV9_IO_ERR_BUSY,
          "but neither can then take it alone");
    check(z == NULL, "and a refused claim hands back nothing");

    rv9_pid_t owner = 0;
    bool excl = true;
    check(rv9_claim_owner(RES_A, &owner, &excl) && !excl,
          "a shared resource reports itself shared");

    rv9_claim_drop(y);
    check(rv9_claim_take(RES_A, PID_Y, true, &z) == RV9_IO_ERR_BUSY,
          "one holder left is still one holder");

    rv9_claim_drop(x);
    check(!rv9_claim_owner(RES_A, NULL, NULL),
          "dropping the last reference frees it");
}

static void exclusion(void)
{
    struct rv9_claim *x = NULL, *y = NULL;

    check(rv9_claim_take(RES_A, PID_X, true, &x) == RV9_IO_OK,
          "an unheld resource may be taken alone");
    check(rv9_claim_take(RES_A, PID_Y, false, &y) == RV9_IO_ERR_BUSY,
          "which shuts out even a shared opener");
    check(rv9_claim_take(RES_B, PID_Y, true, &y) == RV9_IO_OK,
          "a different unit on the same device is a different resource");

    rv9_pid_t owner = 0;
    bool excl = false;
    check(rv9_claim_owner(RES_A, &owner, &excl) && owner == PID_X && excl,
          "and the table names who has it");

    /* Taking it twice oneself is not a conflict with oneself. */
    struct rv9_claim *again = NULL;
    check(rv9_claim_take(RES_A, PID_X, true, &again) == RV9_IO_OK &&
          again == x, "the owner may open it again");

    rv9_claim_drop(again);
    check(rv9_claim_owner(RES_A, NULL, NULL),
          "and one close does not give it up");

    rv9_claim_drop(x);
    rv9_claim_drop(y);
    check(!rv9_claim_owner(RES_A, NULL, NULL) &&
          !rv9_claim_owner(RES_B, NULL, NULL), "both let go");
}

static void upgrading(void)
{
    struct rv9_claim *x = NULL, *up = NULL;

    check(rv9_claim_take(RES_A, PID_X, false, &x) == RV9_IO_OK,
          "hold a resource shared");
    check(rv9_claim_take(RES_A, PID_X, true, &up) == RV9_IO_OK && up == x,
          "the only holder may take it alone after all");

    struct rv9_claim *y = NULL;
    check(rv9_claim_take(RES_A, PID_Y, false, &y) == RV9_IO_ERR_BUSY,
          "and the upgrade shuts others out");

    rv9_claim_drop(up);
    rv9_claim_drop(x);
    check(!rv9_claim_owner(RES_A, NULL, NULL), "let go");
}

static void reservations(void)
{
    check(rv9_claim_reserve(RES_A, PID_X, true) == RV9_IO_OK,
          "a process may reserve a device before it starts");
    check(rv9_claim_reserve(RES_A, PID_Y, true) == RV9_IO_ERR_BUSY,
          "which refuses the next one at fork rather than at open");

    /* The same requirement stated twice is one requirement. */
    check(rv9_claim_reserve(RES_A, PID_X, true) == RV9_IO_OK,
          "reserving it twice is not an error");

    rv9_claim_release_pid(PID_X);
    check(!rv9_claim_owner(RES_A, NULL, NULL),
          "and one release gives it back, not two");
}

/*
 * The case the whole thing exists for: a process that reserved a device,
 * opened it, and then died. The reservation goes with the process; the
 * open goes with the path. Both have to be gone before anyone else can
 * have it, and neither may wait for the other.
 */
static void death(void)
{
    struct rv9_claim *open_path = NULL, *other = NULL;

    check(rv9_claim_reserve(RES_A, PID_X, true) == RV9_IO_OK, "reserved");
    check(rv9_claim_take(RES_A, PID_X, true, &open_path) == RV9_IO_OK,
          "and opened");

    /*
     * The exit hook releases reservations only. The path reference stands
     * for one the I/O manager has not closed yet -- in the real system,
     * one a child inherited -- and the resource is still open while it
     * does, which is the honest answer even though its owner is gone.
     */
    rv9_claim_release_pid(PID_X);
    check(rv9_claim_owner(RES_A, NULL, NULL),
          "the exit gives back the reservation but not an open path");
    check(rv9_claim_take(RES_A, PID_Y, false, &other) == RV9_IO_ERR_BUSY,
          "so nobody else has it yet");

    rv9_claim_drop(open_path);
    check(!rv9_claim_owner(RES_A, NULL, NULL),
          "closing the last path finishes it");

    check(rv9_claim_take(RES_A, PID_Y, true, &other) == RV9_IO_OK,
          "and the next process may have it");
    rv9_claim_drop(other);
    check(!rv9_claim_owner(RES_A, NULL, NULL), "nothing left behind");
}

/*
 * The mode bit, through a real open on a real device.
 *
 * Everything above talks to the claim table directly, which tests the
 * rules but not the wiring: RV9_MODE_EXCL has to survive the trip from a
 * module's open() through the I/O manager into a claim, and a bit that
 * quietly fails to arrive would leave every check above passing.
 *
 * /term is free at this point in boot -- the banner has not been written
 * and no shell exists -- and it is given back immediately.
 */
static void exclusive_open(void)
{
    int p = rv9_io_open("/term", RV9_MODE_WRITE | RV9_MODE_EXCL);
    if (p < 0) {
        check(false, "open /term exclusively");
        return;
    }
    check(true, "a device may be opened exclusively");

    struct rv9_claim *other = NULL;
    check(rv9_claim_take("/term", PID_Y, false, &other) == RV9_IO_ERR_BUSY,
          "and the mode bit reached the table: nobody else may share it");

    rv9_io_close(p);
    check(rv9_claim_take("/term", PID_Y, false, &other) == RV9_IO_OK,
          "closing it hands the device back");
    rv9_claim_drop(other);
}

bool rv9_io_selftest(void)
{
    s_passed = s_failed = 0;
    ESP_LOGI(TAG, "device ownership");

    resource_names();
    sharing();
    exclusion();
    upgrading();
    reservations();
    death();
    exclusive_open();

    if (s_failed == 0) {
        ESP_LOGI(TAG, "ownership: %d/%d", s_passed, s_passed);
    } else {
        ESP_LOGE(TAG, "ownership: %d passed, %d FAILED", s_passed, s_failed);
    }
    return s_failed == 0;
}
