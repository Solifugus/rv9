# RV-9 — Reference

**The contract: what RV-9 promises a program, what it requires of one, and
what it enforces.**

> **Status: being written.** Sections marked *To write* are outlines.
>
> Where this document lists something the firmware defines — a system call, a
> manifest tag, a fault code — the list is meant to be **complete**, and
> `tools/hosttest/run.sh` is meant to fail if it stops being. See §16.

Three documents, three jobs. This one is for looking things up. The
[tutorial](tutorial.md) is for learning by doing. The
[design log](design.md) is for why, and records the wrong turns.

The machine-readable version of most of what follows is
[`docs/target/rv9-profile.json`](target/rv9-profile.json), generated from the
sources. A compiler targeting RV-9 should read that rather than this; this
document exists to explain what the fields mean.

**Module ABI 14.** Anything below marked *since ABI n* is absent from an
older firmware.

---

## 1. The seven nouns

Everything in RV-9 is one of these, and the separations between them are the
architecture rather than an implementation detail.

| | what it is |
|---|---|
| **module** | A position-independent lump of code with a header, a CRC and a manifest, found by name in a store. Programs, drivers, file managers and device descriptors are all modules. |
| **process** | A running module: a stack, its own statics, a path table, a memory budget, a priority, and a parent. |
| **path** | An open thing, named as a string. `open("/gpio/2")`. The number you get back is an index into your own path table, and children inherit it. |
| **file manager** | The *discipline* a path obeys — what read and write mean. SCF makes characters, RBF makes blocks, PIO makes single values, IFM makes transactions. |
| **driver** | The hardware underneath, with no opinion about discipline. |
| **device descriptor** | A module that says *this file manager, that driver, these options, called this name* — which is how `/i2c0` exists without anybody compiling it in. |
| **class** | What kind of citizen a process is: `REALTIME` is admitted or refused, and gets reserves nothing else may touch. |

The consequence worth stating once: because a file manager, a driver and a
descriptor are three separate loadable modules, adding a device is adding a
descriptor, and adding a *kind* of device is adding a file manager. Neither
needs the firmware rebuilt.

## 2. Paths

A path is an open thing. The name is a string, the same shape everywhere:

```
/device                 the device itself
/device/rest            something below it
```

The split is at the **first slash after the leading one**, and RV-9 does no
more parsing than that. `/r0/notes` is the device `/r0` and the remainder
`notes`; `/n0/example.com/80` is the device `/n0` and the remainder
`example.com/80`. Everything after that first slash is the file manager's
business and RV-9 has no opinion about it — which is why a file manager can
be added without teaching the I/O manager a new grammar.

A device name is at most 15 characters; anything longer is cut, and the
device is then simply not found. The full name is used as the key of an
ownership record, which holds 47 characters and **truncates rather than
refuses** — so two names agreeing in their first 47 characters would be
treated as one resource. Nothing shipped can reach that: `rbf` file names stop
at 27, pipe names at 15. A long enough host name through `nfm` could, which is
the one place the limit is worth remembering.

### What each manager makes of the remainder

| manager | remainder | empty remainder means |
|---|---|---|
| `scf` | ignored | the device; a character stream has no names below it |
| `rbf` | a file name | **the directory** — read it for `rv9_dirent_t` records |
| `pio` | a unit number, decimal | refused: a pin has to be said |
| `ifm` | an address, decimal **or** `0x`-hex | refused |
| `nfm` | `host/port`, split at the *last* slash; `listen/PORT` to accept | the device, for `getstat` |
| `pfm` | a cell name | the directory of cells; **read-only** |
| `pipe` | a pipe name | refused: an unnamed pipe has no other end to find |

`ifm` taking both `0x68` and `104` is deliberate. A datasheet says the first
and a shell script usually says the second, and refusing either would be a
small cruelty repeated every time somebody types an address.

`rbf` and `pfm` answering the empty remainder with a directory is what makes
`dir /r0` and `pubs` ordinary programs rather than built-ins: they open a
path and read records.

### The path table

Twelve slots per process (§14), private to it, holding descriptors that may
be shared. Slots 0, 1 and 2 are standard input, output and error **by
convention rather than by rule** — nothing in the kernel treats them
specially except the three facts below.

**`open` returns the lowest free slot.** Not an arbitrary one, and not a
stable one: it depends on what is already open. Code that opens something and
*then* parks a path into a fixed slot can park on top of what it just opened.
The shell lost the read end of a pipe exactly this way, with no symptom but a
command that quietly produced nothing; it now parks before it opens anything.

**`fork` passes down slots 0, 1 and 2, and only those.** The child shares the
parent's descriptors rather than reopening them, which is what makes
redirection work: the shell opens the destination, aims its own stdout at it
with `dup2`, forks, and the child writes there without knowing. Everything the
parent had open above slot 2 the child does not have. A program that wants a
child to inherit a path must put it in one of the three.

**`chain` keeps everything.** The process survives — same pid, same table,
same open paths — and only the code is replaced.

`dup2(from, to)` makes `to` a second name for one descriptor, closing
whatever `to` held. Both names share the position, the mode and the device;
closing one leaves the other working.

### Ownership

Every open takes an **ownership record**, keyed on the full path as written,
and it is taken before the file manager or the driver hears about the open at
all. `/gpio/2` and `/gpio/3` are two resources on one device because they are
two pins.

Sharing is the default: five processes open `/term` and there are five
records, because the question an operator asks is *who has it* and the
question a failsafe will ask is *whose device was this when it died* —
neither survives being reduced to a count. A program that must be alone says
so, either with `RV9_MODE_EXCL` at open or, better, with `exclusive` in its
manifest, which claims the device at fork: refused before the program starts
rather than partway through its first control period.

Refusing early is the whole point of doing it here. By the time a driver has
configured a PWM channel it has already begun driving the pin that the second
opener is about to be told it cannot have.

Two devices behave differently on last close, and §4 says which and why.

## 3. The disciplines

A file manager decides what `read` and `write` *mean*. Seven of them, and the
first four are the ones that matter architecturally — they are four different
answers to "what is the unit of transfer", and a device belongs to exactly
one.

| manager | unit | `seek` | read RT-safe | write RT-safe |
|---|---|---|---|---|
| `scf` | characters | no | no | no |
| `rbf` | blocks, in files | yes | no | no |
| `pio` | one 32-bit value | no | **yes** | **yes** |
| `ifm` | a byte transaction | no | no | no |
| `nfm` | a stream, from a connection | no | no | no |
| `pfm` | a published cell, whole | no | **yes** | **yes** |
| `pipe` | characters, between processes | no | no | no |

Only `pio` and `pfm` are real-time safe in both directions, and that is the
whole reason a control loop reads a pin and publishes a cell rather than
writing to a file: those two paths are bounded and resident, so they still
work while the flash cache is off.

### `scf` — the sequential character file manager

Characters, in order, with no position. Read blocks until there is something.
The console (`/term`), the serial port (`/uart0`) and SSH sessions (`/ssh0`)
are all SCF, which is why the shell needed no changes to work over SSH.

Line editing and echo live *here*, below the shell — which is why
`modules/shell/shell.c` contains no terminal handling at all. Over a terminal
SCF hands back a whole line; over a socket it hands back whatever arrived,
and a line may take several reads or share one with the next.

Console settings — cursor, colour, attributes, clear, and asking whether the
panel is currently showing the console — are getstat/setstat codes; see §9.

### `rbf` — the random block file manager

Files on a volume, in 512-byte sectors, with a position you may `seek`.
`/r0` is a RAM disk, `/f0` a flash partition, `/sd0` a microSD card. `dir`,
`df` and `format` work on all three because they talk to RBF rather than to
any of them.

Reading past the end returns zero bytes rather than an error — end of data is
not a failure. Opening with `RV9_MODE_CREATE` **truncates** an existing file,
which is why `>` and `>>` in the shell are different operations (§ tutorial 7).

There is **no sector cache**: every read fetches the whole sector containing
the bytes you asked for. This matters when you are tempted to read a file one
byte at a time.

### `pio` — the positional I/O manager

One 32-bit value per read or write, and a unit number in the path: `/gpio/2`,
`/pwm0/3`, `/adc0/0`. Not text — a program converts for humans, a control
loop does not.

This is the discipline for anything whose state *is* a number: a pin, a duty
cycle, a conversion, a die temperature. Its generic settings (direction, pull,
frequency, edge arming, pulse timing) are the same codes whatever the driver
underneath, which is the point: a program that arms an edge does not need to
know whether it is talking to a pin or a UART.

### `ifm` — the interface file manager

A byte transaction with a device on a shared bus, addressed by the path:
`/i2c0/0x68`, or `/i2c0/104` in decimal.

PIO cannot do this, and the reason is worth keeping. Reading six bytes of
acceleration has to be **one** transaction — three axes sampled at one
instant, fetched without letting go of the bus — or the numbers come from
three different moments and the vector they form never existed.

The register lives on the **path**, set with `RV9_IFM_SS_REG`, so a read
becomes the write-then-read with a repeated start that datasheets describe
and several devices require. On the path rather than on the device, so two
programs reading different registers of the same chip cannot corrupt each
other. Set it to `RV9_IFM_REG_NONE` and a read is a plain read.

### `nfm` — the network file manager

The path *is* the connection. `/n0/host/port` opens an outbound TCP
connection; `/n0/listen/port` accepts an inbound one. After that it is a
stream, so `cat` works on a socket.

### `pfm` — the publication file manager

A cell at `/pub0/NAME` with exactly one writer, declared in that writer's
manifest and reserved at fork. A write publishes the whole object
indivisibly: the fields become visible together or not at all, which is what
makes a set of three numbers a *reading* rather than three numbers. Each
carries a sequence number and a `stamp_us`. See §10.

### `pipe` — the pipe file manager

`/pipe/name`, what one process writes and another reads. This is why `|` in
the shell costs two opens and a fork and no special machinery: a pipe is a
device, so every program that reads standard input and writes standard output
composes through one without being changed.

A reader that opens before any writer does is not told end-of-data — it
waits. A reader learns the run is over when the last writer closes, which is
why the shell must let go of a write end it is holding.

## 4. Devices as shipped

Sixteen devices over fifteen drivers (`ssh` serves two). A device exists
because a **descriptor module** says *this file manager, that driver, these
options, called this name* — so adding one is adding a module, not rebuilding
the firmware.

| path | manager | driver | notes |
|---|---|---|---|
| `/term` | `scf` | `lcdcon` | The ST7789 panel as a console. Brightness and cursor via setstat. |
| `/uart0` | `scf` | `uart` | USB serial: the boot log and the console shell. |
| `/ssh0` | `scf` | `ssh` | SSH sessions as a character device. |
| `/sshcfg` | `scf` | `ssh` | The server's own settings — `passwd`, `authkey`. |
| `/w0` | `scf` | `svgwin` | A window you draw on by writing SVG to it. Documents up to 4 KB. |
| `/r0` | `rbf` | `ramdisk` | RAM disk. Fast, and gone at reboot. |
| `/f0` | `rbf` | `flashdisk` | A flash partition. Where modules live to survive a reboot. |
| `/sd0` | `rbf` | `sdspi` | microSD over SPI, sharing the bus with the display. |
| `/gpio/N` | `pio` | `gpio` | Pins. Refuses the ones the board has spoken for: USB console, display, card. |
| `/pwm0/N` | `pio` | `pwm` | Duty cycle. Stops driving when the path closes — it holds a hardware channel that must be given back. |
| `/adc0/N` | `pio` | `adc` | Analogue in. |
| `/tsens` | `pio` | `tsens` | The die's own temperature. |
| `/i2c0/ADDR` | `ifm` | `i2c` | The two-wire bus. Options: SDA (8), SCL (9), kHz (100). |
| `/n0/…` | `nfm` | `net` | TCP, out and in. |
| `/pub0/NAME` | `pfm` | `pubmem` | Published cells. |
| `/pipe/NAME` | `pipe` | `pipemem` | Pipes. |

Two behaviours that differ deliberately and will bite if assumed uniform:
**`/gpio` holds its level when the last path closes**, because an enable line
that dropped when a command finished would be useless — and that is what
makes a failsafe on a pin outlive the program that declared it. **`/pwm0`
stops driving**, because its safe state is *off* and an actuator still
running because a program exited is a bad surprise. Declaring a failsafe
value for a PWM channel is therefore refused at admission: it would be a
promise that expires at the moment it matters.

## 5. The module format

> **To write:** the 40-byte header, the magic, the ABI check (the loader
> refuses a module declaring a *higher* ABI than the firmware, never a
> lower), the CRC, the type field, and `.text.entry`. Then the rules a
> module must obey and the reason for each: no `.data`/`.bss`, no libc, no
> 64-bit division, no function-scope `static const` arrays, `-mcmodel=medany`
> — each of which was discovered by breaking it.

## 6. The manifest

A module's TLV manifest is what it says it needs. RV-9 reads it **before the
module runs**, which is what makes refusal possible: a program that asks for
more than the machine can promise never starts, rather than failing halfway.

Tags come from `build.conf`. **Enforced** means RV-9 acts on it; advisory
means it is recorded and passed on but nothing checks it.

| tag | enforced | what it says |
|---|---|---|
| `desc` | advisory | One line, for `mdir` and for humans. |
| `stack` | **yes** | Stack bytes. Allocated at fork; a fork that cannot have it is refused. |
| `static` | advisory | Bytes of private storage, allocated and zeroed at fork. Reaching `env->statics`. |
| `heap_max` | **yes** | Ceiling on what the process may allocate. Zero means *none*, which is the declaration a control loop should make. |
| `class` | advisory | `UNSPECIFIED`, `PROACTION`, `REACTION` or `REALTIME`. |
| `period_us` | **yes** | How often a real-time component is released. |
| `deadline_us` | **yes** | How long after release it must have answered. |
| `min_inter_us` | **yes** | For an event-released component: the shortest gap between events it will tolerate. |
| `wcet_us` | **yes** | Declared worst-case execution time. This is the number admission does arithmetic with. |
| `device` | **yes** | A device the module needs, shared. |
| `exclusive` | **yes** | A device the module needs *alone*. A second owner is refused before it runs. |
| `failsafe` | **yes** | A device and a constant. RV-9 writes it when the process ends, however it ends — including when the process is the thing that failed. |
| `capability` | advisory | Something the module claims to offer. |
| `compiler` | advisory | What produced it. |
| `runtime` | advisory | What it expects to run under. |
| `on_deadline` | **yes** | `REPORT` or `FAULT`. Whether a missed deadline stops the component. |
| `publishes` | **yes** | A cell this module owns. Reserved at fork, so a second copy is refused and nothing else can write into it. |
| `watches` | **yes** | A cell this module reads. Admitted because the declaration of its writer exists on the machine, whether or not the writer is running. |
| `mem_max` | **yes** | The process's whole memory budget, itself and everything it starts. |
| `placement` | **yes** | `DERIVED`, `URGENT` or `ROUTINE`. A pin constrains the analysis rather than overriding it: if the pin would make any loop late, the program is refused. |
| `mandatory` | — | Not a tag but a bitmask over them. See below. |

### Mandatory tags, and unknown ones

A tag RV-9 does not recognise is **ignored** if advisory and **refuses the
module** if marked mandatory. That is the whole forward-compatibility story,
and it runs in the useful direction: a module built for a newer RV-9 that
needs something this one has never heard of does not start, instead of
starting and quietly not getting it.

So `mandatory="heap_max"` in a `build.conf` means *refuse me rather than run
me without this*, and a control loop should say it.

> **To write:** the TLV layout itself, the mandatory bit's position, and
> `highest_known_tag`.

## 7. The environment

Thirty-four entries, arriving as one struct of function pointers and values.
No libc, no globals, no kernel calls: **everything a module can do is here**,
and a module that needs something not in this list needs a newer ABI or a
device.

Five are plain values; the other twenty-nine are calls. Unless a call says
otherwise it returns **0 or a count on success and a negative `RV9_PE_*` or
`RV9_IO_ERR_*` on failure** — negated, so `-RV9_PE_NOMEM` is what a refused
`fork` hands back.

**Real-time safety** is the column that decides whether a call may appear
between `rt_wait`s:

| | |
|---|---|
| **yes** | Bounded, and resident in memory that survives flash operations. |
| **device** | Bounded only if the path's file manager *and* driver are both real-time safe — see §3. `pio` and `pfm` are; nothing else is. |
| **no** | Not to be called with a deadline pending. |

That classification is read from the `RV9_RT_CODE` attribute on the function
implementing each entry. What *that* function goes on to call is its
implementer's responsibility and is not checked, so it is a claim about RV-9's
own code rather than a proof about the whole path.

### Itself

| | since | |
|---|---|---|
| `uint32_t abi_version` | 1 | What this firmware implements. Check it before using anything newer than you must. |
| `void *statics` | 1 | The module's private storage, zeroed, as many bytes as the manifest's `static` asked for. **NULL when it asked for none.** |
| `uint32_t statics_size` | 1 | How much was actually given. Check it against your own `sizeof` and refuse rather than overrun — every tool here does. |
| `uint32_t pid` | 2 | **0 when the module was run outside a process**, which `rv9_mod_run` does at boot. |
| `const char *arg` | 2 | The rest of the command line. **May be NULL.** |

The `statics_size` check is not ceremony. A module built against a larger
struct than its manifest declares gets a smaller allocation, and the resulting
corruption is somebody else's heap. The convention throughout is:

```c
mything_t *st = (mything_t *)env->statics;
if (st == NULL || env->statics_size < sizeof(*st)) return 2;
```

### Time and yielding

| | since | RT | |
|---|---|---|---|
| `uint64_t time_ms(void)` | 1 | no | Milliseconds since boot. |
| `uint64_t time_us(void)` | 11 | **yes** | Microseconds since boot. |
| `void sleep_ms(uint32_t)` | 2 | no | Give up the CPU for at least this long. |
| `void yield(void)` | 2 | no | Give up the CPU now; return when scheduled again. |

**A real-time loop must use `time_us`, not `time_ms`** — the millisecond one
is not resident and is too coarse to measure a loop with anyway. Milliseconds
cannot express a 2,022 µs response.

**`sleep_ms` is not how a control loop waits.** It asks for *at least* that
long and drifts; `rt_wait` returns at the next release and reports what it
cost. A loop that sleeps is a loop with no deadline.

### Paths

| | since | RT | |
|---|---|---|---|
| `int open(const char *name, uint32_t mode)` | 3 | no | Returns a path number, or negative. |
| `int close(int path)` | 3 | no | |
| `int read(int path, void *buf, uint32_t len)` | 3 | **device** | Bytes read; 0 at end of data, which is not an error. |
| `int write(int path, const void *buf, uint32_t len)` | 3 | **device** | Bytes written. |
| `int seek(int path, int32_t offset, int whence)` | 8 | no | `RV9_SEEK_SET/CUR/END`. |
| `int dup2(int from, int to)` | 5 | no | Point `to` at what `from` names. |
| `int getstat(int path, uint32_t code, void *arg)` | 8 | no | Ask the device something. |
| `int setstat(int path, uint32_t code, void *arg)` | 8 | no | Tell the device something. |
| `int remove(const char *name)` | 7 | no | Delete a file. |

Three things to know before writing code that opens more than one thing.

**`open` returns the lowest free slot.** So parking a path into a fixed slot
*after* opening something can land on top of what was just opened. The shell
lost the read end of a pipe this way, and the only symptom was a command that
quietly produced nothing — it now parks before opening anything. Twelve slots
per process (§14), of which 0, 1 and 2 are standard input, output and error by
convention rather than by rule.

**`seek` reports whether it worked, not where it landed.** There is no *tell*.
A caller that needs the position must count what it has consumed — which is
what the shell does to make `while` loops work in scripts.

**`read` and `write` are only real-time safe on a real-time safe path.** In
practice that means `/gpio`, `/pwm0`, `/adc0`, `/tsens` and `/pub0/NAME`. A
control loop that writes to a file is not a control loop.

### Processes

| | since | RT | |
|---|---|---|---|
| `int fork(const char *module, int priority)` | 4 | no | The new pid, or negative. |
| `int fork_arg(const char *module, int priority, const char *arg)` | 7 | no | As `fork`, with a command line. |
| `int fork_rt(const char *module, uint32_t period_us, const char *arg)` | 10 | no | Fork into the real-time class; subject to admission. |
| `int wait(int pid, int *status, uint32_t timeout_ms)` | 4 | no | |
| `int wait_why(int pid, int *status, int *fault, uint32_t timeout_ms)` | 14 | no | **Use this one.** |
| `int chain(const char *module)` | 6 | no | Continue as a different module, keeping pid and open paths. |
| `int signal(int pid, uint32_t signals)` | 13 | no | A request the target notices at `signals_take`. |
| `int kill(int pid)` | 13 | no | Not a request. |
| `uint32_t signals_take(void)` | 2 | no | Pending signals; **reading clears them**. |

**`wait` hands back one number where there are two questions.** A module's
return value and RV-9's own verdicts share a number space, so `wait` alone
cannot tell "the program returned −6" from "the scheduler stopped it". Use
`wait_why` and branch on `fault`; see §13, which is written at length because
getting this wrong is how a working `i2c` command was reported as having been
stopped by the scheduler.

**The polite way to stop something**, which is what the `kill` command does:

```c
env->signal(pid, RV9_SIG_STOP);
if (env->wait(pid, &status, 2000) < 0) env->kill(pid);
```

`kill` ends the process at the first point where ending it breaks nothing
else — an ordinary process when it holds no lock, a real-time one between
activations — and applies its declared failsafes as for any other exit. It
returns `-RV9_PE_TIMEOUT` when it could not find such a moment, which is not
the same as having given up.

**`fork_rt` is where admission happens**, and a refusal is specific: see the
seven codes in §13. `RV9_PE_UNSCHEDULABLE` is the one worth reading — the CPU
was sufficient and the *ordering* was not.

### Modules and information

| | since | RT | |
|---|---|---|---|
| `int load(const char *path)` | 9 | no | Read a module file into the store; it becomes runnable by name. |
| `int sysinfo(uint32_t what, void *buf, uint32_t len)` | 5 | no | See §8. Returns how many records it filled, or −1. |
| `int print(const char *s)` | 1 | no | **Do not use.** |

`print` predates SCF — it existed when there was nowhere to write. Anything
now should `write` to standard output, so that it composes through a pipe and
can be redirected. It remains in the ABI because removing it would break
modules built against ABI 1, which is the promise the ABI makes.

`load` is how a module fetched over the network becomes a command without
reflashing: `fetch host /path > /r0/thing.mod`, then `load /r0/thing.mod`.

### Real time

| | since | RT | |
|---|---|---|---|
| `int rt_declare(uint32_t period_us)` | 10 | no | Declare the period and begin. **0 means the one in the manifest.** |
| `int rt_declare_event(int event_id, uint32_t min_interval_us)` | 12 | no | Released by an event rather than a period. |
| `int rt_wait(void)` | 10 | **yes** | Wait for the next release. Returns **releases coalesced**. |
| `int rt_stats(rv9_rt_report_t *out)` | 10 | no | Activations, worst jitter, worst execution, overruns. |

**Pass 0 to `rt_declare`.** The rate is a property of the control law and
belongs written down beside it in `build.conf`, where admission and a compiler
can both read it — not as a constant in the code that nothing outside the
module can see.

**`rt_wait`'s return value is the loop learning it was too slow.** A positive
number is releases that came and went while the body was still running.
Nothing faults; the component is simply told, and can report it, shed work, or
declare `on_deadline=FAULT` and be stopped instead. Negative means the loop is
over — stop.

**The initialisation-then-execution split is load-bearing.** Open every path,
allocate everything, and write once to each before calling `rt_declare`. The
first write through a path is measurably the most expensive: `control` spent
30 µs of a 50 µs budget on activation one warming up a path it had opened but
never used. Anything a loop will touch should be touched before it makes a
promise about how long it takes.

An event-driven component declares `min_interval_us` — the shortest gap it
will tolerate — and admission treats that as the period. Events closer
together than declared are counted as `floods` rather than accepted silently.

## 8. sysinfo

```c
int sysinfo(uint32_t what, void *buf, uint32_t len);
```

One call for everything a program can ask about the machine. It returns **how
many records it filled**, or negative on error — and with `buf == NULL` it
returns how many there are, so a caller can size a buffer before asking.

The convention that matters: **you pass the length you know about.** A record
may grow a field on the end in a later ABI, and an older module asking with
an older `sizeof` gets the prefix it understands rather than a refusal. This
is why `rv9_sys_mem_t` could gain `heap_largest` without breaking anything,
and why `RV9_SYS_MEM_MIN` exists to mark the part that may never move.

| code | records | what you get |
|---|---|---|
| `RV9_SYS_MEM` | one `rv9_sys_mem_t` | Free, available, what is left for programs, the floors, the low-water mark, the largest free block. What `free` prints. |
| `RV9_SYS_MODULES` | one `rv9_sys_module_t` each | The store: name, type, revision, size, live links. What `mdir` prints. |
| `RV9_SYS_PROCS` | one `rv9_sys_proc_t` each | Live processes and remembered dead ones: pid, parent, name, state, priorities, **status and fault separately**. What `procs` prints. |
| `RV9_SYS_RT` | one record | The real-time slots: what is admitted, at what period and deadline, and the load. |
| `RV9_SYS_STACK` | one `rv9_sys_stack_t` per live process | Stack size and how much has never been written. Living processes only — a dead one's stack is gone, and reporting a measurement of memory that no longer exists is worse than reporting none. |
| `RV9_SYS_ADMIT` | one record | Why the last admission decision went the way it did. |
| `RV9_SYS_CLAIM` | one per claim | Who owns which device, exclusively or shared, and any failsafe recorded against it. What `owns` prints. |
| `RV9_SYS_DEVICES` | one per device | Name, file manager, driver, open count. |
| `RV9_SYS_LIMITS` | one record | §14's numbers, from the firmware rather than from this document. |
| `RV9_SYS_BUDGETS` | one `rv9_sys_budget_t` per process | What each process is charged, what it holds including descendants, and what it is allowed. |
| `RV9_SYS_STACK_PEAKS` | one per module run | The high-water mark a module reached last time it ran, which is how a `stack_size` gets right-sized instead of guessed. |
| `RV9_SYS_LOG` | bytes | The log ring, oldest first. What `log` prints, and what makes `log \| match error \| last 20` possible. |
| `RV9_SYS_CLOCK` | one `rv9_sys_clock_t` | Wall-clock time and whether it has ever been set. UTC only. It says *false* rather than lying when the network has never been asked. |
| `RV9_SYS_HAVE` | one `rv9_sys_device_t` | **A question, not an enumeration.** Name in, device record out, returns 1 or 0. See §18. |

## 9. Device settings: getstat and setstat

> **To write:** the codes, by discipline. Generic PIO
> (`SS_DIRECTION`, `SS_PULL`, `SS_FREQUENCY`, `GS_RANGE`, `SS_EDGE`,
> `GS_EVENT`, `GS_PULSE_US`, `GS_PULSES`, `GS_PERIOD_US`), IFM
> (`SS_REG`, `GS_PRESENT`), console (`GS_SIZE`, `SS_CURSOR`, `SS_COLOUR`,
> `SS_ATTR`, `SS_CLEAR`, `SS_CURSOR_ON`, `GS_ONSCREEN`), publication and
> RBF.
>
> **Known gap:** these are not in `rv9-profile.json`, which means a compiler
> reading the profile cannot see them. They should be. Raised here rather
> than quietly omitted.

## 10. Publication

> **To write:** one writer, declared; `rv9_pub_t`'s 16-byte head; the
> sequence number and what `torn` means; `stamp_us` being when the reading
> was *taken*; the blocking `getstat` wait so a watcher re-evaluates on
> change rather than polling; that closing a cell does not erase it, because
> the last thing a stopped loop measured is what an investigation wants.

## 11. The real-time class

> **To write:** admission by response-time analysis rather than utilisation;
> priority derived from deadlines; the utilisation ceiling; the 2 ms runaway
> watchdog and what it does; `on_deadline`; the slot count; what a refusal
> message contains and how to read it.

## 12. Memory

> **To write:** the classes and their floors — ordinary work refused 8 KB
> before a real-time loop would be, the failsafe path lower still; per-process
> budgets and how a charge is attributed to ancestors; what `free` reports
> and what each line means. Cross-reference [memory.md](memory.md), which is
> the measured account.

## 13. Faults and errors: two number spaces

This is the one place where reading the reference carelessly will produce a
bug, so it is stated at length.

A module returns an `int`. RV-9's own verdicts are also `int`s, and they
overlap. `RV9_PE_FAULT` is 6; a tool returning `-6` to mean "that address did
not answer" was for some time announced by the shell as having been *stopped
by the scheduler*. It had run perfectly.

**A status and a fault are two different questions.** Ask both:

```c
int status = 0, fault = RV9_FAULT_NONE;
env->wait_why(pid, &status, &fault, RV9_WAIT_FOREVER);

if (fault != RV9_FAULT_NONE) {
    /* RV-9 stopped it. `status` is not the program's answer. */
} else {
    /* It returned under its own power. `status` means whatever it says. */
}
```

`wait()` still exists and still hands back only a status. It is not wrong,
but anything deciding *what happened* should use `wait_why`.

### Advice: return positive codes

A module's own conditions should be **positive**. Everything RV-9 hands back
is negative, so a positive return can never be mistaken for a verdict — and
`i2c` was changed to do this after the incident above. It is a convention
rather than a rule, and `wait_why` means it is no longer load-bearing, but it
costs nothing and removes a class of confusion.

### Faults — RV-9 stopped the process

Five values, in `fault`. `r9` is the reason R9 §15.3 expects to see.

| fault | value | r9 | cause |
|---|---|---|---|
| `RV9_FAULT_NONE` | 0 | — | It returned under its own power. `status` is the program's. |
| `RV9_FAULT_STACK` | 1 | STACK | It wrote below the floor of its own stack. Caught when it is switched away from, or when it returns. |
| `RV9_FAULT_KILLED` | 2 | — | Stopped from outside, by `kill`. |
| `RV9_FAULT_DEADLINE` | 3 | DEADLINE | It missed a deadline it declared fatal (`on_deadline=FAULT`). |
| `RV9_FAULT_RUNAWAY` | 4 | DEADLINE | A real-time loop stopped coming back to wait, and the 2 ms watchdog noticed. |

In every case the declared `failsafe` has already been applied by the time
anything observes the fault, and it was applied *by RV-9*, because the
process that needed stopping is not available to tidy up after itself.

### Process errors — the thing did not happen

Returned negated: `fork` returning `-RV9_PE_NOMEM`. Grouped by what you can
do about them, which is the reason there are seventeen rather than one "no".

*It could not be started:*

| error | value | cause |
|---|---|---|
| `RV9_PE_NOTFOUND` | 1 | No such pid, or no such module. |
| `RV9_PE_NOMEM` | 2 | Not enough memory, with the floors respected. |
| `RV9_PE_MODULE` | 3 | The module is there but will not load: CRC, ABI, or format. |
| `RV9_PE_INVAL` | 5 | The arguments do not make sense. |
| `RV9_PE_BUDGET` | 17 | Starting it would take a process past its own `mem_max`, so a runaway `fork` stops at its limit instead of taking the machine down. |

*Admission refused it* — these are the interesting ones, because each says
something different about **whose fault it is**:

| error | value | cause |
|---|---|---|
| `RV9_PE_NOSLOT` | 7 | Every real-time slot is taken. Something has to be stopped first. |
| `RV9_PE_CONTRACT` | 8 | The declaration contradicts itself — a deadline longer than the period, an execution bound larger than the deadline. A `build.conf` line to fix. |
| `RV9_PE_UTILISATION` | 9 | The CPU is already promised elsewhere. A statement about the machine, not about the program. |
| `RV9_PE_UNSCHEDULABLE` | 16 | No placement of the real-time work meets every deadline. Utilisation was fine; the *ordering* is not achievable. This is the refusal that makes admission more than arithmetic on load. |
| `RV9_PE_NODEV` | 10 | It needs a device this machine does not have. |
| `RV9_PE_BUSY` | 11 | It needs a device *alone* and somebody already has it. |
| `RV9_PE_NOPUB` | 15 | It `watches` a cell that nothing on this machine `publishes`. |
| `RV9_PE_MEANING` | 18 | It watches a cell that means something else. Both sides declared a unit and they disagree — see §17. |

*It ran and then ended* — these arrive as an exit status, and are the ones
`wait_why` exists to disambiguate from a program's own return:

| error | value | cause |
|---|---|---|
| `RV9_PE_FAULT` | 6 | RV-9 stopped it; `fault` says why. **The number that collided.** |
| `RV9_PE_KILLED` | 12 | Stopped from outside. |
| `RV9_PE_DEADLINE` | 13 | Missed a fatal deadline. |
| `RV9_PE_RUNAWAY` | 14 | Stopped waiting for its releases. |

*And one about waiting:* `RV9_PE_TIMEOUT` (4) — `wait` gave up, or `kill`
could not find a moment at which ending the process breaks nothing else. In
the second case it has **not** given up: a real-time process asked to stop
still stops at its next release.

### I/O errors — the path operation did not happen

Also returned negated. Twelve values, and the order is ABI: existing numbers
never move, which is why `RV9_IO_ERR_TIMEOUT` and `RV9_IO_ERR_BUSY` are at
the end rather than in a sensible place.

| error | value | cause |
|---|---|---|
| `RV9_IO_ERR_NOTFOUND` | 1 | No such device, or no such file on one. |
| `RV9_IO_ERR_BADPATH` | 2 | That path number is not open. |
| `RV9_IO_ERR_NOPATHS` | 3 | This process's path table is full — twelve of them. |
| `RV9_IO_ERR_NOMEM` | 4 | Not enough memory for the path's own state. |
| `RV9_IO_ERR_MODE` | 5 | Not open for that operation: writing a read-only path, or driving a pin opened for reading. |
| `RV9_IO_ERR_UNSUPPORTED` | 6 | The file manager or the driver cannot do this at all — seeking on a bus, a getstat code the device has never heard of. |
| `RV9_IO_ERR_WOULDBLOCK` | 7 | Nothing to read and the caller asked not to wait. |
| `RV9_IO_ERR_IO` | 8 | The hardware said no. |
| `RV9_IO_ERR_INVAL` | 9 | The arguments do not make sense — an I²C address above 0x77, a negative length. |
| `RV9_IO_ERR_EXISTS` | 10 | It is already there, or the pin is spoken for by the board (the USB console, the display, the card). |
| `RV9_IO_ERR_TIMEOUT` | 11 | The device did not answer in time. Distinct from `IO` on purpose: a slow device and a broken one need different reactions. |
| `RV9_IO_ERR_BUSY` | 12 | Somebody else owns it exclusively. |

Two of these are worth remembering because they read like failures and are
not: an I²C address that does not answer is reported as *absent*
(`RV9_IO_ERR_NOTFOUND` from a probe) rather than as a fault, which is what
makes scanning a bus reasonable instead of a hundred error messages; and
`RV9_IO_ERR_WOULDBLOCK` is a normal answer to a normal question.

## 14. Limits

> **To write:** a table straight from the profile — path slots per process,
> pid range, process history depth, real-time slots, publication name
> length, the watchdog and runaway intervals, the utilisation ceiling.
> Generated rather than typed, so it cannot drift.

## 15. The boot suites

> **To write:** what each of the ten suites proves, and why a system ships
> with its tests running at every boot. Reading a suite's output is the
> fastest way to find out whether a board is healthy, and `sched`'s control
> run is the one to read first.

## 16. Keeping this document honest

`tools/checkdocs.py` runs in `tools/hosttest/run.sh`, beside
`mkprofile.py --check`, and **fails the host tests** when the firmware
defines a name this document does not mention. There is a chain of custody
and each link is enforced:

```
the sources       -- mkprofile.py --check -->  rv9-profile.json
rv9-profile.json  -- checkdocs.py         -->  docs/reference.md
```

So a system call added to `module.h` reaches this document without anybody
remembering to tell it. At the time of writing it checks 123 names: the 34
calls, the 20 manifest tags, 5 faults, 17 process errors, 12 I/O errors, 13
sysinfo codes, 7 file managers and 15 drivers.

*Mentioned* means the name appears in backticks somewhere in the file. That
is a deliberately low bar — a bare substring search would pass vacuously for
`open` and `read`, which are the entries most worth documenting — and it is a
test for **absence**, not for quality. A low bar that is actually enforced
beats a high one that is not.

What it cannot check is whether any of the prose is true. That is what the
board is for.

### Known gap

The getstat/setstat codes in §9 are **not** in `rv9-profile.json`, so they are
not covered by the chain above, and more importantly a compiler reading the
profile cannot see them at all. A program cannot set a pin's direction, arm an
edge or read a pulse width from what the profile currently says. That is a
hole in the contract rather than in the documentation, and it should be
closed in `mkprofile.py`.

## 17. Declared meaning

A `publishes` or `watches` declaration may say what the value *means*, as a
unit after a colon:

```
publishes="/pub0/DISTANCE:mm"        # sonar says millimetres
watches="/pub0/DISTANCE:mm"          # admitted
watches="/pub0/DISTANCE:m"           # refused
```

```
E rv9-io: admit 'st-watch-m': it watches /pub0/DISTANCE in m,
          and the publisher writes mm
```

Checked at **admission**, before the program runs, against the publisher's
declaration — which works whether or not the publisher is currently running,
because the declaration is in its manifest and the manifest is on the machine.
The refusal is `RV9_PE_MEANING`.

This is R9's dimensional guarantee surviving past the compiler. R9 makes a
metres/feet confusion a compile error *within* a program; this catches it
*between* two separately compiled components that meet on one machine.

Three rules, each chosen deliberately:

**Exact match, no conversion.** `m` against `mm` is refused, not scaled.
Converting would mean agreeing a dimensional algebra between separately
compiled components, and getting that wrong is the failure the check exists to
prevent. Refusing is the conservative answer and it is what R9 already does at
compile time.

**Silence is not a mismatch.** A declaration without a unit is legal, and a
cell nobody has described stays usable. Most things have no unit, and a system
that demanded one would be lying about pipes and pins. It also means every
program already on the board still runs.

**Units are free text, bounded at `RV9_MEANING_MAX` (12).** Long enough for
`deg/s` and `m/s2`. No table of legal units, because a closed set would have to
be agreed with R9 and with every future device, and disagreeing about the
*spelling* of a unit is a smaller problem than disagreeing about the unit.

### Asking at runtime

Enforcement is not the whole of it: a program can also ask.
`RV9_PUB_GS_MEANING` fills a `char[RV9_MEANING_MAX]` with the cell's declared
unit, empty when nothing was declared.

```
rv9> pubs
name                 means  seq   bytes   cap  age_ms  by   rdrs  torn  note
LATELOOP             -      35    4       64   23547   -    0     0     killed
CONTROL              -      203   12      64   23559   -    0     0     killed
DISTANCE             mm     6     16      64   10423   -    0     0
```

Two details worth knowing.

**It is a separate getstat code, not a field appended to `rv9_pub_info_t`, and
that is an ABI rule rather than a preference.** `getstat` passes a pointer and
no length, so growing that struct would have the firmware write past the buffer
of every module compiled against the smaller one. `sysinfo` records may grow
because callers pass the size they know about (§8); getstat structs may not.
A new code costs nothing and breaks nothing, which is how IFM added its two.

**The meaning outlives its publisher.** It is recorded at the writer's
admission and kept after the process is gone, beside `reserved_by` and
`fault` — because "what did this cell mean" is a question asked *about* a
component that has stopped at least as often as one that is running. A later
publisher declaring no unit does not erase it, for the same reason: silently
becoming "unknown" would be worse than staying true.

## 18. Asking what this machine can do

RV-9 runs on boards that differ in kind, not only in size — a C5 has a radio
on the chip and a P4 does not; one panel is 172×320 over SPI and another
1024×600. A program that must work on both needs a way to ask.

**There are three questions, and they already had three answers.**

| question | how |
|---|---|
| Is it here at all? | `RV9_SYS_HAVE`, or `RV9_SYS_DEVICES` to enumerate |
| How much of it? | `getstat` the device — `RV9_GS_SIZE`, `RV9_PIO_GS_RANGE` |
| What are the system's numbers? | `RV9_SYS_LIMITS` |

### RV9_SYS_HAVE

On entry the buffer holds a NUL-terminated name; on success it is overwritten
with that device's `rv9_sys_device_t` and the call returns 1, or 0 when the
machine has no such thing.

```c
rv9_sys_device_t d;
if (m_have(env, "svgwin", &d)) { /* ... open d.name ... */ }
```

What the name matches:

| | |
|---|---|
| starts with `/` | the device: `"/i2c0"` |
| anything else | the driver, then the file manager: `"svgwin"`, `"scf"` |

**The second form is the one that makes a portable program possible.** *"Is
there anything I can draw on"* is a question about a **driver**, and asking by
driver means not needing to know the device is `/w0` here and something else
there. Only the first match is returned; a caller wanting every RBF volume
should enumerate instead.

`RV9_SYS_DEVICES` answered this already, and remains the right call for a
toolchain interrogating the board. `RV9_SYS_HAVE` exists because a *program*
asking one question had to buffer every record — about 900 bytes of statics on
this machine, which is why nothing did it. One record is 56.

### Why there is no list of capability flags

A fixed list — `HAS_TOUCH`, `HAS_CAMERA` — is a closed set, and every new kind
of hardware needs a new bit and an ABI bump to carry it. A device already
describes itself in three open-ended fields: **name, file manager, driver**. So
a device nobody has invented yet is answerable by this call on the day its
descriptor is written, with no change to the ABI at all.

This is the same reasoning that keeps units as free text rather than a table
(§17), and it is the payoff of the manager/driver/descriptor split being real
rather than notional.

### The convention

| | |
|---|---|
| **Must have it** | Declare it in the manifest. Admission refuses the program with `RV9_PE_NODEV` *before it runs*, naming what was missing. |
| **Would like it** | Ask with `RV9_SYS_HAVE` and do without when the answer is 0. |
| **How much of it** | `getstat` the device once open. |

That is what lets one binary serve a small board and a large one: **require
only what is essential, and ask about the rest.** `ed` is the worked example
already in the tree — it asks the screen its size on every redraw, so the same
binary runs at 236 columns over SSH and 30×8 on the panel.

The `have` command is this at the shell, and its exit status is the answer so
it composes:

```
rv9> have svgwin && echo can draw
can draw
rv9> have camera || echo no camera here
no camera here
```

---

## Licence

Apache License 2.0 — see [LICENSE](../LICENSE) and [NOTICE](../NOTICE).
