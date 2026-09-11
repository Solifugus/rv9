/*
 * RV-9 memory modules.
 *
 * A module is a self-contained, CRC-verified blob of position-independent
 * code that the system can find, verify, share and load at runtime. This is
 * the idea worth stealing from OS-9: code is a runtime object, not something
 * linked in at build time.
 *
 * Position independence is achieved by discipline rather than relocation:
 *
 *   - compiled -mcmodel=medany, so every internal reference is PC-relative
 *   - text and rodata are linked as ONE blob that moves as a unit
 *   - no external symbols; everything the module needs arrives through the
 *     environment pointer handed to its entry point
 *   - no writable static data in the module image; per-instance state lives
 *     in a separate area allocated by the loader
 *
 * That last pair is straight OS-9: pure reentrant code shared between
 * processes, with static storage per process. The 6809 passed it in U and
 * the 68000 in A6; we pass it in the environment struct.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RV9_MODULE_MAGIC   0x4D395652u   /* "RV9M" little-endian */
#define RV9_MODULE_ABI     10
#define RV9_MODULE_HDR_LEN 40

/* Module types. Only PROGRAM is loadable in phase 1; the rest are declared
   now because the I/O system in phase 3 is built from them. */
typedef enum {
    RV9_MOD_PROGRAM    = 1,
    RV9_MOD_LIBRARY    = 2,
    RV9_MOD_FILEMGR    = 3,
    RV9_MOD_DRIVER     = 4,
    RV9_MOD_DESCRIPTOR = 5,
    RV9_MOD_DATA       = 6,
    RV9_MOD_SYSTEM     = 7,
} rv9_mod_type_t;

/*
 * On-media module header. Little-endian, 40 bytes.
 *
 * crc32 covers the entire module image with the crc32 field itself taken
 * as zero -- standard CRC-32 (the zlib polynomial), so tools/mkmodule.py
 * can compute it with Python's zlib.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /* RV9_MODULE_MAGIC */
    uint16_t header_len;    /* RV9_MODULE_HDR_LEN */
    uint16_t abi_version;   /* RV9_MODULE_ABI */
    uint32_t module_len;    /* total bytes, header included */
    uint32_t name_offset;   /* NUL-terminated name, from module start */
    uint32_t entry_offset;  /* entry point, from module start */
    uint32_t static_size;   /* per-instance storage the loader must provide */
    uint32_t stack_size;    /* hint; 0 means "loader decides" */
    uint8_t  type;          /* rv9_mod_type_t */
    uint8_t  attr;          /* reserved for flags */
    uint8_t  revision;      /* higher revision wins when names collide */
    uint8_t  reserved0;
    uint32_t crc32;
    uint32_t reserved1;
} rv9_mod_header_t;

_Static_assert(sizeof(rv9_mod_header_t) == RV9_MODULE_HDR_LEN,
               "module header must be exactly 40 bytes");

/* ------------------------------------------------------------------ */
/* Module ABI -- what a module receives when it runs                   */
/* ------------------------------------------------------------------ */

/*
 * Everything a module may touch arrives through this struct. A module has
 * no other way to reach the system: no libc, no globals, no imports.
 *
 * This is deliberately tiny in phase 1. It grows into the real system-call
 * surface as the I/O manager (phase 3) and process manager (phase 2) land.
 * Appending fields is compatible; reordering or removing them is not, and
 * must bump RV9_MODULE_ABI.
 */
/*
 * What a real-time process can find out about its own timing.
 *
 * Exposed to the application deliberately: "real-time" is a property you
 * measure, and a control loop that cannot see its own jitter cannot report
 * that it has stopped being trustworthy.
 */
typedef struct __attribute__((packed)) {
    uint32_t period_us;
    uint32_t activations;
    uint32_t overruns;
    uint32_t max_jitter_us;
    uint32_t max_exec_us;
    uint32_t last_exec_us;
} rv9_rt_report_t;

typedef struct {
    /* --- ABI 1 --- */
    uint32_t    abi_version;
    void       *statics;       /* zeroed, static_size bytes, per instance */
    uint32_t    statics_size;
    int       (*print)(const char *s);       /* stand-in until SCF exists */
    uint64_t  (*time_ms)(void);

    /* --- ABI 2: running as a process --- */
    uint32_t    pid;           /* 0 when run outside a process */
    const char *arg;           /* may be NULL */
    void      (*yield)(void);
    void      (*sleep_ms)(uint32_t ms);
    uint32_t  (*signals_take)(void);  /* pending signals, cleared by reading */

    /* --- ABI 3: unified I/O --- */
    /* All return >= 0 on success (a path number, or a byte count), and a
       negative rv9_io_err_t on failure. */
    int       (*open)(const char *name, uint32_t mode);
    int       (*close)(int path);
    int       (*read)(int path, void *buf, uint32_t len);
    int       (*write)(int path, const void *buf, uint32_t len);

    /* --- ABI 4: starting other modules --- */
    /* fork returns a pid, or negative on failure. wait blocks for it and
       fills *status with the module's return value. */
    int       (*fork)(const char *module, int priority);
    int       (*wait)(int pid, int *status, uint32_t timeout_ms);

    /* --- ABI 5: asking the system about itself, and redirection --- */
    /* sysinfo fills buf with records of the requested kind and returns how
       many it wrote, or negative on error. */
    int       (*sysinfo)(uint32_t what, void *buf, uint32_t len);
    /* Make path `to` refer to whatever `from` refers to. This is how a
       shell redirects: it points its own stdout somewhere else, forks, and
       puts it back. */
    int       (*dup2)(int from, int to);

    /* --- ABI 6: chain --- */
    /*
     * Replace the running module with another, keeping this pid, its open
     * paths and its priority. Returns 0 if the request was accepted; the
     * module should then return from its entry point, and the new module
     * runs in its place. OS-9 called this F$Chain.
     *
     * It is not exec(): the old module returns normally first, so it can
     * clean up. The process simply continues as something else.
     */
    int       (*chain)(const char *module);

    /* --- ABI 7: files, and passing arguments --- */
    int       (*remove)(const char *name);
    /* fork with an argument, which arrives as the child's env->arg. The
       plain fork above stays for callers with nothing to say. */
    int       (*fork_arg)(const char *module, int priority, const char *arg);

    /* --- ABI 8: the rest of the generic call surface --- */
    /*
     * getstat and setstat were missing, which meant a module could read and
     * write a device but not configure one. They are first-class in the I/O
     * design (see rv9/io.h) and should always have been here.
     */
    int       (*seek)(int path, int32_t offset, int whence);
    int       (*getstat)(int path, uint32_t code, void *arg);
    int       (*setstat)(int path, uint32_t code, void *arg);

    /* --- ABI 9: adding a program at runtime --- */
    /*
     * Read a module from a path and add it to the module directory, after
     * which it is a command like any other. This is what turns the module
     * store from something you reflash into something you add to.
     */
    int       (*load)(const char *path);

    /* --- ABI 10: real-time control --- */
    /*
     * Declare this process periodic, then wait for each period.
     *
     *     env->rt_declare(1000);          // 1 kHz
     *     for (;;) {
     *         read_sensors(); compute(); drive_actuators();
     *         int late = env->rt_wait();  // 0 when on time
     *         if (late) ...               // fell behind; decide what that means
     *     }
     *
     * rt_wait returns how many periods elapsed while the loop was still
     * working. A control loop that silently misses deadlines is worse than
     * one that stops, so the number is handed back rather than absorbed.
     *
     * Only a process forked into the real-time class may use these.
     */
    int       (*rt_declare)(uint32_t period_us);
    int       (*rt_wait)(void);
    int       (*rt_stats)(rv9_rt_report_t *out);
    /* Fork another module into the real-time class. */
    int       (*fork_rt)(const char *module, uint32_t period_us,
                         const char *arg);
} rv9_mod_env_t;

/* Seek whence, matching the I/O manager. */
#define RV9_SEEK_SET 0
#define RV9_SEEK_CUR 1
#define RV9_SEEK_END 2

/* Generic getstat/setstat codes a module may use. */
#define RV9_SS_ECHO        1
#define RV9_SS_AUTOLF      2
#define RV9_GS_READY       3
#define RV9_GS_SIZE        4
#define RV9_SS_DRIVER_BASE 256

/*
 * Peripheral settings, shared by every PIO device so that a program does
 * not need to know which driver is underneath.
 */
#define RV9_PIO_SS_DIRECTION  16   /* 0 = input, 1 = output */
#define RV9_PIO_SS_PULL       17   /* 0 = none, 1 = up, 2 = down */
#define RV9_PIO_SS_FREQUENCY  18   /* Hz, for anything periodic */
#define RV9_PIO_GS_RANGE      19   /* largest value a write may carry */

/* The LCD console's own settings. */
#define RV9_LCD_SS_CLEAR      (RV9_SS_DRIVER_BASE + 0)
#define RV9_LCD_SS_BRIGHTNESS (RV9_SS_DRIVER_BASE + 1)   /* 0..100 percent */

/* ------------------------------------------------------------------ */
/* sysinfo                                                             */
/* ------------------------------------------------------------------ */

#define RV9_SYS_MEM      1
#define RV9_SYS_MODULES  2
#define RV9_SYS_PROCS    3

typedef struct __attribute__((packed)) {
    uint32_t heap_free;
    uint32_t heap_low_water;
    uint32_t heap_exec_free;
    uint32_t module_count;
    uint32_t proc_count;
} rv9_sys_mem_t;

typedef struct __attribute__((packed)) {
    char     name[32];
    uint8_t  type;
    uint8_t  revision;
    uint16_t reserved;
    uint32_t size;
    uint32_t links;
} rv9_sys_module_t;

typedef struct __attribute__((packed)) {
    uint16_t pid;
    uint16_t parent;
    char     name[32];
    uint8_t  state;
    int8_t   base_priority;
    int8_t   effective_priority;
    int8_t   reserved;
    int32_t  status;
} rv9_sys_proc_t;

typedef int (*rv9_mod_entry_fn)(const rv9_mod_env_t *env);

/*
 * Signals a process may be sent. Deliberately minimal: enough to ask a
 * process to stop cooperatively. A module polls with env->signals_take(),
 * which returns and clears whatever is pending.
 */
/* Standard path numbers. A forked process inherits its parent's. */
#define RV9_STDIN   0
#define RV9_STDOUT  1
#define RV9_STDERR  2

/* Open modes, shared by the module ABI and the I/O manager. */
#define RV9_MODE_READ   (1u << 0)
#define RV9_MODE_WRITE  (1u << 1)
#define RV9_MODE_RW     (RV9_MODE_READ | RV9_MODE_WRITE)
#define RV9_MODE_CREATE (1u << 2)   /* make it if absent, truncate if not */

/*
 * A directory is just a file whose records are these. Open a block device
 * with no filename -- "/r0" rather than "/r0/notes" -- and reads return
 * directory entries. This is ABI: modules read them.
 */
typedef struct __attribute__((packed)) {
    char     name[28];
    uint32_t size;
} rv9_dirent_t;

_Static_assert(sizeof(rv9_dirent_t) == 32, "directory entry must be 32 bytes");

#define RV9_SIG_STOP  (1u << 0)
#define RV9_SIG_USER1 (1u << 1)
#define RV9_SIG_USER2 (1u << 2)

/* ------------------------------------------------------------------ */
/* Module directory                                                    */
/* ------------------------------------------------------------------ */

/* One entry per module known to the system, whether loaded or not. */
typedef struct rv9_mod_entry {
    char                  name[32];
    uint8_t               type;
    uint8_t               revision;
    uint32_t              size;         /* module_len */
    uint32_t              store_offset; /* where it lives in the module store */
    uint32_t              link_count;   /* processes holding it */
    void                 *image;        /* RAM image, NULL when not loaded */
    bool                  resident;     /* image is permanent, not from store */
    rv9_mod_entry_fn      entry;        /* valid while loaded */
    struct rv9_mod_entry *next;
} rv9_mod_entry_t;

typedef enum {
    RV9_MOD_OK = 0,
    RV9_MOD_ERR_NOTFOUND,
    RV9_MOD_ERR_BADMAGIC,
    RV9_MOD_ERR_BADCRC,
    RV9_MOD_ERR_BADABI,
    RV9_MOD_ERR_NOMEM,
    RV9_MOD_ERR_IO,
    RV9_MOD_ERR_INVAL,
} rv9_mod_err_t;

const char *rv9_mod_strerror(rv9_mod_err_t err);

/* Scan the module store and build the directory. Safe to call once at boot.
   Returns the number of valid modules found. */
int rv9_mod_dir_init(void);

/* Walk the directory. Pass NULL to start. */
const rv9_mod_entry_t *rv9_mod_dir_next(const rv9_mod_entry_t *prev);

const rv9_mod_entry_t *rv9_mod_find(const char *name);

/*
 * Load a module into executable memory and take a link on it. Repeated
 * links share one image -- this is why module code must be reentrant.
 */
rv9_mod_err_t rv9_mod_link(const char *name, rv9_mod_entry_t **out_entry);

/* Drop a link. The image is freed when the count reaches zero. */
rv9_mod_err_t rv9_mod_unlink(rv9_mod_entry_t *entry);

/*
 * Run a linked module: allocates and zeroes its static storage, builds the
 * environment, calls the entry point, frees the storage.
 *
 * Phase 2 replaces this with fork(), where the process manager owns the
 * static area for the process's lifetime.
 */
rv9_mod_err_t rv9_mod_run(rv9_mod_entry_t *entry, int *out_result);

/*
 * The I/O manager registers itself here so that modules can be given I/O
 * without rv9_module having to depend on rv9_io. The dependency runs one
 * way -- rv9_io knows about rv9_module, never the reverse -- and this is
 * the seam that keeps it that way.
 */
typedef struct {
    int (*open)(const char *name, uint32_t mode);
    int (*close)(int path);
    int (*read)(int path, void *buf, uint32_t len);
    int (*write)(int path, const void *buf, uint32_t len);
    int (*dup2)(int from, int to);
    int (*remove)(const char *name);
    int (*seek)(int path, int32_t offset, int whence);
    int (*getstat)(int path, uint32_t code, void *arg);
    int (*setstat)(int path, uint32_t code, void *arg);
} rv9_mod_io_ops_t;

void rv9_mod_set_io_ops(const rv9_mod_io_ops_t *ops);

/* Same arrangement for the process manager, for the same reason. */
typedef struct {
    int (*fork)(const char *module, int priority);
    int (*wait)(int pid, int *status, uint32_t timeout_ms);
    int (*procs)(void *buf, uint32_t len);   /* fills rv9_sys_proc_t records */
    int (*chain)(const char *module);
    int (*fork_arg)(const char *module, int priority, const char *arg);
    int (*fork_rt)(const char *module, uint32_t period_us, const char *arg);
} rv9_mod_proc_ops_t;

void rv9_mod_set_proc_ops(const rv9_mod_proc_ops_t *ops);

/*
 * Fill in the environment handed to a module. One place builds it so that
 * a module cannot tell whether it was started by rv9_mod_run or by fork.
 * The caller may override individual fields afterwards -- the process
 * manager replaces signals_take with its own.
 */
void rv9_mod_env_init(rv9_mod_env_t *env, void *statics,
                      uint32_t statics_size, uint32_t pid);

/* CRC-32 (zlib polynomial), exposed because the loader and the host tool
   must agree on it exactly. */
uint32_t rv9_crc32(uint32_t crc, const void *data, size_t len);

/* Verify a module image already in memory. */
rv9_mod_err_t rv9_mod_verify(const void *image, size_t avail);

/*
 * Add a module from an image in memory rather than from the store.
 *
 * The image is verified, copied into executable memory and kept there --
 * a resident module has no store to be re-read from, so unlinking it frees
 * its links but not its image.
 */
rv9_mod_err_t rv9_mod_register_image(const void *image, uint32_t len);

/*
 * Read a module from a path and add it to the directory. Used by the
 * `load` command and by boot, which loads whatever a volume is carrying.
 */
rv9_mod_err_t rv9_mod_load_path(const char *path);

#ifdef __cplusplus
}
#endif
