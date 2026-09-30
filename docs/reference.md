# RV-9 — Reference

**The contract: what RV-9 promises a program, what it requires of one, and
what it enforces.**

> **Status: all eighteen sections written**, and kept honest by the build
> rather than by intention.
>
> Where this document lists something the firmware defines — a system call, a
> manifest tag, a fault code, a getstat code — the list is **complete**, and
> `tools/hosttest/run.sh` fails if it stops being. §14's numbers are compared
> against the generated profile on every run. See §16.

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

A module is one blob: a 40-byte header, a name, an optional manifest (§6),
then code and read-only data as a single unit. Little-endian throughout.
Everything is an offset from the start of the module, never an address, and
that is the whole of what makes it relocatable.

### The header

| at | field | |
|---|---|---|
| 0 | `magic` | `0x4D395652` — `RV9M` little-endian. |
| 4 | `header_len` | 40. Checked, so a future longer header is refused rather than misread. |
| 6 | `abi_version` | What it was built against. |
| 8 | `module_len` | Total bytes, header included. |
| 12 | `name_offset` | The NUL-terminated name. |
| 16 | `entry_offset` | Where execution begins. |
| 20 | `static_size` | Per-instance storage the loader must provide. |
| 24 | `stack_size` | A hint; 0 means the loader decides. |
| 28 | `type` | `PROGRAM`, `LIBRARY`, `FILEMGR`, `DRIVER`, `DESCRIPTOR`, `DATA` or `SYSTEM` (1–7). |
| 29 | `attr` | Reserved. |
| 30 | `revision` | **Higher wins when names collide.** |
| 31 | — | Reserved. |
| 32 | `crc32` | Over the whole image with this field taken as zero. |
| 36 | `manifest_offset` | 0 when there is no manifest. |

**The ABI check runs one way.** A module declaring an ABI *higher* than the
firmware is refused with `RV9_MOD_ERR_BADABI`; a lower one runs. Fields are
only ever appended to the environment, so old modules keep working forever
and that is a promise rather than an accident — `print` is still in the ABI
for exactly this reason (§7).

That check is correct and is currently unexercised, which is worth writing
down rather than leaving to be discovered. `tools/mkmodule.py` stamps every
module it builds with `abi_version` 1, whatever the module actually uses, so
no module this toolchain produces can trip the check. A module calling
`wait_why` — ABI 14 — would be admitted by ABI 13 firmware and would call
through a null pointer. Nothing shipped is exposed to this, because modules
and firmware are built and flashed together, but the field does not yet mean
what the header says it means. The profile records a `since_abi` for every
environment entry, so the number is computable; deciding *how* to compute it
is the open part, since a module reaches `write` through `m_say` as often as
directly.

**The CRC is the zlib polynomial**, so `tools/mkmodule.py` computes it with
Python's `zlib` and the loader agrees. It is checked with the field taken as
zero, which the loader does without copying the image: three spans, around
the hole.

**Revision is how a module is replaced.** `rv9_mod_find` takes the highest
revision of a name, which is OS-9's behaviour and means upgrading something
is loading a newer copy — not deleting the old one first, from a machine that
may be using it.

### Running in place

The store is a flash partition, mapped for execution once at boot. A module
forked out of it **runs from mapped flash and is never copied into RAM** —
the shell's image alone is nine and a half KB that no longer has to be
resident.

**Except a real-time one**, and the exception is the whole reason the rest is
safe: *a flash erase disables the cache*. Code in mapped flash cannot execute
while another program is writing a file, for milliseconds at a time. An
interactive program can afford that and a control loop cannot, so a module
whose manifest says `class=REALTIME` is copied into RAM and runs from there.
It is the same trade `RV9_RT_CODE` makes for RV-9's own code, landing in the
same place.

A module already resident is shared rather than loaded twice. That is what
reentrancy is for, and it is why the rules below exist.

### The rules, and what broke to find each one

A module has no `.data` and no `.bss`. The linker script asserts both are
empty, so a module with writable statics **fails to link** rather than
appearing to work and then corrupting itself the moment two processes share
it. Private storage comes from `env->statics`, which is per-process by
construction.

| rule | why |
|---|---|
| `-mcmodel=medany` | Every reference becomes PC-relative, so the blob works wherever the loader puts it. |
| no `.data`, no `.bss` | One copy of the code serves every process. Writable statics would be shared statics. |
| `-nostdlib`, `-ffreestanding` | There is no libc to link against. `modlib.h` is header-only for the same reason. |
| no 64-bit division | `uint64_t / n` calls `__udivdi3` in libgcc, which is not there. Narrow to 32 bits first — see `m_age_ms`. |
| `-fno-jump-tables`, `-fno-tree-switch-conversion` | A jump table holds absolute addresses. A dense `switch` gets silently rewritten into one. |
| no `static const` array of pointers | The array holds absolute addresses. A `switch` returning string literals compiles to exactly this. |
| no local array initialised from a literal | `char pre[] = "/gpio/";` becomes a `memcpy` from rodata, and there is no `memcpy`. A pointer to the literal is fine. |

The last three are the ones that bite, because none of them looks like a
pointer table in the source. So position independence is **proved rather than
assumed**: `build_modules.sh` links every module twice, at two different base
addresses, and compares the bytes. PC-relative code is byte-identical
wherever it lands; anything holding an absolute address differs, and the
build stops with the usual causes named.

That check is why `have` walks a string of device names with `m_word` instead
of indexing an array of `const char *`. The array version was correct C, read
better, and was not position independent.

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

### The layout

Entries back to back, each 4-byte aligned, ending at a tag of
`RV9_MTAG_END` or at the end of the module:

```
    uint16_t tag        top bit is RV9_MTAG_MANDATORY (0x8000)
    uint16_t len        bytes of value; padding not counted
    uint8_t  value[len]
    padding to the next multiple of four
```

The tag numbers are the agreement between a compiler and RV-9, fixed once
published: `RV9_MTAG_DESC` is 1, `RV9_MTAG_STACK` 2, and so on through
`RV9_MTAG_PLACEMENT` at 0x14, which is `RV9_MTAG_MAX` — the highest this
build understands. Above it is unknown, and unknown plus mandatory is a
refusal.

**The mandatory bit belongs to the tag, not to a flags field**, and that is
a deliberate choice rather than a saving. It makes the advisory and mandatory
forms of a field two different tags, so a producer decides *per value*
whether being understood matters. The same compiler can emit `heap_max`
advisorily for a shell command and mandatorily for a control loop, without
the format needing to know which is which.

Most of these tags have no consumer in RV-9 yet, and that is the point. A
program may describe itself completely to a system that acts on part of it,
and the rest becomes enforcement later without anything being rebuilt.

Multi-value tags — `device`, `exclusive`, `failsafe`, `capability`,
`publishes`, `watches` — simply repeat. `failsafe` carries a `uint32` value
followed by the path it applies to.

In `build.conf`, a key is made mandatory by suffixing it with `!`, or by
naming it in `mandatory=`. `RV9_MTAG_STATIC` is spelled `static` there, which
is the tag §7's `statics` is sized by.

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

`read` and `write` move data. Everything else you can ask a device — how big
it is, where the cursor goes, which edge to interrupt on, what the radio can
see — is a numbered code through `getstat` or `setstat`.

A device that does not recognise a code answers `RV9_IO_ERR_UNSUPPORTED`,
which is an answer rather than a failure: `m_screen` falls back to 80×24 when
`RV9_CON_GS_SIZE` is refused, so a caller always has something to lay out
against.

Every code is in `rv9-profile.json` under `stat_codes`, with its discipline
and its direction, and this section is checked against it.

### The number space

| range | whose | unique across the machine? |
|---|---|---|
| 1–255 | Generic, or a discipline's. Every device that can answer one, answers it the same way. | **yes** |
| 256+ (`RV9_SS_DRIVER_BASE`) | The driver's own. | **no** |
| 256+64 and up | The file manager's own, kept above the drivers' so the two cannot be confused. | **no** |

The second row is the one to understand. `RV9_LCD_SS_CLEAR` and
`RV9_NET_SS_CONNECT` are **both 256**, and that is correct: a driver code is
only meaningful on a path open to that driver. Nothing is centrally allocated
above the base, which is what lets a new driver define its settings without
asking anyone.

### Generic — any character device

| code | | |
|---|---|---|
| `RV9_SS_ECHO` | 1 | Echo what is read. |
| `RV9_SS_AUTOLF` | 2 | Turn a newline into carriage-return-newline on the way out. |
| `RV9_GS_READY` | 3 | Is there input waiting? |
| `RV9_GS_SIZE` | 4 | The size of the thing — a file's length, on RBF. |
| `RV9_SS_RAW` | 5 | 0 = lines, 1 = keystrokes. |
| `RV9_SS_HANGUP` | 6 | End the session this path belongs to. |

**`RV9_SS_RAW` belongs to the path, not the device.** SCF normally reads a
*line*: it buffers until return, echoes, handles rubout and discards
anything unprintable — right for a shell and wrong for anything that draws,
because an arrow key is `ESC [ A` and the first byte would be thrown away
before any program saw it. In raw mode a read returns whatever has arrived as
soon as it arrives. Ask for several bytes: an escape sequence is more than
one. It is inherited across `fork` with the path, which is what makes setting
it on standard input mean something — so put it back before exiting.

**`RV9_SS_HANGUP` exists because the last close is the wrong moment.** For
`/ssh0`, whoever established the session says when it is over. A background
job started in that session inherited its terminal and would otherwise hold
the session — and the device — for as long as it runs, so nobody else could
log in. After a hangup the next open starts a new session; paths left from
the old one get `RV9_IO_ERR_IO` and **the processes holding them keep
running**. A control loop started over a network link must not stop because
the link did.

### PIO — any device that is a value

Shared by every PIO device, so a program need not know which driver is
underneath.

| code | | |
|---|---|---|
| `RV9_PIO_SS_DIRECTION` | 16 | 0 = input, 1 = output. |
| `RV9_PIO_SS_PULL` | 17 | 0 = none, 1 = up, 2 = down. |
| `RV9_PIO_SS_FREQUENCY` | 18 | Hz, for anything periodic. |
| `RV9_PIO_GS_RANGE` | 19 | The largest value a write may carry. |
| `RV9_PIO_SS_EDGE` | 20 | 0 = off, 1 = rising, 2 = falling, 3 = both. |
| `RV9_PIO_GS_EVENT` | 21 | The event id this unit signals, or 0 if not armed. |
| `RV9_PIO_GS_PULSE_US` | 24 | Width of the last complete high pulse. |
| `RV9_PIO_GS_PULSES` | 25 | How many complete pulses have been seen — a sequence number. |
| `RV9_PIO_GS_PERIOD_US` | 26 | Last rising edge to the one before it. |

**Interrupts are a device setting, not a pin feature.** Arm a unit with
`RV9_PIO_SS_EDGE`, read the event id from `RV9_PIO_GS_EVENT`, and a
real-time process asks `rt_declare_event` to be released by that number.
Nothing about it is specific to a pin: a UART with a character waiting, or a
card finishing a transfer, answers the same two codes.

**The pulse codes are microsecond sensing without the real-time class.**
Whole families of sensor are a pulse whose duration *is* the measurement — a
sonic ranger's echo, a servo frame, a tachometer, a wheel encoder. Both edges
are stamped in the interrupt handler and the subtraction happens there, so
the scheduler's jitter cancels and an ordinary program gets an answer good to
a couple of microseconds. The accuracy comes from where the clock was read,
not from when the program ran.

`RV9_PIO_GS_PULSES` is a sequence number and not decoration — it is how a
caller knows the width it just read belongs to the pulse it was waiting for
rather than to the last one, which matters most when nothing arrived at all:

```c
uint32_t seen = 0, n, width;
env->getstat(p, RV9_PIO_GS_PULSES, &seen);
/* ... provoke the pulse ... */
env->getstat(p, RV9_PIO_GS_PULSES, &n);
if (n != seen) env->getstat(p, RV9_PIO_GS_PULSE_US, &width);
```

A pulse longer than about an hour reads as an hour. The interval is narrowed
to 32 bits, no physical measurement this is for comes close, and a saturating
answer beats a wrapped one.

### IFM — transactions

| code | | |
|---|---|---|
| `RV9_IFM_SS_REG` | 22 | The register a read should be preceded by. `RV9_IFM_REG_NONE` for a plain read. |
| `RV9_IFM_GS_PRESENT` | 23 | Non-zero if anything acknowledged at this address. |

Setting the register makes a read into *write-this-byte-then-read* — one
transaction with a repeated start, which is what almost every sensor
documents and several require. It is a property of the **path**, so two
programs reading different registers of the same chip do not disturb each
other. `RV9_IFM_GS_PRESENT` costs one byte on the bus and is how `i2c scan`
works.

### Console — anything with a screen at the far end

| code | | |
|---|---|---|
| `RV9_CON_GS_SIZE` | 32 | `rows << 16 \| cols`. Get only. |
| `RV9_CON_SS_CURSOR` | 33 | `row << 16 \| col`, both 0-based. |
| `RV9_CON_SS_COLOUR` | 34 | `fg \| bg << 8`; sixteen `RV9_COL_*` names. |
| `RV9_CON_SS_ATTR` | 35 | `RV9_CON_ATTR_*`, the whole set each time. |
| `RV9_CON_SS_CLEAR` | 36 | `RV9_CON_CLEAR_SCREEN`, `_EOL` or `_EOS`. |
| `RV9_CON_SS_CURSOR_ON` | 37 | 0 hides it, 1 shows it. |
| `RV9_CON_GS_ONSCREEN` | 38 | Is this path what the screen is showing? Get only. |

**These are codes and not escape sequences, which is the whole point.**
`/term` is not a terminal — it is a panel with a font renderer, and teaching
it to parse ANSI would be absurd when it has no need of one. A program says
*cursor to 10,20* once and the device decides what that means: `ESC[11;21H`
down a wire, a change of render position on the glass. SCF supplies the
escape sequences for any driver with no opinion of its own, so a new
character driver gets all of this for free. `modlib.h` wraps them as
`m_cursor`, `m_colour`, `m_attr`, `m_clear` and `m_cursor_on`.

Colours are named rather than RGB. A text console thinks in named colours and
a palette is something a device can honour; truecolour, if it is ever wanted,
belongs in a driver code where the dishonesty would at least be local.

**`RV9_CON_GS_ONSCREEN` exists so an incidental write can decline to steal
the screen.** The panel shows whoever painted last, which is the only
workable rule with one display and no spare framebuffer — but it makes no
distinction between a program that meant to draw and a shell echoing the
command you just typed, and the second should not destroy the first. A
terminal at the end of a wire always says yes, because nothing else can be
using it.

### Driver codes

Relative to `RV9_SS_DRIVER_BASE`, and meaningful only on that driver's paths.

| code | | |
|---|---|---|
| `RV9_LCD_SS_CLEAR` | +0 | Superseded by `RV9_CON_SS_CLEAR`, kept because clearing a screen is not worth breaking. |
| `RV9_LCD_SS_BRIGHTNESS` | +1 | 0–100 percent. |
| `RV9_SVG_SS_ROWS` | +8 | `y0 << 16 \| y1` — the half-open row range to repaint. |
| `RV9_NET_SS_CONNECT` | +0 | `rv9_net_creds_t`. Saved. |
| `RV9_NET_SS_DISCONNECT` | +1 | |
| `RV9_NET_GS_STATUS` | +2 | `rv9_net_status_t`. |
| `RV9_NET_GS_SCAN` | +3 | `rv9_net_scan_t`, up to twelve access points. |
| `RV9_NET_SS_FORGET` | +4 | Clear the saved credentials. |
| `RV9_NET_SS_NOWAIT` | +5 | 0 blocks (the default); 1 returns `RV9_IO_ERR_WOULDBLOCK`. |
| `RV9_NET_SS_SHUTDOWN` | +6 | Shut the connection both ways **without closing the path**. |
| `RV9_NET_SS_TRY` | +7 | `rv9_net_creds_t`, not saved. |
| `RV9_SSH_SS_PASSWORD` | +16 | `rv9_ssh_pw_t`, on `/sshcfg`. |
| `RV9_SSH_GS_INFO` | +17 | `rv9_ssh_info_t`: host key fingerprint and who is logged in. |
| `RV9_RBF_SS_FORMAT` | +64 | Empty the volume this path is open on. A *file manager's* code. |
| `RV9_RBF_GS_SPACE` | +65 | `rv9_rbf_space_t`, asked of the volume (`/sd0`, not `/sd0/notes`). |

**`RV9_SVG_SS_ROWS` applies to the next document written and then clears
itself**, because a document is a whole picture by default and a clip left
set by accident would be a window that never fully repaints again.

**A clipped document must carry every pixel of its rows.** The band is
cleared to the *device* background — the `OPT_BG` descriptor option — and not
to what the scene drew there, so a document holding only the widget repaints
the rest of its rows in the device colour and loses whatever was beside it.
`flick` did this for as long as it existed, leaving two black strips beside
its button; no geometric check catches it, because every shape in the
document is where it should be. Carry the backing for the clipped rows only:
a full-screen background rect is bytes re-parsed once per band for pixels
outside the clip. A full
1024×600 redraw costs about 640 ms on a C5-class core and a 48-row strip
about 80 ms, which is the difference between tap feedback being affordable
and not. Bands are full width, so a tall narrow widget costs what a tall wide
one does — a hint to lay widgets out in horizontal bands.

**`RV9_NET_SS_NOWAIT` exists for closing tidily.** TCP sends a reset instead
of a clean finish when a socket is closed with data still unread, and a reset
throws away whatever was in flight — including the last thing the program
printed. Draining before closing needs a read that can come back empty.

**`RV9_NET_SS_SHUTDOWN` is for a different thread than the one using the
path.** A read waiting on a peer that will never send returns at once, as if
the peer had closed, and the path stays valid until its owner closes it.
Closing it instead would free the socket underneath the read. This is how a
hung-up SSH session gets a background job's read to let go.

**`RV9_SSH_SS_PASSWORD` is on `/sshcfg` and not on `/ssh0`** because `/ssh0`
blocks: a password that could only be set through a device that waits for a
login would be a password you could never set the first time.

**`RV9_RBF_GS_SPACE` is asked of the volume, not of a file.** `RV9_GS_SIZE`
answers for a file and there was no way to ask about the device — which
mattered little on a 16 KB RAM disk and matters on a 32 GB card. It is
counted from the bitmap when asked rather than kept running, because a wrong
cached figure would be worse than a slow true one.

## 10. Publication

A cell is where a real-time component leaves **what it observed**, so that
something slower — a supervisor, a log, an operator — can read it without
touching the loop that produced it.

It is not a lock, a queue, a mailbox or an RPC. R9 §19's *inputs* are this
same object with the ownership reversed: the supervisor publishes, the
control loop observes, and nothing is added for it.

`/pub0` as shipped: **sixteen cells of 64 bytes**, preallocated at boot and
never freed. Sixteen is more independent components than this machine will
run, and 64 bytes holds sixteen 32-bit values published together. Names are
at most 24 characters.

### One writer, declared

A component names the cell it owns in its manifest — `publishes` — not the
device:

```
publishes="/pub0/CONTROL"
```

`/pub0` carries every other component's cells too, and claiming the whole
device would stop them. The cell is created if need be and **reserved at
fork**: a second copy of the program is refused before it starts, and another
process opening that cell to write is refused at open.

A watcher declares `watches` and is admitted when *some module on the machine
declares it publishes that cell* — whether or not the publisher is running.
Start order is not a contract; a cell nothing will ever publish is
`RV9_PE_NOPUB`, refused at fork rather than waited for forever.

§17 covers the other half: both sides may declare what the cell *means*, and
a disagreement about units is refused at admission with `RV9_PE_MEANING`.

### The head

Every read fills a `rv9_pub_t` and then as much of the value as the buffer
holds; every write supplies both together. The same object goes both ways on
purpose — what comes out of one cell can go into another unchanged, which is
what a bridge or a recorder needs.

| field | |
|---|---|
| `seq` | 0 means **never published**; it counts up by one per publication. R9 §18's validity indication and its sequence number in one value. Ignored on write: the cell owns it. |
| `len` | Bytes of value following the head. Always the **true published length**, even when the caller's buffer was too small. |
| `stamp_us` | When the **observation was taken**, not when it was published. Passing 0 means "now". |

`stamp_us` being the observation and not the publication is the field people
get wrong. They differ by however long the computing took, and a reactive
layer deciding how stale a reading is needs the first one.

A reader that asked for too little gets a truncated value and a `len` that
tells it so. What is never truncated is **coherence**: the bytes handed back
are all from one publication.

### Reading without stopping the writer

A cell is a seqlock. Publishing costs two stores and a memcpy with no bound
it can miss; all the cost of contention lands on the observer, which is the
process that can afford it. That is what makes the real-time half of R9 §18's
contract keepable.

An observer that is preempted mid-snapshot needs exactly one retry on one
core — the writer runs to completion before the reader is scheduled again.
RV-9 allows eight, and then **gives up and counts it**:

```
rv9> pubs
name                 means  seq   bytes   cap  age_ms  by   rdrs  torn  note
```

`torn` is snapshots abandoned since boot. It is a real condition worth
seeing — publications arriving faster than a snapshot can be taken — and not
a reason to spin inside the I/O manager with a deadline running.

### Waiting instead of polling

R9 §21 asks that a watcher re-evaluate when a value changes. `getstat` with
`RV9_PUB_GS_WAIT` blocks: pass the sequence last seen, and it returns when
the cell has moved past it, with the current sequence in its place.

- A publication arriving while nobody is waiting is **not lost** — the
  comparison is against the sequence, not against an edge.
- Several arriving together **coalesce into one wakeup**, which is what §21
  wants.
- `timeout_ms` bounds it. `RV9_WAIT_FOREVER` blocks; **zero makes it a poll**,
  which is what a real-time observer should use.

Four observers may be blocked at once across the whole device. The semaphores
are made at mount and handed out, because a semaphore taken from the heap
while a control loop is running is the kind of thing this file manager exists
to avoid. *Reading* a cell needs no slot — only blocking on one does.

`RV9_PUB_GS_INFO` fills a `rv9_pub_info_t`: name, sequence, length,
capacity, stamp, the writer's pid, the reader count, whether it is `held`,
and the fault its publisher died of. `held` and `writer` are two questions
because a cell opened by the system has no pid, and reporting that as
`writer == 0` would make "nobody is publishing this" and "RV-9 itself is" the
same answer.

**A fault is itself a publication.** R9 §15.1 has a faulted component publish
that it faulted; the sequence moves on by one, so a watcher blocked in
`RV9_PUB_GS_WAIT` wakes up and finds out. The fault is recorded in R9's
names, so a runaway reads as DEADLINE.

### The cell outlives the publisher

Closing a cell does not erase it. A control loop that has stopped leaves
behind the last value it published and the moment it observed that value,
which is precisely what an operator or a supervisor arriving afterwards
needs. Cells are claimed for the life of the system, not the life of a
process.

Both states at once, with `control` running and `lateloop` long since
stopped:

```
rv9> pubs
name         means  seq   bytes  cap  age_ms   by  rdrs  torn  note
LATELOOP     -      35    4      64   4294967  -   0     0     killed
CONTROL      -      4560  12     64   0        83  0     0     declared by 83
```

`CONTROL` is live: `by 83`, `declared by 83`, a sequence that moved to 4,908
by the next `pubs` a third of a second later, and `torn 0` — nothing was
outrun. `LATELOOP` is what a stopped component leaves: its last publication,
still readable, noted as `killed`.

That `age_ms` of 4,294,967 is a ceiling, not a measurement. `m_age_ms`
narrows to 32 bits before dividing — a module has no 64-bit division (§5) —
and anything older than about 71 minutes is reported as the ceiling rather
than wrapped into a small and plausible lie.

## 11. The real-time class

A real-time process is one RV-9 has **agreed** to run. The agreement is made
at `fork_rt`, before the program starts, and either it is kept or the program
does not run — which is the difference between a deadline and a hope.

Four slots (§14). The contract is:

```c
env->rt_declare(0);                 /* 0: the period from the manifest */
for (;;) {
    int missed = env->rt_wait();
    if (missed < 0) break;          /* the loop is over */
    /* ... the work, every period ... */
}
```

Everything the loop will touch is opened, allocated and written to **before**
`rt_declare`. See §7: the first write through a path is measurably the most
expensive, and a promise made before the expensive part is a promise about
the wrong number.

### Admission is not arithmetic on load

Two tests, in this order, and they refuse for different reasons.

**The utilisation ceiling** (§14, 70 %) is a flat refusal:
`RV9_PE_UTILISATION`. What the headroom is for is everything that is *not*
in the sum at all — the radio, the panel, the SPI driver, RV-9's own kernel,
and every ordinary process. Admitting real-time work up to the last percent
starves the system the real-time work depends on.

**Response-time analysis** is the real test, and a set using 8 % of the CPU
can fail it. For each component, iterate

```
R = C + sum over j that can run ahead of it of ceil(R / T_j) * C_j
```

to a fixed point or until it passes the deadline. `C` is the declared
`wcet_us`, or the worst execution yet *measured* when nothing was declared.
`T` is the period. `D` is `deadline_us`, or the period when no deadline was
declared. A component whose `R` exceeds its `D` means the whole set is
refused with `RV9_PE_UNSCHEDULABLE` — the CPU was sufficient and the
*ordering* was not.

**The analysis is of the whole set, not of the newcomer.** Admitting one
component may move the ones already running, and an `RV9_PE_UNSCHEDULABLE`
refusal names whichever component would miss — frequently not the one being
admitted. Read it as *there is no arrangement under which everybody makes
it*: the fix is a period, a deadline or an execution bound somewhere in the
set, not necessarily in the program that was refused.

Here are both tests on the machine. `heavyloop` (180 ‰) and `fastloop`
(500 ‰) are running; `control` wants 50 more:

```
E admit 'control': wants 50 permille, 680 already promised, ceiling 700
control: the CPU is already promised

rv9> rt
real-time promised  68.0% of 70.0%
slots               2 of 4
declared            2
measured            0
unaccounted         0

slot  period  deadline  runs     bound  worst  misses
0     100000  100000    routine  38000  16395  0
1     5000    3000      urgent   2500   2036   0
```

`bound` is the response-time analysis's answer; `worst` is what has actually
been seen. `heavyloop` moved to **routine** because its deadline is the
longer, and `fastloop` stayed **urgent** — the derivation below, in one line
of output.

Its bound of 38,000 µs is the formula, and it is worth following once.
`heavyloop` is charged for every release of `fastloop` that can land inside
its own response: 18,000 → ⌈18000/5000⌉·2500 = 10,000 → 28,000 → 15,000 →
33,000 → … → a fixed point at 38,000, well inside its 100,000 µs deadline.
`fastloop`, urgent, is charged for nobody: its bound is its own 2,500 against
a 3,000 deadline.

### Priority is derived, never chosen

R9 §13 asks that nobody pick these numbers. The host offers two useful
levels, so the derivation is a *placement* rather than a ranking:

1. Everything starts **urgent**. A set whose urgent components all meet their
   deadlines together stays there — which is every set of one loop.
2. While some urgent component cannot, the least urgent of them — the longest
   deadline — moves to **routine**, where the urgent ones no longer wait on
   it.
3. Then everything routine must meet its deadline too, counted against every
   urgent component *and every other routine one*. Components sharing a level
   time-slice, so each is charged for all of its peers: the analysis is
   pessimistic there on purpose.

`urgent` sits above every host task, the radio included. `routine` sits above
every ordinary process and below the radio.

`placement` in a manifest pins a component to one level. A pin **constrains
the analysis rather than overriding it**: if no arrangement honouring the
pins meets every deadline, the program is refused rather than admitted into a
set that cannot work.

This existed because of a specific failure. Before it, every real-time task
ran at one host priority and time-sliced. A 1 kHz loop with a tight deadline,
released while a slow loop was in the middle of 20 ms of work, waited for the
next tick to share the CPU — and was stopped for a deadline **the scheduler
had missed on its behalf**.

### What cannot be analysed

A component with no release bound — an event source that declared no
`min_inter_us` — cannot be put in the sum. It stays urgent, counts against
nobody, and is reported as *unaccounted*. So is a component that declared no
`wcet_us` and has not run yet.

That is why `rt` prints three counts under the load — `declared`, `measured`
and `unaccounted` — rather than a single figure, and why admission logs the
load as *a floor* when any of it is measured or unaccounted. A number with
`unaccounted` beside it is not wrong; it is incomplete, and it says so.

The analysis also does not see the host's own work. Routine components run
below the radio, and their bounds are bounds on RV-9's workload only.

### Being late, and stopping

`on_deadline` decides what a miss means. `REPORT` — the default — counts it:
`rt_wait` returns the number of releases that came and went while the body
was still running, and the component can report, shed work, or carry on.
`FAULT` stops it, applies its `failsafe`, and does not release it again;
`wait_why` then gives `RV9_FAULT_DEADLINE`.

`REPORT` is not permission to stop waiting. Whatever a component declared,
the watchdog looks every 2 ms, and one activation running longer than 250 ms
while on the CPU for at least half of that is a **runaway** (§14): stopped
from outside, failsafe applied, `RV9_FAULT_RUNAWAY`. A late loop is a
component's problem; a loop that never comes back is the machine's.

Failsafes are applied by RV-9 through a fresh detached open, not through the
dying process's own paths — because the process this is for is frequently
one that ran off its stack, and a path it owned is not a thing to trust at
that moment.

## 12. Memory

RV-9 has one heap, shared with ESP-IDF. What makes it usable on a machine
that moves is not how it is allocated but **who is refused first**.

[memory.md](memory.md) is the measured account — where the RAM actually goes
on this board. This section is the rules.

### The floor exists for what RV-9 cannot ask

WiFi, the PHY, the SPI driver and ESP-IDF's own internals allocate straight
from the heap, and when they cannot get what they need they do not return
NULL. They abort:

```
ESP_ERROR_CHECK failed: ESP_ERR_NO_MEM at phy_track_pll_init
abort() was called
```

That is a reboot, inside a layer RV-9 does not own, caused by an unrelated
component's allocation failing. It happened here running the window, an SSH
session and a control loop together. On a vehicle it is the whole system
stopping because a display wanted a buffer.

Everything RV-9 allocates goes through the KAL and *can* be told no. So the
last few kilobytes are never offered to it: a request that would go below the
floor returns NULL, the process manager says "no memory to start it", and the
machine stays up.

**The floor does not make more memory exist.** It decides which of two
failures happens — a refusal RV-9 can report, or an abort it cannot catch —
and only one of those leaves a system running. It is a convention rather than
a wall: nothing stops ESP-IDF spending the reserve, the point is that RV-9
does not spend it first.

`RV9_HEAP_FLOOR` is 12,288 bytes by default. Zero disables it, which is how
RV-9 behaved before phase 4.

### Three classes, three floors

One floor decided whether RV-9 or ESP-IDF failed. It did not decide *which
part of RV-9*: a shell session starting background jobs could take the last
kilobyte, and then a control loop could not be admitted, a failsafe could not
open the device it had to park — that open allocates — and `kill` could not
be started to stop whatever was responsible.

So every allocation is made in a class, taken from whoever is asking:

| class | who | stops at |
|---|---|---|
| `RV9_MEM_GENERAL` | Every process, by default. | the floor **plus** the 8 KB real-time reserve — 20,480 bytes |
| `RV9_MEM_REALTIME` | Admitting a real-time loop, and that loop's own set-up. | the floor — 12,288 bytes |
| `RV9_MEM_SYSTEM` | Failsafes, `init` restarting a service, RV-9's own host tasks. | *half* the floor — 6,144 bytes |

The ordering is the design: **ordinary work is refused 8 KB before a
real-time loop would be, and the failsafe path lower still.** The other half
of the floor stays ESP-IDF's, because ESP-IDF aborts rather than fails.

The reserve is sized for what admitting a loop actually costs, not for
comfort: a control loop's stack and statics are about two kilobytes, its
set-up opens a few hundred bytes more, and a failsafe's detached open is less
than that.

The check is asked against the *default* heap for every allocation, including
the DMA and executable ones — on this board they are the same physical memory
seen through different capability masks, and treating them as separate pools
would let the reserve be spent three times over. It is also deliberately
pessimistic rather than exact, since the allocator has a header and rounds
up: a floor accurate to the byte is a floor that has been crossed.

### Per-process budgets

A separate mechanism, and a separate refusal. A process is charged for its
stack, its statics and its descriptor — nearly all of it at fork, because
modules cannot allocate — and so are its eight nearest ancestors. Exceeding
`mem_max`, or the 32,768-byte default, gives `RV9_PE_BUDGET`. §14 has the
numbers and why the accounting is an array.

The two are independent. The floor answers *is there memory on this
machine*; the budget answers *may this family of processes have it*. A fork
can fail either test, and `RV9_PE_NOMEM` and `RV9_PE_BUDGET` are different
answers on purpose.

### Reading `free`

```
heap free      59340
  largest block 34816
available      47052
  for programs 38860
  rt reserve   8192
reserved       12288
refused        3
low water      12400  (since boot; the tests spend to the floor)
  since serving 51636
executable     59340
modules        109
processes      20
```

| line | |
|---|---|
| `heap free` | The total. **Not** what decides whether an allocation succeeds. |
| `largest block` | What does. A heap free in small pieces refuses a large request while reporting plenty of room. |
| `available` | Free, less the floor. |
| `for programs` | What something you type may still take — `available` less the real-time reserve. |
| `rt reserve` | Held back so a loop can be admitted and a failsafe applied after ordinary work has run out. |
| `reserved` | The floor itself. |
| `refused` | Allocations turned away since boot. **Not an error count** — it is the floor doing its job. |
| `low water` | The lowest free has ever been. The boot suites spend deliberately to the floor, so this is usually the tests. |
| `since serving` | The same, from the point the machine finished starting — the number that describes ordinary running. |
| `executable` | Free memory that can hold code. Real-time modules are copied here (§5). |

`low water` and `since serving` are two lines because they answer different
questions and the first one is misleading alone. 12,400 against a 12,288
floor looks alarming until you know the suites drove it there on purpose;
51,636 is what the machine has actually been living on.

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

Every number here is checked against `docs/target/rv9-profile.json` on each
run of the host tests, and the profile is checked against the sources. A
limit that changes in the firmware and not here fails the build — which is
the only reason to trust a table of numbers in a document.

The name in the first column is the profile's, so that anything generating
code from the profile and anything reading this page are talking about the
same quantity.

### A process

| profile | value | |
|---|---|---|
| `max_paths` | 12 | Open paths. `open` fails with `RV9_IO_ERR_NOPATHS`, not by growing the table. |
| `pids` | 1–65535 | 0 is not a pid; `env->pid` is 0 for a module run outside a process. |
| `history` | 16 | Exited processes remembered, so `wait_why` still answers after the fact. |
| `budget_default` | 32768 | Bytes a process and everything it starts may hold when its manifest says nothing. Sized so a shell can hold its largest ordinary command — `ed`, 17 KB with its statics — with room left. Say `mem_max` to want more. |
| `budget_ancestors` | 8 | How far up a charge is carried, so a fork bomb is paid for by whoever started it. |

### Real time

| profile | value | |
|---|---|---|
| `slots` | 4 | Real-time processes at once. The fifth is refused at admission (§11). |
| `utilisation_ceiling_permille` | 700 | 70 %. A set that needs more is refused even when the analysis says it fits. |
| `watchdog_us` | 2000 | How often the watchdog looks — and so the resolution a deadline is enforced to while a loop is *still running*. |
| `runaway_ms` | 250 | One activation longer than this, **and** on the CPU for at least half of it, is a runaway. |

### Memory

| profile | value | |
|---|---|---|
| `rt_reserve` | 8192 | Bytes ordinary work may not touch, so that admission and a loop's set-up always can (§12). |

### Publication

| profile | value | |
|---|---|---|
| `max_name` | 24 | Characters in a cell's name. |
| `head_bytes` | 16 | The fixed head on every cell, before its payload (§10). |

### The module format

| profile | value | |
|---|---|---|
| `abi` | 14 | What this firmware implements. See §7 on `abi_version`. |
| `header_bytes` | 40 | The fixed header before the manifest (§5). |

Two of these are worth a sentence, because the number is not the interesting
part.

**The utilisation ceiling is not the admission test.** Admission is
response-time analysis (§11): it asks whether each component finishes before
its deadline given everything that can pre-empt it, which is a stronger and
sometimes stricter question than whether the total fits in the CPU. The
ceiling sits on top of that as a flat refusal, because a set that passes
analysis at 95 % has no room for the thing nobody modelled — a slow flash
erase, a burst of radio work, the next component somebody adds.

**A runaway is measured, not assumed.** Both halves of that test matter. An
activation blocked for a second in a slow write is late, and being late is
what `on_deadline` is for; an activation that has not come back to `rt_wait`
and is *burning* the CPU is a different thing, and the sample count is how
the one is told from the other. A loop that waits on a slow device is left
alone.

**`budget_ancestors` is why a fork bomb stops.** Modules cannot allocate:
everything a process costs is spent on its behalf by RV-9, and nearly all of
it at fork — the stack, the statics, the descriptor. So the footprint is known
before the program runs, and it is charged to the process *and* to its eight
nearest ancestors. A program that forks children which fork children cannot
escape its own budget by putting the memory one generation further down; it
stops at its own ceiling and the programs beside it keep working.

Budgets nest, which is the useful consequence: a shell's budget includes the
commands it runs, and `sshd`'s includes each session's shell and whatever that
starts. Eight is where the array stops, and it is an array on purpose —
accounting for the heap must not live on the heap it accounts for.

## 15. The boot suites

RV-9 runs its tests every time it starts. Not in a CI job somewhere, not on
request: on the board, on the hardware that will be doing the work, against
the firmware that is about to run.

The reason is that most of what matters here is only true *on a machine*. A
seqlock's contended path, a failsafe reaching a real pin, a priority
derivation under a real scheduler, a card that is actually present — none of
these can be settled by a host test, and a board whose panel is miswired or
whose card has gone is a board that should say so before a program trusts it.

**Reading a suite's output is the fastest way to find out whether a board is
healthy.** `sched`'s control run is the one to read first.

### The ten suites

Captured from a boot on 2026-09-29. The suites are the contract; the counts
are an observation, and they go up as tests are added.

| suite | checks | what it proves |
|---|---|---|
| `kal-test` | 45 | The KAL's own contract, against whichever backend is built in — FreeRTOS today, the native kernel when it lands. Backend-agnostic on purpose: these are how a replacement kernel is known to behave. |
| `conform` | 27 | The portable part of that contract, exercised against an implementation handed in. Host-specific things — DMA memory, heap statistics, critical sections — are not the kernel's business and are checked separately. |
| `mod-test` | 17 | The manifest, against images built in memory: a length running past the end of the module, an offset pointing into the header, an entry that does not advance, an unknown mandatory tag. |
| `io-test` | 44 | Device ownership, against the claim table directly. |
| `pub-test` | 38 | Publication, through `open`, `read` and `write` rather than against PFM's internals. |
| `fault-test` | 76 | Stopping processes, by forking real ones — `deaf`, `lateloop` — and judging them from outside. |
| `proc-test` | 11 | How long a process is remembered, by forking dozens. |
| `sched-test` | 15 | Derived real-time priority, with the workload that needs it. |
| `mem-test` | 13 | What still works when memory has run out. Runs last, because it uses the memory up on purpose. |
| `sd-test` | 7 | The card: a driver, a file manager and a volume a program will trust with a file it cannot get back. |

293 checks, and about thirty seconds from reset to a shell prompt. `sched`,
`sd` and `fault` account for most of that, and all three are waiting on
something real rather than computing.

### Why each is shaped the way it is

**`mod-test` builds broken modules rather than reading good ones.** A
manifest comes off flash, which means it is data written by somebody else and
must be treated as hostile. None of the malformed cases can be produced by
`mkmodule.py`, so a test that only reads well-formed modules tests nothing
that matters — and building the images in memory is also how the refusal path
gets exercised without needing a way to flash a deliberately broken module.

**`io-test` asks the table, not the devices.** Every interesting case is a
conflict between two processes, and arranging two real processes to race over
a real pin at boot is both slow and unreliable. The table is what decides, so
the table is what is asked — with pids that do not exist and resource names
no device answers to, which is safe precisely because ownership is settled
above the drivers and never consults one. The names start with `/test-` so
that a leaked claim shows up in `owns` as obvious wreckage rather than as a
plausible device.

**`pub-test` goes through the calls a module makes.** Ownership is a table
and is tested as one; a publication is a *protocol* between two processes,
and almost everything that could be wrong with it — a sequence that does not
advance, a set that arrives half old, a second writer let in, a wait that
sleeps through a change — is only wrong on the far side of `open`. The one
thing it cannot arrange is a real race, since it runs on one task before
there is any contention; the seqlock's contended path is exercised on the
machine instead, by `control` publishing at 1 kHz while `watch` reads. It
removes its cells at the end, because cells are *not* freed when a publisher
exits (§10) and two left behind would sit in `pubs` until the next reboot.

**`fault-test` uses a pin as evidence.** The pin is driven to 1 before each
loop starts, so that reading 0 afterwards means *RV-9 wrote 0* rather than
that the pin was 0 anyway. Every claim it makes is about something that is
not the test: a thread it started, a process that will not listen, a loop
that is late.

**`proc-test` forks dozens because the bug was a slope.** Every process that
had ever run once kept its descriptor, so the heap went down a little with
every command and never came back. Nothing about one fork is wrong; the
forty-first is where it shows.

**`mem-test` spends the memory on purpose** — threads allocate until refused
and hold what they got — and then asks the questions that matter on a machine
that moves. Can a control loop still be admitted? Can a dying one's actuator
still be parked? Does a program that forks without end take the machine with
it, or stop at its own budget? It runs last and gives everything back.

**`sd-test` must skip cleanly.** A board with no card is the ordinary case,
not a failure. And it must not disturb what is on the volume: one file of its
own, read back, removed. Nothing there formats anything.

### `sched`'s control run

The one to read first, because it is the only suite that proves a *negative*
by demonstration. `fastloop` does 2 ms of work in every 5 and must answer
within 3; `heavyloop` spends 15 ms of every 100. The pair is run twice — once
with the derivation on, once with it switched off:

```
--- a fast loop beside a heavy one, priority derived ---
  pass  admitting the fast loop moved the running heavy loop below it
  (bounds: fast 2500 us, heavy 38000 us)
  (fast loop's worst response 2038 us, status 0)
  pass  the fast loop met every deadline beside the heavy one

--- the same, at one priority: the control ---
  (fast loop's worst response 2259 us, status 0)
  pass  at one priority it answers later than when placed by deadline
```

If the fast loop survived the control run too, the other run would prove
nothing. Those bounds — 2,500 and 38,000 µs — are the response-time analysis
of §11 arriving at the same numbers from a different direction.

What the control run checks is *measured, not assumed*, and that is a
correction. The first version expected the fast loop to be **stopped** at one
priority, which was true on some boots and not others: this host serves a
release of equal priority promptly most of the time, and the false DEADLINE
fault it causes is real but rare. What is reliably true is that the fast loop
answers *later* when it can be made to wait — hundreds of microseconds
against a dozen — so that is what is checked, with a fault counting as later
too.

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
remembering to tell it. It checks **167 names**: the 34 environment entries,
the 20 manifest tags, 5 faults, 17 process errors, 12 I/O errors, 13 sysinfo
codes, 42 getstat/setstat codes, 7 file managers and 15 drivers.

*Mentioned* means the name appears in backticks somewhere in the file. That
is a deliberately low bar — a bare substring search would pass vacuously for
`open` and `read`, which are the entries most worth documenting — and it is a
test for **absence**, not for quality. A low bar that is actually enforced
beats a high one that is not.

### Three other things the build checks

**Structure.** Numbered headings must appear once each and count from 1
without gaps. Added after a duplicated `## 4. Devices as shipped` sat here
for six days: an edit inserted a written section *before* the skeleton it was
meant to replace, and the name check could not notice — every name was
mentioned, twice, which reads as fine.

**§14's numbers.** Every limit is compared against the generated profile, by
the profile's own key. A table of numbers in a document is worthless unless
something fails when it goes stale, and the failure mode is not a wrong
number but a number that was right in March.

**Signatures count.** A backtick span is mined for the identifiers inside it,
so writing `int close(int path)` documents `close`. The first honest draft of
§7 failed on thirteen calls because the check only accepted a span that was
*nothing but* a name — the tool punishing the better documentation. A span
that is only an identifier is still taken as written, which is how the
manifest tag `static` can be named at all.

### What it cannot check

Whether any of the prose is true. That is what the board is for, and most of
what is quoted in this document — the admission refusals in §11, the
publication table in §10, the `free` breakdown in §12, the header offsets in
§5, the path grammar in §2 — was captured from a running machine rather than
written from the source. A log line composed from a format string reads as
evidence later, which is worse than no line at all.

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
