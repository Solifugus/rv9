#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef enum { RV9_IO_OK=0, RV9_IO_ERR_NOTFOUND, RV9_IO_ERR_BADPATH,
  RV9_IO_ERR_NOPATHS, RV9_IO_ERR_NOMEM, RV9_IO_ERR_MODE, RV9_IO_ERR_UNSUPPORTED,
  RV9_IO_ERR_WOULDBLOCK, RV9_IO_ERR_IO, RV9_IO_ERR_INVAL, RV9_IO_ERR_EXISTS,
  RV9_IO_ERR_TIMEOUT } rv9_io_err_t;
/*
 * Mirrored from rv9/module.h, which is not on this include path.
 *
 * A hazard worth naming: these are copies, and nothing checks that they
 * still match. If a code is renumbered there and not here, the host test
 * goes on passing while exercising a different code than the firmware uses.
 * Kept because pulling the real header in drags the whole module ABI into a
 * build that only wants a driver -- but the hazard is no longer only a
 * comment: tools/checkstub.py reads both files and fails run.sh if any name
 * below disagrees with rv9/module.h. The list may grow; it may not drift.
 */
#define RV9_GS_SIZE 4
#define RV9_SS_DRIVER_BASE 256
#define RV9_SVG_SS_ROWS (RV9_SS_DRIVER_BASE + 8)
#define RV9_SVG_GS_LIMITS (RV9_SS_DRIVER_BASE + 9)
#define RV9_GS_PHYSICAL 7
#define RV9_PHYS_UNKNOWN 0
#define RV9_PHYS_FIXED 1
#define RV9_PHYS_VARIABLE 2
typedef struct __attribute__((packed)) {
  uint32_t width_px, height_px, width_um, height_um; uint8_t kind, reserved[3];
} rv9_physical_t;
typedef struct __attribute__((packed)) {
  uint32_t src_max, pts_max, contours_max, depth_max;
} rv9_svg_limits_t;
struct rv9_dev;
typedef struct rv9_driver {
  const char *name;
  rv9_io_err_t (*init)(struct rv9_dev *);
  rv9_io_err_t (*term)(struct rv9_dev *);
  rv9_io_err_t (*open)(struct rv9_dev *, uint32_t);
  rv9_io_err_t (*close)(struct rv9_dev *);
  rv9_io_err_t (*read)(struct rv9_dev *, void *, size_t, size_t *);
  rv9_io_err_t (*write)(struct rv9_dev *, const void *, size_t, size_t *);
  rv9_io_err_t (*getstat)(struct rv9_dev *, uint32_t, void *);
  rv9_io_err_t (*setstat)(struct rv9_dev *, uint32_t, void *);
} rv9_driver_t;
typedef struct rv9_dev { char name[16]; uint32_t opt[8]; void *drv_state; } rv9_dev_t;
static inline rv9_io_err_t rv9_io_register_driver(const rv9_driver_t *d){(void)d;return RV9_IO_OK;}
