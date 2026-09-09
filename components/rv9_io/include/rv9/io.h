/*
 * RV-9 unified I/O.
 *
 * This is the part of OS-9 most worth keeping. Every I/O operation goes
 * through the same four layers, and each layer is replaceable without
 * touching the others:
 *
 *   process          holds a path number
 *      |
 *   I/O manager      generic: open close read write seek getstat setstat
 *      |
 *   file manager     the *discipline* -- SCF for character streams, RBF for
 *      |             blocks, NFM for the network. Knows nothing of hardware.
 *   driver           the *hardware* -- uart, lcdcon, sdspi. Knows nothing of
 *      |             files, lines, or records.
 *   descriptor       the *binding* -- "/term is SCF over lcdcon, echo off"
 *
 * The split is the point. A new character device needs a driver and a
 * descriptor, and inherits line discipline, buffering and the whole generic
 * call surface for free. A new file manager serves every driver beneath it.
 *
 * getstat/setstat carry the device-specific operations that do not fit
 * read/write, which keeps the main path narrow. It is ioctl, but arrived at
 * deliberately rather than by accretion.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rv9/proc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RV9_IO_OK = 0,
    RV9_IO_ERR_NOTFOUND,     /* no such device */
    RV9_IO_ERR_BADPATH,      /* bad path number */
    RV9_IO_ERR_NOPATHS,      /* process path table full */
    RV9_IO_ERR_NOMEM,
    RV9_IO_ERR_MODE,         /* not open for that operation */
    RV9_IO_ERR_UNSUPPORTED,  /* driver or file manager cannot do this */
    RV9_IO_ERR_WOULDBLOCK,
    RV9_IO_ERR_IO,
    RV9_IO_ERR_INVAL,
    RV9_IO_ERR_EXISTS,
} rv9_io_err_t;

const char *rv9_io_strerror(rv9_io_err_t err);

/* Open modes (RV9_MODE_*) come from rv9/module.h -- they are ABI. */

/* Seek whence */
#define RV9_SEEK_SET 0
#define RV9_SEEK_CUR 1
#define RV9_SEEK_END 2

/* getstat/setstat codes. Low numbers are generic; drivers may define their
   own above RV9_SS_DRIVER_BASE. */
#define RV9_SS_ECHO        1   /* uint32: line echo on/off */
#define RV9_SS_AUTOLF      2   /* uint32: translate \n to \r\n on write */
#define RV9_GS_READY       3   /* uint32: bytes available to read */
#define RV9_GS_SIZE        4   /* uint64: size, for block devices */
#define RV9_SS_DRIVER_BASE 256

struct rv9_dev;
struct rv9_path;

/* ------------------------------------------------------------------ */
/* Driver interface -- hardware, and nothing else                      */
/* ------------------------------------------------------------------ */

typedef struct rv9_driver {
    const char *name;

    rv9_io_err_t (*init)(struct rv9_dev *dev);
    rv9_io_err_t (*term)(struct rv9_dev *dev);

    /* Return the number of bytes actually moved in *done. A driver may move
       fewer than asked; the file manager decides what that means. */
    rv9_io_err_t (*read)(struct rv9_dev *dev, void *buf, size_t len, size_t *done);
    rv9_io_err_t (*write)(struct rv9_dev *dev, const void *buf, size_t len, size_t *done);

    rv9_io_err_t (*getstat)(struct rv9_dev *dev, uint32_t code, void *arg);
    rv9_io_err_t (*setstat)(struct rv9_dev *dev, uint32_t code, void *arg);
} rv9_driver_t;

/* ------------------------------------------------------------------ */
/* File manager interface -- discipline, and nothing else              */
/* ------------------------------------------------------------------ */

typedef struct rv9_filemgr {
    const char *name;

    rv9_io_err_t (*open)(struct rv9_path *path);
    rv9_io_err_t (*close)(struct rv9_path *path);
    rv9_io_err_t (*read)(struct rv9_path *path, void *buf, size_t len, size_t *done);
    rv9_io_err_t (*write)(struct rv9_path *path, const void *buf, size_t len, size_t *done);
    rv9_io_err_t (*seek)(struct rv9_path *path, int64_t offset, int whence);
    rv9_io_err_t (*getstat)(struct rv9_path *path, uint32_t code, void *arg);
    rv9_io_err_t (*setstat)(struct rv9_path *path, uint32_t code, void *arg);
} rv9_filemgr_t;

/* ------------------------------------------------------------------ */
/* Device descriptor -- the binding                                    */
/* ------------------------------------------------------------------ */

/*
 * On-media form of a device descriptor. This is the body of a
 * RV9_MOD_DESCRIPTOR module, so adding a device is loading a module --
 * no kernel rebuild, exactly as OS-9 intended.
 */
typedef struct __attribute__((packed)) {
    char     name[16];       /* "/term" */
    char     filemgr[16];    /* "scf" */
    char     driver[16];     /* "lcdcon" */
    uint32_t opt[8];         /* meaning is the driver's and manager's own */
} rv9_devdesc_t;

_Static_assert(sizeof(rv9_devdesc_t) == 80, "device descriptor must be 80 bytes");

/* Live device: a descriptor bound to its file manager and driver. */
typedef struct rv9_dev {
    char                 name[16];
    const rv9_filemgr_t *fmgr;
    const rv9_driver_t  *drv;
    uint32_t             opt[8];

    void                *drv_state;   /* the driver's own */
    bool                 initialised;
    uint32_t             open_count;

    struct rv9_dev      *next;
} rv9_dev_t;

/* Path descriptor: one open connection to a device. */
typedef struct rv9_path {
    rv9_dev_t *dev;
    uint32_t   mode;
    int64_t    pos;
    void      *fm_state;     /* the file manager's own */
    uint32_t   refs;         /* shared when a child inherits it */
} rv9_path_t;

/* ------------------------------------------------------------------ */
/* I/O manager                                                         */
/* ------------------------------------------------------------------ */

rv9_io_err_t rv9_io_init(void);

rv9_io_err_t rv9_io_register_filemgr(const rv9_filemgr_t *fm);
rv9_io_err_t rv9_io_register_driver(const rv9_driver_t *drv);

/* Bind a descriptor to its manager and driver, creating the device. */
rv9_io_err_t rv9_io_attach(const rv9_devdesc_t *desc);

/* Load every RV9_MOD_DESCRIPTOR module in the module store and attach it.
   Returns how many devices came up. */
int rv9_io_attach_from_modules(void);

/* Generic calls. Path numbers are per-process. */
int          rv9_io_open(const char *name, uint32_t mode);   /* <0 on error */
rv9_io_err_t rv9_io_close(int path);
rv9_io_err_t rv9_io_read(int path, void *buf, size_t len, size_t *done);
rv9_io_err_t rv9_io_write(int path, const void *buf, size_t len, size_t *done);
rv9_io_err_t rv9_io_seek(int path, int64_t offset, int whence);
rv9_io_err_t rv9_io_getstat(int path, uint32_t code, void *arg);
rv9_io_err_t rv9_io_setstat(int path, uint32_t code, void *arg);

/* Convenience: write a NUL-terminated string. */
rv9_io_err_t rv9_io_dup2(int from, int to);
rv9_io_err_t rv9_io_puts(int path, const char *s);

const rv9_dev_t *rv9_io_dev_next(const rv9_dev_t *prev);   /* NULL to start */

/*
 * Standard path numbers, by convention rather than enforcement.
 * A forked process inherits whatever its parent had open on these.
 */
#define RV9_STDIN   0
#define RV9_STDOUT  1
#define RV9_STDERR  2

/* Set the paths a process with no parent inherits. */
rv9_io_err_t rv9_io_set_system_std(const char *in, const char *out);

#ifdef __cplusplus
}
#endif
