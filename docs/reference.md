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

> **To write:** the grammar — `/dev` and `/dev/unit` and `/dev/a/b/c`; which
> managers take what; the path table's size (`max_paths`, 12) and that slots
> 0, 1 and 2 are stdin, stdout and stderr by convention rather than by rule;
> `dup2` and inheritance across `fork`; that `open` returns the lowest free
> slot, which is a fact you can get wrong (see design §47's neighbours).
> Table of every path as shipped.

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

## 4. Devices as shipped

> **To write:** all fifteen drivers and their descriptors, with the
> descriptor options each takes (`opt[0..]`) and its default — for example
> `/i2c0` takes SDA, SCL and kHz, defaulting to 8, 9 and 100. Note which
> pins are reserved by the board and refused (`/gpio` refuses the USB
> console, display and card pins).

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

Thirty-four calls, arriving as a struct of function pointers. No libc, no
globals, no kernel calls: everything a module can do is here.

> **To write:** all thirty-four, grouped — lifecycle, paths, processes,
> information, real time — each with its signature, what it returns, its
> error codes, its real-time safety, and the ABI it arrived in. The grouping
> to use:
>
> - **itself**: `abi_version`, `statics`, `statics_size`, `pid`, `arg`
> - **time and yielding**: `time_ms`, `time_us`, `sleep_ms`, `yield`
> - **paths**: `open`, `close`, `read`, `write`, `seek`, `dup2`, `getstat`,
>   `setstat`, `remove`
> - **processes**: `fork`, `fork_arg`, `fork_rt`, `wait`, `wait_why`,
>   `chain`, `signal`, `kill`, `signals_take`
> - **modules**: `load`
> - **information**: `sysinfo`, `print`
> - **real time**: `rt_declare`, `rt_declare_event`, `rt_wait`, `rt_stats`
>
> Two things to be explicit about. `wait_why` (ABI 14) is the one that
> separates what a program returned from what RV-9 decided about it, and
> without it the two share a number space — see §13. And `rt_wait` returns
> the number of releases *coalesced*, which is how a component learns it was
> too slow without being told by a fault.

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

---

## Licence

Apache License 2.0 — see [LICENSE](../LICENSE) and [NOTICE](../NOTICE).
