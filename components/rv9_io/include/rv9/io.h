/*
 * RV-9 unified I/O.
 *
 * This is the part of the classic OS-9 design most worth learning from. Every I/O operation goes
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
    RV9_IO_ERR_TIMEOUT,      /* appended: existing values are ABI, they
                                travel to modules as negative returns */
    RV9_IO_ERR_BUSY,         /* somebody else owns it -- see claims, below */
} rv9_io_err_t;

const char *rv9_io_strerror(rv9_io_err_t err);

/* Open modes (RV9_MODE_*) come from rv9/module.h -- they are ABI. */

/* Seek whence (RV9_SEEK_*) comes from rv9/module.h -- it is ABI. */

/* rv9_dirent_t comes from rv9/module.h -- modules read them too. */

/* getstat/setstat codes. Low numbers are generic; drivers may define their
   own above RV9_SS_DRIVER_BASE. */
/* Generic codes (RV9_SS_ECHO and friends) come from rv9/module.h. */

struct rv9_dev;
struct rv9_path;

/* ------------------------------------------------------------------ */
/* Driver interface -- hardware, and nothing else                      */
/* ------------------------------------------------------------------ */

typedef struct rv9_driver {
    const char *name;

    /*
     * Does a unit keep its state after the last path to it closes?
     *
     * True for /gpio, which holds its level deliberately -- setting an
     * enable line and having it drop when the command finished would be
     * useless. False for /pwm0, equally deliberately: it holds a hardware
     * channel that must be given back, and an actuator still running
     * because a program exited is a bad surprise.
     *
     * The distinction was always there in the drivers. It is declared here
     * because a declared failsafe outlives its program only on a device of
     * the first kind, and admission should be able to say so rather than
     * letting the author find out from the machine.
     */
    bool retains;

    rv9_io_err_t (*init)(struct rv9_dev *dev);
    rv9_io_err_t (*term)(struct rv9_dev *dev);

    /*
     * Called when the device goes from nobody-using-it to somebody, and
     * back. Optional, and most drivers want nothing to do with it: a UART
     * is ready from `init` and stays ready.
     *
     * It exists for a device that cannot be ready in advance. `ssh` is the
     * first: being open means a client has connected, been through key
     * exchange and proved who it is, none of which can happen at boot with
     * nobody there. So the work happens on the first open, which may block
     * for as long as it takes somebody to connect -- the same shape as
     * opening a listening path, arrived at from the other side.
     */
    rv9_io_err_t (*open)(struct rv9_dev *dev, uint32_t mode);
    rv9_io_err_t (*close)(struct rv9_dev *dev);

    /* Return the number of bytes actually moved in *done. A driver may move
       fewer than asked; the file manager decides what that means. */
    rv9_io_err_t (*read)(struct rv9_dev *dev, void *buf, size_t len, size_t *done);
    rv9_io_err_t (*write)(struct rv9_dev *dev, const void *buf, size_t len, size_t *done);

    rv9_io_err_t (*getstat)(struct rv9_dev *dev, uint32_t code, void *arg);
    rv9_io_err_t (*setstat)(struct rv9_dev *dev, uint32_t code, void *arg);

    /*
     * The session is being ended while calls are still inside the driver.
     *
     * Make them return, promptly and without freeing anything: they are
     * still using it. `ssh` shuts its socket so a read waiting for a client
     * that will never type again gets an answer. Called before `close`,
     * which does the freeing once nothing is inside. Optional; a driver
     * without it is waited on for a while and then abandoned rather than
     * freed under its callers.
     */
    rv9_io_err_t (*hangup)(struct rv9_dev *dev);

    /*
     * Block devices implement these instead of read/write. A driver is one
     * kind or the other: character drivers move bytes as they arrive, block
     * drivers move whole sectors at an address. Pretending one is the other
     * is how storage stacks end up unpleasant.
     */
    rv9_io_err_t (*geometry)(struct rv9_dev *dev, uint32_t *sector_size,
                             uint32_t *sector_count);
    rv9_io_err_t (*read_blocks)(struct rv9_dev *dev, uint32_t lsn,
                                void *buf, uint32_t count);
    rv9_io_err_t (*write_blocks)(struct rv9_dev *dev, uint32_t lsn,
                                 const void *buf, uint32_t count);

    /*
     * Peripheral drivers implement these: a third shape, after character
     * streams and block devices.
     *
     * A pin, a PWM output or an ADC channel is not a stream of bytes and
     * not an array of sectors. It is an addressable unit with a value, and
     * pretending otherwise would mean a control loop formatting decimal to
     * move a servo. `unit` is whatever the device numbers its units by --
     * a GPIO number, a channel.
     */
    rv9_io_err_t (*unit_open)(struct rv9_dev *dev, uint32_t unit,
                              uint32_t mode, void **unit_state);
    rv9_io_err_t (*unit_close)(struct rv9_dev *dev, void *unit_state);
    rv9_io_err_t (*unit_read)(struct rv9_dev *dev, void *unit_state,
                              uint32_t *value);
    rv9_io_err_t (*unit_write)(struct rv9_dev *dev, void *unit_state,
                               uint32_t value);
    rv9_io_err_t (*unit_stat)(struct rv9_dev *dev, void *unit_state,
                              bool set, uint32_t code, uint32_t *value);

    /*
     * Where this device's memory is. A fourth shape, and the smallest.
     *
     * The publication file manager needs somewhere to keep cells that
     * outlives every process touching them, and nothing else: no
     * registers, no interrupts, no transfer. So its driver has one job --
     * say where the store is and how big -- and the whole discipline of
     * naming, sequencing and coherence stays above it, in the file
     * manager, where discipline belongs.
     *
     * It is a driver entry point rather than a malloc in the file manager
     * because the store is the part that will vary: a cell in a PMP region
     * shared with an isolated process, or one in memory that survives a
     * restart, is a different driver answering the same call.
     */
    rv9_io_err_t (*arena)(struct rv9_dev *dev, void **base, uint32_t *size);
} rv9_driver_t;

/* ------------------------------------------------------------------ */
/* File manager interface -- discipline, and nothing else              */
/* ------------------------------------------------------------------ */

typedef struct rv9_filemgr {
    const char *name;

    /* `rest` is whatever followed the device name: "notes" for "/r0/notes",
       and "" for "/r0" itself. SCF ignores it; RBF treats it as a filename
       and an empty one as the directory. */
    rv9_io_err_t (*open)(struct rv9_path *path, const char *rest);
    rv9_io_err_t (*close)(struct rv9_path *path);
    rv9_io_err_t (*read)(struct rv9_path *path, void *buf, size_t len, size_t *done);
    rv9_io_err_t (*write)(struct rv9_path *path, const void *buf, size_t len, size_t *done);
    rv9_io_err_t (*seek)(struct rv9_path *path, int64_t offset, int whence);
    rv9_io_err_t (*getstat)(struct rv9_path *path, uint32_t code, void *arg);
    rv9_io_err_t (*setstat)(struct rv9_path *path, uint32_t code, void *arg);

    /* Remove a named thing: a file on a volume, a cell on a publication
       device. Managers whose names are not removable leave it NULL. */
    rv9_io_err_t (*remove)(struct rv9_dev *dev, const char *name);

    /* Called once when the device is attached, so the manager can mount. */
    rv9_io_err_t (*mount)(struct rv9_dev *dev);

    /*
     * Ownership a file manager enforces itself, declared in a manifest.
     *
     * The claim table decides who may open a device at all. Some managers
     * have a finer rule of their own -- PFM's one writer and many readers
     * per cell -- that the claim table cannot express, and a manifest can
     * still declare it. All three are optional; a manager that leaves them
     * NULL is one whose names cannot be declared, and admission says so.
     */

    /* Reserve `rest` for writing by `pid`, at fork. BUSY if another has it. */
    rv9_io_err_t (*reserve_writer)(struct rv9_dev *dev, const char *rest,
                                   rv9_pid_t pid);

    /* Does `rest` exist -- is it something a reader could open now? */
    bool (*provided)(struct rv9_dev *dev, const char *rest);

    /*
     * A process has ended. Called with fault 0 from the exit path, where it
     * drops what the process reserved, and again with its RV9_FAULT_* once
     * the process table has recorded one -- after its failsafes.
     */
    void (*ended)(struct rv9_dev *dev, rv9_pid_t pid, int fault);
} rv9_filemgr_t;

/* ------------------------------------------------------------------ */
/* Device descriptor -- the binding                                    */
/* ------------------------------------------------------------------ */

/*
 * On-media form of a device descriptor. This is the body of a
 * RV9_MOD_DESCRIPTOR module, so adding a device is loading a module --
 * no kernel rebuild.
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

    void                *drv_state;    /* the driver's own */
    void                *fmgr_state;   /* the file manager's own, e.g. a mount */
    bool                 initialised;
    uint32_t             open_count;

    /*
     * Sessions, for a device whose driver has one (drv->open != NULL).
     *
     * `session` names the current one; a path remembers the session it was
     * opened in, and a path from an ended session gets RV9_IO_ERR_IO from
     * then on without reaching the driver. `busy` counts calls inside the
     * driver right now, so a session is never torn down under one.
     * `hanging` refuses new opens while a hangup is still in progress.
     */
    uint32_t             session;
    uint32_t             busy;
    bool                 hanging;

    struct rv9_dev      *next;
} rv9_dev_t;

/* Path descriptor: one open connection to a device. */
typedef struct rv9_path {
    rv9_dev_t *dev;
    char       name[28];     /* the part after the device, "" for the device */
    uint32_t   mode;
    int64_t    pos;
    void      *fm_state;     /* the file manager's own */
    uint32_t   refs;         /* shared when a child inherits it */
    uint32_t   session;      /* the device's session when this was opened */

    /* The ownership record this path holds a reference on, released when
       the last reference to the path goes. NULL for a path opened before
       the claim table existed. */
    struct rv9_claim *claim;
} rv9_path_t;

/* ------------------------------------------------------------------ */
/* Ownership                                                           */
/*                                                                     */
/* Above the drivers, because a driver is about hardware and this is a  */
/* question about programs. Below the programs, because the answer must */
/* be the same for all of them. See claim.c for the reasoning.          */
/* ------------------------------------------------------------------ */

/* Long enough for a device name and the longest thing a file manager will
   accept after it: 16 + '/' + 28, rounded up. */
#define RV9_CLAIM_NAME_MAX 48

struct rv9_claim;

rv9_io_err_t rv9_claim_init(void);

/* Build "/gpio" + "2" into "/gpio/2", the form everything else compares. */
void rv9_claim_resource(char *out, size_t cap, const char *dev,
                        const char *rest);

/*
 * Claim a resource for the duration of an open. RV9_IO_ERR_BUSY when
 * somebody else has it and either party wants it alone. `owner` may be
 * RV9_PID_NONE, which means a driver holds it on the system's behalf and
 * no process exit will take it away.
 */
rv9_io_err_t rv9_claim_take(const char *resource, rv9_pid_t owner,
                            bool exclusive, struct rv9_claim **out);
void         rv9_claim_drop(struct rv9_claim *c);

/*
 * Claim a resource for a process's whole life, from its manifest, before
 * it starts. Released by rv9_claim_release_pid however the process ends.
 */
rv9_io_err_t rv9_claim_reserve(const char *resource, rv9_pid_t owner,
                               bool exclusive);
void         rv9_claim_release_pid(rv9_pid_t owner);

/*
 * Where a device is to be left when its owner stops.
 *
 * Recorded against an existing claim, so it can only be set by a process
 * that already owns the resource -- which is the whole of the check.
 * RV9_IO_ERR_NOTFOUND means the caller does not own it.
 */
rv9_io_err_t rv9_claim_failsafe(const char *resource, rv9_pid_t owner,
                                uint32_t value);

typedef struct {
    char     name[RV9_CLAIM_NAME_MAX];
    uint32_t value;
} rv9_claim_fs_t;

/* Snapshot what this owner promised to park. Counts when out is NULL. */
int rv9_claim_failsafes(rv9_pid_t owner, rv9_claim_fs_t *out, int max);

/* Fill records, or count them when out is NULL. */
int  rv9_claim_list(rv9_sys_claim_t *out, int max);
bool rv9_claim_owner(const char *resource, rv9_pid_t *out_owner,
                     bool *out_exclusive);

/* ------------------------------------------------------------------ */
/* Console settings as escape sequences                                */
/*                                                                     */
/* For devices with a terminal at the far end rather than a screen of   */
/* their own. Writes the sequence for one RV9_CON_* setstat into buf    */
/* and returns its length, or 0 if the code is not a console code or    */
/* would not fit. 32 bytes is plenty for any of them.                   */
/*                                                                     */
/* Shared by SCF and the network file manager: a shell over TCP is      */
/* driving somebody's terminal too.                                     */
/* ------------------------------------------------------------------ */

#define RV9_CON_ANSI_MAX 32

size_t rv9_con_ansi(char *buf, size_t cap, uint32_t code, uint32_t value);

/*
 * How big is a screen we cannot ask?
 *
 * A serial line does not carry its terminal's dimensions, and asking over
 * the wire means writing a query and reading a reply back through a line
 * discipline that is busy being a shell. So a device whose driver cannot
 * say gets told 80x24 -- the size everything has defaulted to since the
 * VT100, and a guess that is stated rather than hidden. A session that
 * knows better (ssh, one day) is the right place to fix it.
 */
#define RV9_CON_DEFAULT_COLS 80
#define RV9_CON_DEFAULT_ROWS 24

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

/*
 * A path that belongs to nobody.
 *
 * Every path above is in some process's table, which is right for a
 * process: it opened the thing, it should lose it when it exits, and a
 * child should inherit it. A *driver* that opens a path has none of those
 * properties. `ssh` runs over a network connection it opened itself, and
 * that connection has to outlive the process that opened `/ssh0` -- the
 * daemon forks a shell, and only the child's standard paths are inherited,
 * so a path number would be meaningless in the process actually doing the
 * reading.
 *
 * So a detached path is held by pointer instead of by number, and lives
 * until the holder closes it. It is the mechanism a stacking driver needs:
 * one device implemented over another, with the layer below reached the
 * same way everything else is reached.
 */
rv9_io_err_t rv9_io_open_detached(const char *name, uint32_t mode,
                                  rv9_path_t **out);
rv9_io_err_t rv9_io_read_path(rv9_path_t *p, void *buf, size_t len, size_t *done);
rv9_io_err_t rv9_io_write_path(rv9_path_t *p, const void *buf, size_t len,
                               size_t *done);
rv9_io_err_t rv9_io_setstat_path(rv9_path_t *p, uint32_t code, void *arg);
void         rv9_io_close_path(rv9_path_t *p);

/* Remove a file, e.g. "/r0/notes". */
rv9_io_err_t rv9_io_remove(const char *name);
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
