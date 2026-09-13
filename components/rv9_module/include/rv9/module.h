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
#define RV9_MODULE_ABI     12
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
    uint32_t manifest_offset; /* TLV manifest, from module start; 0 = none */
} rv9_mod_header_t;

_Static_assert(sizeof(rv9_mod_header_t) == RV9_MODULE_HDR_LEN,
               "module header must be exactly 40 bytes");

/* ------------------------------------------------------------------ */
/* The manifest                                                        */
/* ------------------------------------------------------------------ */

/*
 * What a program says it needs, before RV-9 agrees to run it.
 *
 * The fixed header holds four numbers -- static size, stack hint, type,
 * entry -- and they were never going to be enough. A resource contract
 * wants heap ceiling, execution class, period, deadline, minimum
 * inter-arrival, required devices, exclusive versus shared ownership,
 * failsafe state, capabilities, which compiler built it. Adding each of
 * those to the header in turn means breaking every module in the store,
 * in turn.
 *
 * So the header points at an optional list of tagged values instead, laid
 * out after the name and before the code. A module with no manifest has
 * manifest_offset == 0, which is every module built before this existed.
 *
 * Layout: entries back to back, each 4-byte aligned, ending at a tag of
 * RV9_MTAG_END or at the end of the module.
 *
 *     uint16_t tag
 *     uint16_t len        bytes of value, padding not counted
 *     uint8_t  value[len]
 *     padding to the next multiple of four
 *
 * ---- advisory and mandatory ----
 *
 * An unknown tag is normally skipped: that is what makes the format worth
 * having, because a compiler can emit tomorrow's field into today's
 * system. But some requirements cannot be quietly dropped -- "this program
 * must never allocate", "this device must be mine alone" -- and a loader
 * that ignores one of those has agreed to a contract it does not
 * understand.
 *
 * So the top bit of the tag says which kind it is. An unknown MANDATORY
 * tag means the module is refused. The bit belongs to the tag rather than
 * to a flags field so that the two versions of a field are different tags:
 * a producer decides per value whether being understood matters.
 */
#define RV9_MTAG_MANDATORY  0x8000u
#define RV9_MTAG_NUMBER(t)  ((uint16_t)((t) & 0x7FFFu))

typedef struct __attribute__((packed)) {
    uint16_t tag;
    uint16_t len;
} rv9_mod_tlv_t;

/*
 * The registry.
 *
 * These numbers are the agreement between the compiler and RV-9, and they
 * are fixed once published. Most have no consumer here yet, and that is
 * the point: a program may describe itself completely to a system that
 * only acts on part of it, and the rest becomes enforcement later without
 * anything being rebuilt.
 */
#define RV9_MTAG_END          0x0000  /* ends the list                     */
#define RV9_MTAG_DESC         0x0001  /* string: what this program is      */
#define RV9_MTAG_STACK        0x0002  /* u32: bytes of stack required      */
#define RV9_MTAG_STATIC       0x0003  /* u32: bytes of per-instance data   */
#define RV9_MTAG_HEAP_MAX     0x0004  /* u32: ceiling; 0 means none at all */
#define RV9_MTAG_CLASS        0x0005  /* u8:  rv9_mod_class_t              */
#define RV9_MTAG_PERIOD_US    0x0006  /* u32: release period               */
#define RV9_MTAG_DEADLINE_US  0x0007  /* u32: from release                 */
#define RV9_MTAG_MIN_INTER_US 0x0008  /* u32: minimum inter-arrival        */
#define RV9_MTAG_WCET_US      0x0009  /* u32: worst-case execution         */
#define RV9_MTAG_DEVICE       0x000A  /* string: needed, shared; repeats   */
#define RV9_MTAG_EXCLUSIVE    0x000B  /* string: needed alone; repeats     */
#define RV9_MTAG_FAILSAFE     0x000C  /* u32 value + path; repeats. See below */
#define RV9_MTAG_CAPABILITY   0x000D  /* string: privilege wanted; repeats */
#define RV9_MTAG_COMPILER     0x000E  /* string: what built it             */
#define RV9_MTAG_RUNTIME      0x000F  /* string: language runtime version  */

/* The highest tag this build understands. Anything above it is unknown,
   and unknown plus mandatory is a refusal. */
#define RV9_MTAG_MAX          0x000F

/*
 * What a device must be left at when its owner stops.
 *
 * One entry per actuator, repeated -- a program parking a robot sets a
 * throttle and a brake and an enable line, and those are three constants
 * on three devices, not one string describing an intention.
 *
 * The value is deliberately a constant and the device deliberately a path:
 *
 *     uint16_t tag = RV9_MTAG_FAILSAFE
 *     uint16_t len = 4 + strlen(path)
 *     uint32_t value
 *     char     path[len - 4]        not NUL-terminated
 *
 * Nothing here can allocate, wait, call anything, or read the dead
 * program's memory, because there is nothing here but a number and a
 * name. That is the point: a failsafe is applied *after* its program has
 * stopped, frequently because that program ran off its own stack, and
 * anything richer would be asking the corpse for help.
 *
 * The path must be one the same manifest claimed with RV9_MTAG_EXCLUSIVE.
 * A program may only promise to park what it owns; anything else is a
 * promise about somebody else's device, and admission refuses it.
 */
#define RV9_FAILSAFE_MIN_LEN  5       /* a value and at least one character */

/*
 * A failsafe survives only on a device that holds its state when the last
 * path closes. /gpio does, deliberately. /pwm0 deliberately does not -- it
 * stops driving, because an actuator still running because a program
 * exited is a bad surprise. On such a device the driver's own release
 * behaviour is the safe state and a declared value is redundant at best;
 * admission says so rather than pretending otherwise.
 */

/*
 * Execution class, as the language means it.
 *
 * Distinct from rv9_proc_class_t, which is the two scheduling arrangements
 * RV-9 actually has. Proaction and reaction are both scheduled as normal
 * processes today; saying so is the module's business, deciding what to do
 * about it is RV-9's, and conflating them would lose the distinction the
 * moment RV-9 learns to treat them differently.
 */
typedef enum {
    RV9_MCLASS_UNSPECIFIED = 0,
    RV9_MCLASS_PROACTION   = 1,   /* goals and planning; loose timing     */
    RV9_MCLASS_REACTION    = 2,   /* events and state; bounded preferred  */
    RV9_MCLASS_REALTIME    = 3,   /* control and sampling; hard deadlines */
} rv9_mod_class_t;

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
 *
 * The same six numbers serve an event-driven process, with period_us
 * reading as the declared minimum inter-arrival, max_jitter_us as the time
 * from the interrupt to the process running, and overruns as events folded
 * together because they arrived while it was busy.
 *
 * This struct did NOT grow when event-driven processes arrived, and that is
 * a rule rather than an oversight. Appending to rv9_mod_env_t is safe
 * because the kernel writes it and the module only reads what it knows
 * about. This one goes the other way: the module supplies the buffer and
 * the kernel fills it, so an older module passing a shorter struct to a
 * newer kernel would have its stack written past the end. Fields the
 * kernel writes into module memory are frozen once published.
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

    /* --- ABI 11 --- */
    /* Microseconds. Milliseconds are too coarse for control code to
       measure itself with, and a loop that cannot measure itself cannot
       report that it has stopped being trustworthy. */
    uint64_t  (*time_us)(void);

    /* --- ABI 12: released by an event rather than by a period --- */
    /*
     * Some control code is periodic and some is reactive, and the second
     * kind is not the first kind sampling fast enough. A process declared
     * this way runs when something happens:
     *
     *     uint32_t both = 3;
     *     env->setstat(pin, RV9_PIO_SS_EDGE, &both);
     *     uint32_t ev = 0;
     *     env->getstat(pin, RV9_PIO_GS_EVENT, &ev);
     *     env->rt_declare_event((int)ev, 200);   // no faster than 5 kHz
     *     for (;;) {
     *         int coalesced = env->rt_wait();    // returns when it happens
     *         ...
     *     }
     *
     * rt_wait is the same call as for a periodic process, and reports the
     * same things: max_jitter_us becomes the time from the interrupt to
     * this code running, and overruns becomes the number of events that
     * arrived while it was still busy and were folded into one.
     *
     * min_interval_us is what the caller promises to cope with -- the
     * sporadic task's period, and what makes the load analysable. Pass 0
     * to say there is no bound, which is honest and promises nothing.
     *
     * Event ids come from the device, through getstat. They are small
     * integers rather than handles for the same reason OS-9's F$Event used
     * numbers: a driver can hand one out without either end needing a
     * pointer into the other.
     */
    int       (*rt_declare_event)(int event_id, uint32_t min_interval_us);
} rv9_mod_env_t;

/*
 * Wait until it exits, however long that is.
 *
 * A shell that gives up on its child after some number of seconds and
 * prints a prompt anyway has not stopped the child -- it has arranged for
 * two processes to read the same terminal. Anything interactive is a
 * program that legitimately runs for hours.
 */
#define RV9_WAIT_FOREVER ((uint32_t)0xFFFFFFFFu)

/* Seek whence, matching the I/O manager. */
#define RV9_SEEK_SET 0
#define RV9_SEEK_CUR 1
#define RV9_SEEK_END 2

/*
 * What a negative return from an I/O call means.
 *
 * These are the I/O manager's own error numbers, negated. They have always
 * travelled to modules this way; what was missing was any way for a module
 * to say which one it got, so every failure had to be reported as "it did
 * not work". A module that can tell "somebody else already has this" from
 * "this is broken" can say something useful instead.
 *
 * Values are frozen. New ones are appended, and a module that does not
 * know a number should treat it as a plain failure.
 *
 * IOE rather than ERR because the KAL already has an RV9_ERR_ set of its
 * own, with different numbers behind several of the same names. Two error
 * spaces is one more than ideal; two error spaces sharing names would be
 * a trap.
 */
#define RV9_IOE_NOTFOUND     1   /* no such device or file */
#define RV9_IOE_BADPATH      2
#define RV9_IOE_NOPATHS      3   /* this process has no free path slots */
#define RV9_IOE_NOMEM        4
#define RV9_IOE_MODE         5   /* not open for that, or not in a state for it */
#define RV9_IOE_UNSUPPORTED  6
#define RV9_IOE_WOULDBLOCK   7
#define RV9_IOE_IO           8
#define RV9_IOE_INVAL        9
#define RV9_IOE_EXISTS      10   /* already there, or already in use */
#define RV9_IOE_TIMEOUT     11
#define RV9_IOE_BUSY        12   /* somebody else owns it */

/*
 * And the same for fork, negated. Distinguishing these matters more than
 * it looks: a shell that reports every failure as "no such module" sends
 * you looking for a missing file when the machine has simply run out of
 * memory, which is a different problem with a different fix.
 */
#define RV9_PE_NOTFOUND      1   /* no module of that name */
#define RV9_PE_NOMEM         2   /* no room for its stack or statics */
#define RV9_PE_MODULE        3   /* found, but not loadable */
#define RV9_PE_TIMEOUT       4
#define RV9_PE_INVAL         5
#define RV9_PE_FAULT         6   /* the scheduler stopped it */

/* Admission refusals. A real-time program is asked for rather than
   started, and these are the ways the machine says no.

   The last two are not about timing and so apply to every program, not
   only real-time ones: a device that does not exist and a device somebody
   else owns are both answerable before the program starts, and neither
   becomes more answerable by letting it start first. */
#define RV9_PE_NOSLOT        7   /* every real-time slot is taken */
#define RV9_PE_CONTRACT      8   /* the declaration contradicts itself */
#define RV9_PE_UTILISATION   9   /* the CPU is already promised */
#define RV9_PE_NODEV        10   /* it needs a device this machine lacks */
#define RV9_PE_BUSY         11   /* it needs a device alone; somebody has it */

/* Generic getstat/setstat codes a module may use. */
#define RV9_SS_ECHO        1
#define RV9_SS_AUTOLF      2
#define RV9_GS_READY       3
#define RV9_GS_SIZE        4
#define RV9_SS_RAW         5   /* 0 = lines, 1 = keystrokes */
#define RV9_SS_DRIVER_BASE 256

/*
 * Raw input.
 *
 * Normally SCF reads a *line*: it buffers until return, echoes, handles
 * rubout, and throws away anything that is not printable. That is right
 * for a shell and wrong for anything that draws, because an arrow key is
 * ESC [ A and the first byte of it would be discarded before any program
 * saw it.
 *
 * In raw mode a read returns whatever has arrived, as soon as it arrives,
 * unechoed and unfiltered. Ask for several bytes: an escape sequence is
 * more than one, and getting it in a single read saves guessing whether
 * more is coming.
 *
 * It belongs to the open path rather than the device, so a program that
 * sets it cannot leave somebody else's shell in a strange state -- but it
 * is inherited with the path across fork, which is what lets a program set
 * it on stdin and have it mean something. Put it back before exiting.
 */

/*
 * Peripheral settings, shared by every PIO device so that a program does
 * not need to know which driver is underneath.
 */
#define RV9_PIO_SS_DIRECTION  16   /* 0 = input, 1 = output */
#define RV9_PIO_SS_PULL       17   /* 0 = none, 1 = up, 2 = down */
#define RV9_PIO_SS_FREQUENCY  18   /* Hz, for anything periodic */
#define RV9_PIO_GS_RANGE      19   /* largest value a write may carry */

/*
 * Interrupts, as a device setting.
 *
 * Arming a unit with SS_EDGE makes it signal an event when the world
 * changes; GS_EVENT then says which event, as a small integer, and a
 * real-time process asks to be released by that number. Nothing about this
 * is specific to a pin -- a UART with a character waiting, or a card
 * finishing a transfer, answers the same two codes -- which is the point of
 * putting it in the generic PIO settings rather than in the gpio driver.
 */
#define RV9_PIO_SS_EDGE       20   /* 0 = off, 1 = rising, 2 = falling,
                                      3 = both */
#define RV9_PIO_GS_EVENT      21   /* event id, or 0 if not armed */

/* ------------------------------------------------------------------ */
/* Publication cells                                                   */
/*                                                                     */
/* One process computes a value; another has to see it. RV-9 had no    */
/* answer to that at all -- paths and signals, and neither carries an   */
/* observation -- which is the gap under R9's reactive layer: `watch`,  */
/* `state` and `transition` all read values a real-time component in    */
/* another process produced.                                           */
/*                                                                     */
/* The answer is a device. A publication is a named, fixed-size cell    */
/* on a device that serves them, and publishing is one write of one     */
/* struct to one path -- atomic because it is one call and one copy.    */
/* Naming, ownership, lifetime and the RT-safe transfer path all come   */
/* from the I/O system rather than being invented alongside it.         */
/*                                                                     */
/*     /pub0/MOTOR_CONTROL      the cell                               */
/*                                                                     */
/* See components/rv9_io/src/pfm.c for how it is done, and             */
/* docs/design.md for why it is a device rather than shared memory.     */
/* ------------------------------------------------------------------ */

#define RV9_PUB_MAX_NAME 24

/*
 * The head of every publication, read and written.
 *
 * A read fills this and as much of the value as the caller's buffer will
 * hold; a write supplies it and the value together. The same object goes
 * both ways deliberately -- what comes out of one cell can be written into
 * another unchanged, which is what a bridge or a recorder needs.
 *
 * seq is R9 §18's validity indication and its publication sequence in one
 * number: zero means never published, and it counts up by one per
 * publication thereafter. A watcher remembers the last it saw.
 *
 * stamp_us is when the *observation* was made, not when it was published.
 * Those differ by however long the computing took, and a reactive layer
 * deciding how stale a reading is needs the first. A publisher that passes
 * zero is saying "now", and gets the time of the write.
 *
 * On a write, seq is ignored: the cell owns it. len is the bytes of value
 * following this struct.
 */
typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint32_t len;
    uint64_t stamp_us;
} rv9_pub_t;

_Static_assert(sizeof(rv9_pub_t) == 16, "publication head must be 16 bytes");

/*
 * Wait until a cell changes. getstat, arg is an rv9_pub_wait_t.
 *
 * R9 §21 asks that a watcher re-evaluate when a value changes rather than
 * polling, so this blocks: pass the sequence last seen, and it returns
 * when the cell has moved past it, with the current sequence in its place.
 * A publication that arrives while nobody is waiting is not lost -- the
 * comparison is against the sequence, not against an edge -- and several
 * that arrive together coalesce into one wakeup, which is what §21 wants.
 *
 * timeout_ms bounds the wait. RV9_WAIT_FOREVER blocks indefinitely; zero
 * makes it a poll that never blocks, which is what a real-time observer
 * should use.
 */
#define RV9_PUB_GS_WAIT  48
#define RV9_PUB_GS_INFO  49   /* rv9_pub_info_t: what this cell is */

typedef struct __attribute__((packed)) {
    uint32_t seq;          /* in: last seen. out: current */
    uint32_t timeout_ms;
} rv9_pub_wait_t;

typedef struct __attribute__((packed)) {
    char     name[RV9_PUB_MAX_NAME];
    uint32_t seq;
    uint32_t len;          /* bytes in the last publication */
    uint32_t cap;          /* bytes this cell can hold */
    uint64_t stamp_us;

    /*
     * `held` and `writer` are two questions, not one. A cell opened for
     * writing by the system rather than by a process has no pid, and
     * reporting that as writer == 0 would make "nobody is publishing this"
     * and "RV-9 itself is" the same answer.
     */
    uint16_t writer;       /* the publisher's pid; 0 when it is the system */
    uint16_t readers;
    uint8_t  held;         /* somebody has it open for writing */
    uint8_t  reserved[3];
    uint32_t torn;         /* reads abandoned mid-publication since boot */
} rv9_pub_info_t;

/* ------------------------------------------------------------------ */
/* Console settings                                                    */
/*                                                                     */
/* Addressing the cursor, colour and attributes -- for every character */
/* device that has a screen at the far end, whatever kind of screen it */
/* is.                                                                 */
/*                                                                     */
/* These are setstat codes and NOT escape sequences in the byte stream, */
/* which is the whole point. /term is not a terminal: it is a panel     */
/* with a font renderer, and teaching it to parse ANSI would be absurd  */
/* when it has no need of one. So a program says "cursor to 10,20" once */
/* and the device decides what that means -- ESC[11;21H down a wire, a  */
/* change of render position on the glass. Programs that address the    */
/* screen work on both without knowing which they have.                 */
/*                                                                     */
/* SCF supplies the escape sequences for any driver with no opinion of  */
/* its own, so a new character driver gets all of this for free.        */
/* ------------------------------------------------------------------ */

#define RV9_CON_GS_SIZE     32   /* rows << 16 | cols. getstat only */
#define RV9_CON_SS_CURSOR   33   /* row << 16 | col, both 0-based */
#define RV9_CON_SS_COLOUR   34   /* fg | bg << 8, see RV9_COL_* */
#define RV9_CON_SS_ATTR     35   /* RV9_CON_ATTR_*, the whole set each time */
#define RV9_CON_SS_CLEAR    36   /* RV9_CON_CLEAR_* */
#define RV9_CON_SS_CURSOR_ON 37  /* 0 = hide it, 1 = show it */

/*
 * Is this device what the screen is currently showing? getstat only.
 *
 * Only meaningful where devices share one display. A terminal at the end
 * of a wire always says yes, because nothing else can be using it.
 *
 * It exists so that an *incidental* write can decline to steal the screen.
 * The panel shows whoever painted last, which is the only workable rule
 * with one framebuffer-less display -- but it makes no distinction between
 * a program that meant to draw and a shell echoing the command you just
 * typed, and the second should not destroy the first.
 */
#define RV9_CON_GS_ONSCREEN  38

/*
 * Sixteen colours, in the order every terminal has used since the VT100,
 * because that is the order the escape codes are in and a program that
 * wants blue should not have to know which end it is talking to.
 *
 * Deliberately not RGB: a text console app thinks in named colours, and a
 * palette is something a device can honour. Truecolour, if it is ever
 * wanted, belongs in a driver-specific code where the honesty is local.
 */
#define RV9_COL_BLACK    0
#define RV9_COL_RED      1
#define RV9_COL_GREEN    2
#define RV9_COL_YELLOW   3
#define RV9_COL_BLUE     4
#define RV9_COL_MAGENTA  5
#define RV9_COL_CYAN     6
#define RV9_COL_WHITE    7
#define RV9_COL_BRIGHT   8    /* or it into any of the above */
#define RV9_COL_DEFAULT  0xFF /* whatever this device came up as */

#define RV9_CON_ATTR_BOLD      (1u << 0)
#define RV9_CON_ATTR_UNDERLINE (1u << 1)
#define RV9_CON_ATTR_REVERSE   (1u << 2)

#define RV9_CON_CLEAR_SCREEN   0   /* all of it, and home the cursor */
#define RV9_CON_CLEAR_EOL      1   /* cursor to end of line */
#define RV9_CON_CLEAR_EOS      2   /* cursor to end of screen */

/* The LCD console's own settings. */
/* RV9_LCD_SS_CLEAR is superseded by RV9_CON_SS_CLEAR, which every console
   answers. Kept because clearing the screen is not worth breaking. */
#define RV9_LCD_SS_CLEAR      (RV9_SS_DRIVER_BASE + 0)
#define RV9_LCD_SS_BRIGHTNESS (RV9_SS_DRIVER_BASE + 1)   /* 0..100 percent */

/* ------------------------------------------------------------------ */
/* sysinfo                                                             */
/* ------------------------------------------------------------------ */

#define RV9_SYS_MEM      1
#define RV9_SYS_MODULES  2
#define RV9_SYS_PROCS    3
#define RV9_SYS_RT       4     /* rv9_sys_rt_t, one per real-time task */
#define RV9_SYS_STACK    5     /* rv9_sys_stack_t, one per live process */
#define RV9_SYS_ADMIT    6     /* rv9_sys_admit_t, one record            */
#define RV9_SYS_CLAIM    7     /* rv9_sys_claim_t, one per device claim  */

/*
 * Who owns what.
 *
 * One record per resource per owner, which is why the same device may
 * appear more than once: five processes sharing /term are five records,
 * and that is the answer to "who has it open" rather than a count.
 *
 * `reserved` marks a claim taken at fork from the module's manifest rather
 * than by an open. Such a claim exists before the program runs and outlives
 * every path it opens, which is the whole point: a declared exclusive
 * device is the program's for its lifetime, not for the duration of one
 * open.
 */
typedef struct __attribute__((packed)) {
    char     name[48];         /* the resource, as opened: "/gpio/2"     */
    uint16_t owner;            /* pid, or 0 for a driver's own hold      */
    uint8_t  exclusive;
    uint8_t  reserved_at_fork;
    uint32_t refs;             /* opens outstanding, plus the reservation */

    /*
     * What this device is to be left at when its owner stops, from the
     * owner's manifest.
     *
     * Appended -- but note that an *array* record cannot grow the way
     * rv9_sys_mem_t can. The caller's buffer length is divided by the
     * record size to get a stride, so a module built against the shorter
     * version reads misaligned rather than short. Every module in the
     * store is rebuilt together, which is why this is survivable and not
     * why it is right; see docs/design.md §24.
     */
    uint8_t  has_failsafe;
    uint8_t  fs_reserved[3];
    uint32_t failsafe_value;
} rv9_sys_claim_t;

/*
 * What real-time work the machine has promised, and how sure it is.
 *
 * Three counts rather than one total. Only `declared` is a promise kept
 * from what a module said about itself; `measured` is a floor taken from
 * what a loop has actually cost so far, which is evidence and not a
 * bound; `unaccounted` is work nobody can say anything about. A single
 * "18% used" would hide which of those the other 82% is standing on.
 *
 * Fields may be appended, never reordered -- filled up to the caller's
 * length, like rv9_sys_mem_t.
 */
typedef struct __attribute__((packed)) {
    uint32_t used_permille;
    uint32_t ceiling_permille;
    uint32_t declared;
    uint32_t measured;
    uint32_t unaccounted;
    uint32_t slots_used;
    uint32_t slots_total;
} rv9_sys_admit_t;

/*
 * Fields may be appended to this record but never reordered or removed.
 * A module built against a shorter version passes the length it knows
 * about and is filled up to there -- see env_sysinfo. The first five
 * fields are what shipped before the floor existed and are the minimum
 * any caller must ask for.
 */
typedef struct __attribute__((packed)) {
    uint32_t heap_free;
    uint32_t heap_low_water;
    uint32_t heap_exec_free;
    uint32_t module_count;
    uint32_t proc_count;

    uint32_t heap_floor;       /* reserved; RV-9 will not allocate into it */
    uint32_t heap_available;   /* free above the floor: what may be spent */
    uint32_t heap_refusals;    /* allocations turned away since boot */
} rv9_sys_mem_t;

/* What a caller must ask for at minimum, and the boundary that must not
   move: everything before it shipped in the original record. */
#define RV9_SYS_MEM_MIN offsetof(rv9_sys_mem_t, heap_floor)

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

/*
 * What a real-time task is actually doing, for anything watching.
 *
 * rt_stats reports the caller's own timing, which is right for a loop
 * checking itself and useless for a display. These records are every
 * real-time task at once, so a panel can show how late the control loop
 * is running without being the control loop.
 *
 * Which is the metric an autonomous machine wants on its screen: not that
 * it is working, but by how much it is missing.
 */
typedef struct __attribute__((packed)) {
    uint16_t index;            /* the task's slot, stable while it lives */
    uint8_t  event_driven;     /* released by an interrupt, not a period */
    uint8_t  reserved;
    uint32_t period_us;        /* or the declared minimum gap, if event-driven */
    uint32_t activations;
    uint32_t overruns;
    uint32_t max_jitter_us;
    uint32_t max_exec_us;
    uint32_t last_exec_us;
    uint32_t min_interval_us;  /* shortest gap actually seen */
} rv9_sys_rt_t;

/*
 * What a process's stack cost and what it actually used.
 *
 * A separate record rather than fields on rv9_sys_proc_t, which the kernel
 * fills into a buffer the module supplies and therefore cannot grow --
 * the same rule that froze rv9_rt_report_t.
 *
 * `unused` is measured, not estimated: thread stacks are painted at
 * creation and scanned from the low end. Zero unused does not mean it fits
 * exactly; it means the thread has touched every byte it was given and has
 * very likely gone past them.
 */
typedef struct __attribute__((packed)) {
    uint16_t pid;
    uint16_t reserved;
    char     name[32];
    uint32_t stack_size;
    uint32_t stack_unused;
} rv9_sys_stack_t;

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
 * Open it alone, or not at all.
 *
 * Two programs driving one actuator is not a race to be won, it is two
 * answers to a question with one physical outcome. So a program that must
 * be the only one on a device says so, and the open is refused rather than
 * shared -- RV9_IO_ERR_BUSY, before any hardware is touched.
 *
 * A program that knows this at compile time should declare it in its
 * manifest instead (RV9_MTAG_EXCLUSIVE), which claims the device at fork:
 * refused before the program starts rather than partway through it. This
 * bit is for the case the compiler could not know, where the device is
 * chosen at runtime.
 */
#define RV9_MODE_EXCL   (1u << 3)

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
    RV9_MOD_ERR_CONTRACT,   /* the manifest demands something unknown here */
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
    int (*claims)(void *buf, uint32_t len);  /* fills rv9_sys_claim_t records */
} rv9_mod_io_ops_t;

void rv9_mod_set_io_ops(const rv9_mod_io_ops_t *ops);

/* Same arrangement for the process manager, for the same reason. */
typedef struct {
    int (*fork)(const char *module, int priority);
    int (*wait)(int pid, int *status, uint32_t timeout_ms);
    int (*procs)(void *buf, uint32_t len);   /* fills rv9_sys_proc_t records */
    int (*stacks)(void *buf, uint32_t len);  /* fills rv9_sys_stack_t records */
    int (*chain)(const char *module);
    int (*fork_arg)(const char *module, int priority, const char *arg);
    int (*fork_rt)(const char *module, uint32_t period_us, const char *arg);
    int (*rt_load)(void *buf, uint32_t len); /* fills one rv9_sys_admit_t */
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
 * Read one value out of a module's manifest.
 *
 * Returns a pointer into the image -- no copy, no allocation, and valid
 * for as long as the image is. NULL when the tag is absent, which is the
 * common answer and not an error: almost nothing declares almost anything
 * yet.
 *
 * `tag` is given without the mandatory bit; a value carrying it is found
 * either way, because whether the producer insisted on being understood
 * does not change what the value means.
 *
 * Repeatable tags -- devices, capabilities -- are walked by passing the
 * previous value back as `after`, or NULL to start.
 */
const void *rv9_mod_manifest_find(const void *image, uint16_t tag,
                                  const void *after, uint16_t *out_len);

/* The same, for the tags whose value is a little-endian number. Returns
   false when the tag is absent or the wrong size, leaving *out alone. */
bool rv9_mod_manifest_u32(const void *image, uint16_t tag, uint32_t *out);
bool rv9_mod_manifest_u8(const void *image, uint16_t tag, uint8_t *out);

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
