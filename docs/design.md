# RV-9 — Design

A small modular operating system for RISC-V, inspired by the classic OS-9 architecture.

Status: phases 0-6 complete; phase 7 steps 1-2 done, and **the whole
system now runs on RV-9's own kernel** — processes, I/O, storage,
networking and the shell, all scheduled by rv9_kernel — KAL on FreeRTOS (27/27 conformance tests
passing on hardware), module format/directory/loader, and processes with
priority aging, all verified on hardware. See docs/roadmap.md.

---

## 1. What this is

RV-9 is an independent operating system. Some of its design ideas come from
the classic OS-9 architecture: position-independent memory modules, a
unified I/O model, and real preemptive multitasking in a very small
footprint. It rebuilds them on a modern RISC-V microcontroller with WiFi.

It is **not** OS-9, and not a port, version or derivative of it. It is not
affiliated with or endorsed by the owner of OS-9, and it contains no OS-9
code. It has no binary compatibility with OS-9 modules and makes no attempt
to run OS-9 software. OS-9 is a trademark of its respective owner. It is
named in this document only to credit where ideas came from, and
occasionally for historical comparison.

### Goals

- A module format and module directory that make code a first-class runtime object
- An I/O model with file managers, drivers and device descriptors as separate,
  independently loadable pieces
- Preemptive priority scheduling with aging
- Networking that feels like the rest of the I/O system, not a bolted-on socket API
- A kernel small enough to understand completely

### Non-goals

- POSIX compatibility
- Running someone else's binaries
- Beating FreeRTOS at anything measurable

### The honest motivation

The author likes RISC-V and admired the small modular systems of the 1980s.
That is sufficient.

---

## 2. Target hardware

Waveshare **ESP32-C5-LCD-1.47**, verified in hand:

| | |
| --- | --- |
| Core | single RISC-V @ 240MHz, plus an LP RISC-V core |
| RAM | ~328 KB free at boot (measured, phase 0, before WiFi), no PSRAM |
| Flash | 4 MB, memory-mapped, executes in place |
| Display | ST7789 172x320 SPI |
| Storage | microSD over SPI (shares the LCD bus) |
| Radio | dual-band WiFi 6, BLE 5, 802.15.4 |
| Protection | RISC-V PMP (physical memory protection) |

Note on PMP: ESP-IDF uses it to mark data RAM non-executable
(`ESP_SYSTEM_PMP_IDRAM_SPLIT`). Since the module loader jumps into heap
memory, RV-9 disables that split for phases 1-6. This is not a permanent
retreat from memory protection — in phase 7 RV-9 programs the PMP itself and
marks module regions executable explicitly, which is stricter than the IDF
default, not weaker.

The RAM figure is the dominant constraint in every decision below. It is
measured on hardware with nothing but the KAL resident; WiFi and lwIP will
take a large bite out of it. Systems of that era ran well in 64 KB, so the kernel is not the
problem — the guest is.

---

## 3. The central constraint: the WiFi blobs

ESP32 WiFi is closed-source. For the C5 that is ~2.6 MB of objects
(`libnet80211.a`, `libpp.a`, and friends). It cannot be replaced, inspected or
slimmed. Any design that wants WiFi must host these blobs on their terms.

The saving grace: **Espressif already abstracted the OS dependency.** The
driver reaches the operating system through `wifi_osi_funcs_t`
(`esp_private/wifi_os_adapter.h`) — a struct of function pointers covering
semaphores, mutexes, queues, task creation and yield, ISR attachment and
interrupt masking. NuttX and Zephyr use this to run the blobs on non-FreeRTOS
kernels.

Similarly, lwIP's entire OS dependency is `sys_arch` — mailboxes, semaphores,
threads, a time source. The IDF tree ships both FreeRTOS and Linux ports of it.

**Consequence:** the set of primitives RV-9 must eventually provide is already
specified by someone else, and it is small. This is the single most important
fact in the project, and it shapes the architecture below.

---

## 4. Architecture

```
   ┌──────────────────────────────────────────────┐
   │  user modules — shell, utilities             │
   ├──────────────────────────────────────────────┤
   │  RV-9 personality                            │
   │    module manager   format, directory, load  │
   │    process manager  fork/chain/exit, sched   │
   │    I/O manager      paths, file mgrs, drivers│
   ├──────────────────────────────────────────────┤
   │  KAL — kernel abstraction layer  ◄── the seam│
   │    tasks sems mutexes queues timers alloc    │
   ├──────────────────────────────────────────────┤
   │  FreeRTOS  (phase 1)  →  RV-9 kernel (later) │
   ├──────────────────────────────────────────────┤
   │  ESP32-C5 hardware                           │
   └──────────────────────────────────────────────┘
```

### The KAL is the whole strategy

RV-9 is built on FreeRTOS first and native later. That evolution is only
possible if the personality layer never touches FreeRTOS directly.

**Rule: no source file above the KAL may include `freertos/*.h`.** This is
enforced by a build step that greps for it and fails the build. Every OS
project that intended to abstract the kernel "later" did not.

The KAL is deliberately shaped to be a superset of what `wifi_osi_funcs_t` and
lwIP `sys_arch` demand, so that implementing the native kernel in a later phase
is a matter of satisfying an interface that has already been proven correct by
a working system.

Provisional KAL surface:

```
  tasks      create destroy yield delay priority_set current
  sems       create destroy take give   (counting)
  mutexes    create destroy lock unlock (incl. recursive)
  queues     create destroy send recv send_from_isr
  timers     now_us oneshot periodic cancel
  memory     alloc free alloc_dma
  critical   enter exit / int_disable int_restore
  isr        attach detach
```

Backend selected at build time (`RV9_KERNEL=freertos|native`), not by function
pointer — direct calls, no indirection cost.

---

## 5. Memory modules

The idea worth preserving: **code is a data structure the system can find,
verify, share and load** — not a blob linked at build time.

Provisional header, little-endian:

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 4 | magic `"RV9M"` |
| 0x04 | 2 | header length |
| 0x06 | 4 | module length |
| 0x0A | 2 | name offset (into module) |
| 0x0C | 1 | type |
| 0x0D | 1 | attributes / revision |
| 0x0E | 2 | ABI version |
| 0x10 | 4 | entry offset |
| 0x14 | 4 | static data size |
| 0x18 | 4 | stack size hint |
| 0x1C | 4 | CRC-32 over the whole module |

OS-9 used CRC-24 and a header parity byte; CRC-32 is cheaper on this hardware
and the ESP32 ROM provides it.

Module types: `PROGRAM`, `LIBRARY`, `FILEMGR`, `DRIVER`, `DESCRIPTOR`, `DATA`,
`SYSTEM`.

### Module directory

An in-RAM index of every known module: name, type, revision, address, link
count. Modules are shared, not copied — two processes running the same program
share one text image. Link count governs unloading.

### Execute in place

Flash is memory-mapped, so modules living in a flash partition can execute
without being copied into RAM. Given ~246 KB of RAM this is not a nicety, it
is how the system survives.

### Relocation — decided in phase 1

**Modules are position-independent by construction, and are copied into
executable RAM to run.** No relocation table, no GOT, no ELF parsing.

This works because of four rules the build enforces:

- compiled `-mcmodel=medany`, so every symbol reference is PC-relative
- text and rodata link as one blob that moves as a unit
- no external symbols; everything arrives through the environment struct
- no `.data` and no `.bss` — `modules/module.ld` *asserts* both are empty and
  refuses to link otherwise

The last rule is the important one. A module with writable static data would
appear to work and then corrupt itself the moment two processes shared its
image. Catching that at link time rather than in the field is worth the
restriction. Per-instance state comes from `env->statics`, allocated and
zeroed by the loader — exactly OS-9's arrangement, where the 6809 passed the
data pointer in U and the 68000 in A6.

Verified on hardware: `objdump` shows string constants reached via `auipc`
(PC-relative), one `.text` section, zero undefined symbols.

**The build proves this rather than assuming it.** `tools/build_modules.sh`
links every module twice, at two different base addresses, and compares the
bytes. PC-relative code is byte-identical wherever it is linked; anything
holding an absolute address differs, and the build fails with an explanation.

This was added after a crash, not before one. GCC rewrote a `switch` over
string literals into a table of pointers in `.rodata` — absolute addresses,
tiny because the module links at base 0. The module loaded, ran, printed its
header, and took a load fault at `0x294` the moment it touched the table.
`-fno-jump-tables` does not prevent this; `-fno-tree-switch-conversion` does.

Compiler flags stop the constructs we know about. The dual-link check stops
the ones we do not.

Execute-in-place from flash remains attractive for RAM reasons and is
compatible with this design — the blob is relocatable, so mapping it rather
than copying it is an optimisation, not a redesign. Deferred until RAM
pressure justifies it.

---

## 6. Processes

- Preemptive, priority-scheduled, with **aging** so low-priority work cannot starve
- `fork` (new process from a module), `exit`, `wait`, and minimal signals
- Each process: module reference, static data area, stack, path table, priority,
  state, parent, exit status
- No memory protection in early phases; PMP-based isolation arrives with the
  native kernel

### Scheduling policy

The KAL provides priorities and nothing else. The policy is RV-9's own, which
is why phase 7 can replace the kernel without changing how the system behaves.

Effective priority = base + age, capped. An ager runs every 20 ms: whichever
runnable process currently ranks highest has its age reset (it is the one
getting CPU), and every other runnable process ages upward, to a maximum
boost of 10. The result is a sawtooth — a starved process climbs until it
outranks the hog, runs, and falls back.

Measured on hardware with two CPU-bound processes at priority 12 and 4:

| | low-priority progress at 600 ms | round duration |
| --- | --- | --- |
| aging off | 0 units | 2421 ms (sequential) |
| aging on | 59 units | 1359 ms (concurrent) |

### Priority hierarchy

Three bands, and the ordering is load-bearing:

| Band | Priority | Rule |
| --- | --- | --- |
| ager | `RV9_PRIO_AGER` (15) | must outrank everything it manages |
| system tasks | `RV9_PRIO_SYSTEM` (14) | must outrank user processes |
| user processes | ≤ 13 including aging boost | |

Both rules were learned by violating them. An ager that can be starved
rescues nobody. And an orchestrator below its children means a forked child
preempts the forker before it can create a sibling — which silently turned
the first scheduler test into two processes running one after the other
while appearing to run together.

---

## 7. I/O — the good part

An I/O model after the classic OS-9 structure, because it is better than what most
small systems do:

```
   process
      │  path number
      ▼
   I/O manager      generic: open close read write seek getstat setstat
      │
      ▼
   file manager     SCF (character)  RBF (block)  NFM (network)  PIPE
      │
      ▼
   driver           lcdcon  uart  sdspi  wifi
      │
      ▼
   descriptor       /term  /sd0  /n0    — names, binds, configures
```

Four independently loadable module types. A device descriptor is a small data
module naming a file manager, a driver, and initialisation parameters. Adding a
device is loading a descriptor — no kernel rebuild.

`getstat`/`setstat` carry the device-specific operations that don't fit
read/write, keeping the main path narrow. This is `ioctl` done deliberately
rather than by accident.

### The console is the USB cable

The C5 has a USB Serial/JTAG *device* peripheral and no host controller, so
a USB keyboard cannot be attached to this board at any price. The terminal
is whatever sits at the other end of the programming cable — which is also
the only reason the shell has a keyboard at all.

`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` matters here: the default routes the
console to UART0, whose pins go nowhere on this board.

### Planned devices

| Descriptor | File manager | Driver | Notes |
| --- | --- | --- | --- |
| `/term` | SCF | lcdcon | console on the ST7789 panel |
| `/uart0` | SCF | uart | serial console |
| `/r0` | RBF | ramdisk | 64 KB of memory; exists so RBF can be proven without a card |
| `/sd0` | RBF | sdspi | the microSD card; see §42 |
| `/n0` | NFM | wifi+lwIP | network as a path, not a socket API |

`/term` on the LCD is the first milestone that will actually feel like an
operating system.

### Block devices and RBF

Character drivers move bytes as they arrive; block drivers move whole
sectors at an address. A driver is one kind or the other, and
`rv9_driver_t` has separate entry points for each — `read`/`write` for
character devices, `geometry`/`read_blocks`/`write_blocks` for block ones.
Pretending one is the other is how storage stacks become unpleasant.

RBF takes OS-9's structure and none of its encodings:

| | |
| --- | --- |
| LSN 0 | identification sector: geometry, and where everything else is |
| LSN 1.. | allocation bitmap, one bit per sector |
| root | fixed-size directory entries: name, and a file descriptor sector |
| fd sector | one per file: size, and a list of segments |
| segment | a `(start sector, count)` run |

The segment list is the part worth keeping. A FAT-style chain makes you walk
the whole file to find its end; a segment list finds any offset in a handful
of comparisons, and stays at one entry for a file that was written
contiguously — which most are.

**A directory is a file.** Opening `/r0` rather than `/r0/notes.txt` reads
directory entries. That is not a special case in the I/O manager; it falls
out of the design, and it is why `dir` is thirty lines with no knowledge of
sectors, bitmaps or segments.

The RAM disk exists so this could be built and proven without depending on
an SD card driver, or on there being a card in the slot. When `sdspi`
arrives, its descriptor names the same file manager over a different driver
and nothing above changes. That is the claim the layering makes; the RAM
disk is the cheap way to test it.

One deliberate asymmetry: an unformatted RAM disk is formatted on sight,
because it is empty every boot and there is nothing to lose. A real card
must never be formatted without being asked.

### Redirection

A child inherits its parent's standard paths **by reference**, sharing one
path descriptor rather than reopening the device. That is what makes shell
redirection work without the child participating: the shell parks its own
stdout with `dup2`, points stdout at the target, forks — the child writes to
the target knowing nothing about it — then puts its stdout back.

`echo > /term` sends a module's output to the panel instead of the serial
line, and `echo` contains no code for either.

### What is loadable, and what is not yet

Device **descriptors** are real loadable modules: `rv9_io_attach_from_modules()`
scans the store for `RV9_MOD_DESCRIPTOR` modules and binds each one. Adding a
device is adding a module — no kernel rebuild.

File managers and drivers are *interfaces* but are still compiled in. The
module ABI cannot yet express what a driver needs — register access,
interrupts, DMA — and inventing that before a second driver exists would be
guessing. The interfaces in `rv9/io.h` are already shaped for it, so making
them loadable later changes nothing above them.

The `uart` driver reaches the console through the host kernel's stdout and
`lcdcon` drives the panel through ESP-IDF's `esp_lcd`. Both are shims at the
layer where shims belong: drivers are exactly where hardware knowledge is
allowed to live, and phase 7 replaces their insides without touching the
stack above.

---

## 8. Networking

WiFi association and lwIP run underneath a **network file manager**, so network
endpoints are opened, read and written like any other path. A raw socket API
may exist beneath it, but it is not the interface the system presents.

Ordering matters: this comes *after* the I/O manager is solid, because NFM is
the proof that the I/O abstraction was designed correctly rather than shaped
around one device.

---

## 9a. The native kernel, step 1 — done

The context switch (`components/rv9_kernel/src/switch.S`) saves the
callee-saved registers of the running thread, records its stack pointer,
loads another's, and returns — into the other thread. Caller-saved
registers are absent because the ABI already declares them dead across a
call, and this *is* a call: the calling convention does half the work.

A new thread's stack is built so the first switch into it "returns" into a
trampoline, with the entry point in `s0` and the argument in `s1`. Those are
callee-saved, so starting a thread and resuming one take exactly the same
path.

The scheduler implements the phase 2 policy directly instead of steering
another kernel's priorities. Measured on hardware, two CPU-bound threads at
priority 12 and 4, both stopping at one shared deadline:

| | RV-9 scheduler | FreeRTOS (phase 2) |
| --- | --- | --- |
| high priority | 5409 units | 2253 units |
| low priority | 601 units | 385 units |

Same shape: the urgent work dominates, the low-priority thread is never
starved. The policy survived being reimplemented, which is the point of
having written it down.

### What running as a guest cost

Two mechanisms had to be turned off, and both are things RV-9 wants for
itself later rather than things it is giving up:

**The hardware stack guard.** The C5 faults when the stack pointer leaves
the running task's registered range, and RV-9's threads run on stacks the
host has never registered — so the very first context switch tripped it.
While the kernel is a guest it cannot describe its stacks to a guard the
host owns. Step 3 programs that guard per thread; step 5 adds PMP.

**Host preemption during a switch.** `rv9_sched_lock()` holds the host
scheduler still while ours drives, because a host context switch taken
while the stack pointer is on a foreign stack saves a context it cannot
account for. One line on the native backend, and unnecessary once RV-9
owns the CPU.

Doing step 1 as a guest is deliberate: the context switch and scheduler
were proven while something known-good still held the machine up. The
alternative — bringing up a scheduler with no working system to compare
against — is how these projects stall.

## 9b. The native kernel, step 2 — the tick

The kernel's clock is its own tick and nothing else. Time is counted in
ticks, sleeps are measured in ticks, aging happens every twenty of them.
Nothing outside is consulted.

**The kernel has no code that runs in interrupt context.** `rv9k_tick_ref()`
hands out the counter's address and whoever owns the timer increments it
directly from its handler. That is not fastidiousness: on this hardware an
interrupt may arrive while the flash cache is disabled — the WiFi driver
writes NVS — and anything living in flash is unreachable while it is. A
handler that called into the kernel faulted with a cache error the moment
the radio was used. Keeping the kernel out of interrupt context is simpler
than annotating it to survive being there.

The counter is 32 bits so an increment is one store on a 32-bit machine and
a reader can never see half of one. It wraps after about 49 days at 1 kHz,
and every comparison is written to survive that (`RV9K_TICK_REACHED`).

Measured on hardware — two CPU-bound threads at priority 12 and 4, over
300 ms, with the kernel doing its own accounting:

| | |
| --- | --- |
| tick accuracy | 49 ticks in 50 ms |
| CPU charged | high 270 ticks, low 30 ticks |
| switches | 24 over 300 ms |

The switch count is the interesting one. Threads no longer yield on every
loop iteration; they offer a preemption point, and a switch happens only
when the clock says one is due. The scheduler's cadence comes from the
timer rather than from how often a thread happens to be polite.

### What step 2 deliberately did not do

Threads still choose *where* they can be preempted. True asynchronous
preemption — interrupting a thread mid-instruction-stream and resuming a
different one — requires the interrupt to return into another thread with a
full register frame, which means owning the trap vector. ESP-IDF owns
`mtvec` while RV-9 is a guest.

Fighting the host for it would be fragile and is unnecessary: step 3 takes
the CPU outright, and the trap vector comes with it. Doing preemption
properly there is less work than doing it improperly here.

## 9c. The contract, satisfied by both kernels

Phase 0 said the conformance suite would become the acceptance test for the
native kernel. A suite that calls one implementation directly cannot do
that, so it now takes the implementation as an argument: `kal_ops_t` is the
contract, and there are two of them.

```
[freertos]   23 passed, 0 failed     the reference
[rv9-kernel] 23 passed, 0 failed     RV-9's own kernel
```

Same test code, same assertions, two kernels. The native side runs inside
an RV-9 thread, because its blocking calls reschedule and rescheduling only
means something to a thread the kernel is running.

**The suite immediately found a bug in the reference.** `rv9_mutex_lock`
used FreeRTOS's ordinary take on recursive mutexes, which blocks them
against themselves — so a recursive lock deadlocked until it timed out.
That had been wrong since phase 0. The old suite checked only that a
recursive mutex could be *created*; it never locked one twice.

This is the argument for writing the acceptance test before the thing it
accepts. Building the second implementation is what forced the contract to
be written down precisely, and writing it down precisely is what exposed
the first implementation's mistake.

## 9d. The kernel's heap, and blocking that costs nothing

**Its own allocator.** A doubly-linked list of blocks in address order,
first fit, splitting on allocation and coalescing *both ways* on free.
Unglamorous and adequate: the kernel allocates thread stacks and small
objects, not a workload that needs size classes. Blocks are linked
physically rather than kept in a separate free list, so coalescing is O(1)
and needs no search; the cost is that allocation walks every block. For a
heap holding tens of objects that is the right trade, and the file says so
along with what to do when it stops being.

Coalescing both ways is the part that matters. One-sided coalescing looks
correct and slowly kills a heap under churn, so the test allocates sixteen
blocks, frees alternate ones, refills the gaps and frees everything —
49132 bytes of 49148 come back as a single block.

The only thing still borrowed is the *region* the heap sits in, which a
kernel owning the machine takes from its boot information.

**Blocking that costs nothing.** Wait queues thread through the threads'
own `next` pointers, so blocking allocates nothing — which matters in a
kernel that must still be able to block a thread when memory is exhausted.
A blocked thread leaves the run queue entirely.

Until now, blocking was a spin: mark ready, reschedule, look again.
Correct, and it burned every cycle nobody else wanted. Measured after the
change, with a consumer at *higher* priority than its producer so that
spinning would dominate:

```
ran 160 ms; consumer used 0 ticks, blocked 8 times
```

## 9e. RV-9 running on RV-9

`CONFIG_RV9_KERNEL_NATIVE` selects which kernel backs the KAL. With it on,
every RV-9 thread — the shell, every process, the I/O manager's internals —
is scheduled by RV-9's own scheduler. FreeRTOS is still underneath, holding
one task for the kernel to live in and running the ESP-IDF drivers; taking
the machine outright is what remains of step 3.

Nothing above the KAL changed to make this work. That is the seam paying
for six phases of discipline.

Three things broke, and each was informative.

**Thread slots leaked.** A slot was only reusable once its stack was NULL,
and nothing ever freed a dead thread's stack — so the system died after its
sixteenth thread. The guest tests never saw it because they reset the
kernel between runs. The kernel now reaps dead threads, and needed to be
given a *release* function as well as an allocator: a kernel that can only
allocate stacks runs out of threads.

**Two agers fought.** The process manager has aged priorities since phase 2
because FreeRTOS does not. RV-9's kernel does it directly, and the two
undid each other — setting a priority reset the age the kernel had just
applied. The KAL now answers `rv9_sched_ages()`, and the process manager
stands down when the kernel below it already carries the policy. The policy
did not change; which layer performs it did, which is exactly what writing
it down was for.

**A cooperative kernel needs somewhere to be preempted.** RV-9's kernel
switches only when asked, so a compute-bound module owned the machine.
Every system call is now a preemption point (`rv9_preempt_point()`, free on
a preemptive host), so a module interleaving work with any kernel service
is scheduled fairly without knowing this exists. A module that computes for
a long time touching nothing still cannot be interrupted — that needs the
trap vector.

### And one thing this arrangement taught about drivers

NFM blocked inside `accept()`. Under FreeRTOS that parks one task; under
RV-9's kernel, whose threads all share a single host task, it parked the
entire operating system — including the process about to connect to that
listener.

Every socket is non-blocking now, and waiting is done by sleeping through
the scheduler. That costs a little latency and is correct under both
kernels, which is the trade a driver should make: **a driver has no
business knowing how many host tasks its callers share.**

## 9f. A program that was never flashed

The module store stopped being something you reflash and became something
you add to.

```
rv9> downloaded
downloaded: no such module
rv9> fetch 192.168.1.12:8000 /downloaded.mod > /r0/downloaded.mod
--- 320 bytes of body
rv9> load /r0/downloaded.mod
loaded; 'mdir' lists it
rv9> downloaded
I was never flashed onto this board.
I arrived over WiFi, through a file, as pid 9.
```

Every layer earns its place in that sequence and none of them knew about
the others:

- `fetch` opened `/n0/192.168.1.12:8000/…` — the network as a path
- the shell redirected its stdout into `/r0/downloaded.mod`, a *file*,
  because redirection opens with CREATE and a volume is just another device
- `fetch` writes status to stderr and the body to stdout, so redirection
  captured exactly the file's bytes
- `load` read it back through the I/O manager, verified its CRC, copied it
  into executable memory and added it to the module directory
- the shell then ran it by name, like any other command

`rv9_mod_register_image()` marks such a module **resident**: there is no
store to re-read it from, so unlinking frees its links but keeps its image.

The module is deliberately excluded from the flash image — `.nostore` in
its directory — so that its running at all is proof it arrived some other
way.

## 9g. Persistence

A RAM disk proves a file manager works and is useless for anything real:
files and loaded programs die at reset. The board has spare flash, and
spare flash is a volume.

`/f0` is RBF over a 1 MB flash partition — the same file manager as `/r0`
over a different driver, which is the arrangement the layering promised.
Together with `autoload()` at boot, a program written there is a command
on every boot afterwards:

```
I (341) rv9-rbf: /f0 mounted: volume 'rv9', 2048 sectors
I (1118) rv9: /f0: loaded 1 of 1 module
rv9> downloaded
I was never flashed onto this board.
I arrived over WiFi, through a file, as pid 6.
```

Nothing was typed to make that happen and no cable was involved.

**Flash is not RAM, and the driver says so.** It erases in 4 KB blocks
while RBF writes 512-byte sectors, so a partial write means read the erase
block, patch it, erase, write it back. The driver batches by erase block so
a sequential run costs one cycle rather than eight. Blocks endure on the
order of 100k erases: right for configuration, programs and occasional
logs; wrong for something rewritten every second, and when that matters the
answer is an SD card behind the same interface, which RBF will not notice.

### What this cost, and what it taught

Networking broke. `connect()` began failing for every host, including ones
that had worked minutes earlier — and the cause was memory, not the
network: **11 KB free, 7 KB low water.** lwIP could not allocate a
connection.

The RAM disk was 64 KB and the kernel heap another 64 KB, both sized when
nothing else was competing. `/f0` provides a megabyte that costs no RAM at
all, so `/r0` dropped to 16 KB of scratch and the kernel heap to 32 KB —
which is what the kernel actually holds. Free memory went from 11 KB to
94 KB.

The lesson is not about sizes. It is that on a machine with 300 KB, every
buffer is taken from something else, and a subsystem that fails for want of
memory rarely says so — it reports that it cannot connect.

## 10. Real-time processes

A control loop cannot depend on every other thread in the system being
polite, and RV-9's own scheduler is cooperative. So a real-time process is
**not an RV-9 thread**. It runs on a preemptive scheduler above everything
else — above ordinary processes, above the task RV-9's kernel lives in,
above the drivers — and is released by a hardware microsecond timer rather
than by a software tick.

```c
env->rt_declare(1000);              /* 1 kHz */
for (;;) {
    read_sensors(); compute(); drive_actuators();
    int late = env->rt_wait();      /* 0 when on time */
    if (late) { ... }               /* fell behind; decide what that means */
}
```

The class is not about importance, it is about consequence: an ordinary
process that runs late is slow, a real-time process that runs late is
wrong.

### Measured, not claimed

"Real-time" is a property you measure. Every release records how late it
was and how long the work took, and a period that elapses while the loop
is still working is counted as an overrun rather than quietly absorbed —
a control loop that silently misses deadlines is worse than one that
stops, because it looks correct right up until something hits something.

`env->rt_stats()` hands those numbers to the application, because a loop
that cannot see its own jitter cannot report that it has stopped being
trustworthy.

Measured on hardware, with WiFi associated and the shell running:

| period | activations | worst jitter | worst execution | overruns |
| --- | --- | --- | --- | --- |
| 1000 µs (1 kHz) | 2000 | 27 µs | 34 µs | 0 |
| 200 µs (5 kHz) | 2000 | 7 µs | 8 µs | 0 |

### This shape survives step 3

When RV-9 owns the machine, a real-time process becomes a native
high-priority preemptible thread released by a hardware comparator —
the same arrangement, implemented by RV-9 instead of borrowed. The API
does not change, which matters because code written against it is control
code, and control code should not be rewritten because the kernel
underneath grew up.

### What it cost: two schedulers in one system

A real-time process is scheduled by the host while everything else is
scheduled by RV-9, and every place the two meet had to be found:

- **Locks.** A mutex blocks the caller in whichever scheduler the caller
  belongs to, which is useless for data both worlds touch. `rv9_lock_t` is
  a lock usable from any context, and the process table, path tables and
  driver state now use it. Hold it briefly and never block inside it: an
  RV-9 thread waiting on one stalls the cooperative kernel until it is
  released.
- **Waiting for a process to exit** was a semaphore signalled in one world
  and waited on in the other. It polls a word now.
- **`rv9_task_delete(NULL)`** ended the RV-9 thread, which for a host task
  is nothing at all — so the task function returned, which FreeRTOS
  correctly treats as fatal.
- **`rv9_task_self()`** returned RV-9's notion of the current thread, which
  is NULL for a host task. A real-time process therefore had no identity,
  no pid, no path table and no output: it ran perfectly and silently into
  the void. Handles from the two schedulers are distinct objects, so one
  call now answers for both.

Every one of those was the same mistake in a different place: *not every
caller of the KAL is an RV-9 thread.*

## 11. A shell over the network

```
$ nc 192.168.1.121 2300

RV-9 shell. Type 'help'.
rv9> rt control 1000
control: 2000 activations at 1000 us
  worst jitter   11 us
  overruns       0
```

`rshd` is about forty lines, and contains no terminal handling, no
protocol and no knowledge of sockets. It opens a path, points stdin and
stdout at it, forks the shell, and puts them back:

```c
int c = env->open("/n0/listen/2300", RV9_MODE_RW);   /* blocks */
env->dup2(RV9_STDIN, SAVE_IN);  env->dup2(RV9_STDOUT, SAVE_OUT);
env->dup2(c, RV9_STDIN);        env->dup2(c, RV9_STDOUT);
env->fork_arg("shell", 8, 0);   /* inherits them */
```

Every piece it needs was built for another reason: a connection is a path
(phase 6), a child inherits its parent's paths by reference (phase 3), and
the shell reads stdin without caring what it is (phase 4). Local and
remote shells run at the same time, as ordinary processes.

Two things had to change, and both were latent bugs rather than new work.

**SCF returned a line without its newline.** Reading a terminal, that is
harmless — SCF decides where a line ends. Reading a socket it is fatal,
because bytes arrive in whatever sizes the network chose and the reader
cannot tell a finished line from a partial one. The terminator is part of
what was read now, and the shell reads until it sees one, which works over
both.

**NFM gave up after fifteen seconds.** That made a listening daemon listen
only *most* of the time — connections arriving in the gap were refused —
and silently ended any session idle for a quarter of a minute. Accepting
and receiving wait indefinitely now, the way reading a terminal does.
Connecting still gives up, because an unreachable host should be reported
rather than waited on.

### This is not secure

Anyone who can reach the port gets a shell. It is for a workbench LAN.
SSH is the eventual answer and is real work — key exchange, a cipher, host
keys, channels — not something to bolt onto this. What this does provide
is the plumbing SSH would need, already proven: a shell whose standard
paths can be pointed at a connection.

## 12. Priority inversion

A real-time process waiting on a lock held by an ordinary one is delayed
by every medium-priority thread in the system — threads it shares nothing
with. That is priority inversion, and it is the classic way a real-time
system misses a deadline for reasons that look like nothing to do with
timing.

**Inheritance has to happen in both schedulers.** A FreeRTOS mutex already
lends the holder its priority, but what it lends to is the *task*, and
every RV-9 thread shares one. Boosting that task makes the kernel run; it
does not make the kernel run the thread holding the lock, because RV-9's
scheduler picks by its own priorities. So the host mutex gets the kernel
scheduled, and `rv9_lock_t` separately boosts the holding RV-9 thread to
get the right thread scheduled inside it. Neither half suffices alone.

A boost is a floor rather than an assignment: aging may lift the thread
further, and releasing the lock returns it to whatever it had earned.

Measured with the classic three actors — a low-priority holder, a
medium-priority hog that wants nothing, and a real-time waiter:

| | urgent waited |
| --- | --- |
| without inheritance | 504 ms |
| with inheritance | 397 ms |

397 ms is very close to the 400 ms the holder actually needed the lock
for, so the inversion is essentially gone.

### Two bugs this found in the locks themselves

**An RV-9 thread must not block on a host lock.** Every RV-9 thread shares
one host task, so blocking it stops the whole kernel — including the
thread holding the lock, which can then never release it. The waiter
deadlocks against its own scheduler. An RV-9 thread now waits by sleeping
through its own scheduler; a host task, having a task of its own to block,
still blocks.

**And it must sleep, not yield.** Yielding leaves the waiter runnable, and
a waiter that outranks the holder is simply picked again — it spins at full
priority while the thread it is waiting for never runs. Sleeping takes it
off the run queue so the (boosted) holder can get the CPU. The cost is up
to one tick of latency on a contended lock; real-time waiters do not pay
it, being host tasks.

### Inheritance elevates the whole kernel — mind your hold times

This is the important consequence, and it is not obvious.

When a real-time process blocks on a lock held by an RV-9 thread, FreeRTOS
lends *its* priority to the task the lock's holder runs in — which is the
task RV-9's entire kernel runs in. For as long as the holder holds the
lock, the cooperative kernel runs **at real-time priority**, scheduling
whichever thread it pleases. Everything else on the machine, WiFi and the
console included, waits.

That is inheritance working correctly. It is also a design constraint with
teeth:

> **A lock shared with a real-time process bounds how long the whole system
> may run at real-time priority. That bound is the holder's hold time.**

The first version of the demonstration held a lock for 400 ms and hogged
the CPU for 600. The result was half a second of total priority monopoly
on every boot, arriving exactly while the radio was associating — which
produced stalls and panics that looked like a lock bug and were nothing of
the kind. The lock code was correct throughout; the *test* was pathological.

A control loop does not monopolise a CPU for half a second, and neither
should anything holding a lock one might want. Keep critical sections
shared with real-time work to the shortest thing that is correct.

The demonstration remains in the source, disabled
(`RV9_RUN_INVERSION_DEMO`), and the measurement it produced — 504 ms
without inheritance, 397 ms with — stands. Re-enabling it at these shorter
durations still did not produce output reliably and was not pursued
further: the finding above is worth more than the harness.

## 13. Hardware as devices

Pins, PWM outputs and analogue inputs are devices under the same I/O
manager as everything else:

```
/gpio/8        pin 8
/pwm0/3        PWM on pin 3
/adc0/1        analogue channel 1
```

**PIO is a third discipline**, after SCF's character streams and RBF's
blocks. A pin is not a stream of bytes and not an array of sectors; it is
an addressable unit carrying a value. Drivers of this shape implement
`unit_open` / `unit_read` / `unit_write` / `unit_stat`, and a read or write
moves one 32-bit value rather than text.

That is for the fast path: a control loop writing a duty cycle every
millisecond should not format decimal first. The `pin`, `pwm` and `adc`
utilities convert for humans at the shell, where a microsecond does not
matter. Configuration — direction, pull, frequency — goes through
getstat/setstat, which is what those calls are for.

Measured working: GPIO drives and reads back, and `/adc0` returns real
conversions (762, 729, 724, 721 of 4095 on a floating input).

### Two drivers, two opposite closing behaviours

Deliberately, and worth stating because the asymmetry looks like an
oversight:

- **`/gpio` holds its level when the path closes.** An enable line that
  dropped because the program which raised it exited would be worse than
  useless.
- **`/pwm0` stops driving when the path closes.** It holds a hardware
  channel that must be given back, and an actuator still running because a
  program finished is a bad way to learn that it finished.

### A driver that can disconnect the operator

GPIO 13 and 14 carry the USB Serial/JTAG lines: the console, the flashing
channel, and the only way to talk to this board. The reserved list covered
the display and the card slot and missed them, so a routine `pin 13` during
a pin survey took the machine away and needed the cable physically pulled.

They are reserved now, with no override. When there is one it should be
harder to reach than a mistyped pin number — the principle being that **a
driver able to disconnect the operator should refuse to unless asked very
deliberately.**

### The backlight is a PWM output

```
rv9> backlight
backlight 100%
rv9> backlight 20
backlight 20%
```

It was switched on at boot and left there — the largest continuous draw on
this board, and a real contributor to how warm it runs. It is a PWM output
like any other, on its own timer and a channel above the ones `/pwm0`
hands out, so the two cannot fight over hardware.

Brightness is a `setstat` on `/term` rather than a descriptor option,
because the useful time to turn a display down is while the machine is
running, not at boot. It is deliberately **not** remembered across a reset:
a machine that boots with a dark display is unnecessarily hard to diagnose.

The console keeps working at any brightness, including zero.

### The die temperature is a device too

```
rv9> temp 6
46.20 C   46.20 C   45.20 C   45.20 C
range 45 to 46 C over 6 s
```

"The chip feels warm" is not a measurement and a robot cannot feel
anything. `/tsens/0` reads hundredths of a degree; 45-46 C with the radio
associated, the CPU at 240 MHz and the LCD backlight on is ordinary for
this part, and a flat trend is the thing that says nothing is running away.

For a system that will end up in an enclosure with its radio on, this is
the input a thermal-throttling decision needs, and it costs one driver.

### And one bug that was not the driver's

Every pin reported a mismatch: written 1, reads 0. The driver was correct
throughout — instrumenting it showed the pad reading back 1 immediately.
The fault was in the `pin` utility, which opened the path *write-only* and
then read it. The I/O manager refused, exactly as it should, and the
utility ignored the error and printed a variable that had never been
written to.

Ignoring a return value turned a correct refusal into what looked like
broken hardware for several rounds of debugging.

It then happened again within the hour. `MAX_DRIVERS` was 8, sized to what
existed when it was written; `tsens` was the ninth, its registration failed
with "no room", and the caller discarded the error. The symptom appeared
three layers away as a descriptor that could not find its driver.

Both registries now say so loudly and have headroom, and every
registration in `io_bringup` is checked. **A registration that fails is a
device that will not exist: complain where it happens, not where it is
missed.**

## 14. Bounded-latency I/O

A control loop and the shell used to take the same lock to do the same
thing. `rv9_io_read`/`write` looked up the caller's path by asking the
process manager for the current pid — taking the process lock and scanning
the process list — and then walked the path-table list with no lock at all.
Slow where it mattered, and a race everywhere else.

Now each task finds its path table once and keeps it in task-local storage.
The table is filled at `open`, where blocking is allowed, so by the time a
deadline exists a write is a load and an index. The cached pointer is safe
to keep: a table belongs to a process, is created before that process can
run and freed after it has stopped, so no task can watch its own table
disappear.

Open and close are deliberately *not* bounded. They allocate, and closing a
file on a volume writes flash. **A control loop opens what it needs before
its first period and then only reads and writes** — which is the discipline
this design asks for rather than pretends to remove.

### What the measurement actually found

`rt iolat` runs 2000 activations at 1 kHz writing `/gpio/3` and reports the
worst single write, not the average. The first version said:

```
  worst write    26 us
  worst jitter   198902 us
  overruns       7
```

199 milliseconds, in one piece, in a 1 kHz loop. It happened on the first
run after every boot and never again on that boot, and it did not happen at
all with the radio off. That is the WiFi stack storing its calibration data:
a write to flash.

**On this chip, flash is memory-mapped through a cache, and every write to
flash switches that cache off.** Code sitting in flash is not merely slow
then — it is not there. RV-9's timer ISR was already in IRAM for exactly
this reason; the *task* it releases was not, so it woke into nothing.

So `RV9_RT_CODE` (rv9/kal.h) marks the code a control loop runs while a
deadline is pending, and the whole path carries it: the module's env
thunks, `rv9_io_read`/`write` and the path lookup, PIO, the GPIO driver,
`rv9_rt_wait`, `rv9_time_us`. ESP-IDF's `gpio_set_level` and `gpio_get_level`
are already mapped `noflash`, so a pin is reachable end to end. PWM and the
ADC are not, and are not claimed to be: their drivers take mutexes and live
in flash.

This is not an optimisation. A loop whose code can vanish for a fifth of a
second is not a real-time loop, however good its average looks. When RV-9
owns the machine there is no cache to lose and the attribute becomes
nothing — which is why it names the property and not the mechanism.

Same board, same radio, first run after boot:

```
iolat: 2000 writes to /gpio/3 at 1000 us
  worst write    13 us (activation 1932)
  mean write     2 us
  worst wakeup   9 us late
  periods missed 0
```

20,000 activations across ten runs with the shell in use: worst write 10-15
µs, worst wakeup 8-12 µs late, nothing missed.

### The part that is still not bounded, stated plainly

`noise` exists to make the system busy in the two ways that hurt: writing
and closing a file on `/f0`, and forking. Run from the network shell while
`iolat` runs on the console — two shells, one machine, which is what `rshd`
is for — it gives:

```
iolat: 2000 writes to /gpio/3 at 1000 us
  worst write    13 us (activation 441)
  mean write     2 us
  worst wakeup   82588 us late
  periods missed 1879
```

**The write stayed at 13 µs. Being scheduled at all did not.** 82
milliseconds is one flash erase. Residency keeps the loop's code reachable;
it does not give the loop a CPU while the flash driver holds the bus with
interrupts off.

So the honest statement of what RV-9 offers today is: *I/O from a real-time
process is bounded; concurrent flash writes are not, and the two do not
belong in the same second.* A control loop and a log that writes to `/f0`
are in conflict on this hardware, and saying so is more useful than an
average that hides it.

The known lever is the flash chip's suspend/resume feature
(`SPI_FLASH_AUTO_SUSPEND`), which lets an erase be interrupted. It is
deliberately **not** enabled: it works only on specific flash parts,
Espressif tells new applications not to turn it on, and this board reports
`detected chip: generic` — ESP-IDF does not recognise the part well enough
to have a driver for it. Enabling it here would trade a measurable stall
for an unmeasurable risk of corrupting the module store.

### An instrument that saturated where it mattered

The old report said seven overruns for a 199 ms stall, twice. `rv9_rt_wait`
counted missed periods by draining the release semaphore, which holds
eight — and then reported jitter as the *remainder* after whole periods
were subtracted, which can never exceed one period. Two different ways of
saying "late by a little" about a loop that had stopped.

**A measurement that saturates exactly where the trouble is is worse than
no measurement.** Lateness is now the whole overshoot, measured against the
clock; missed periods fall out of it.

### The bug underneath: whose thread am I?

Once in roughly eight runs the machine died in the kernel's host task with
a return address made of text. The cause was `rv9k_self()`.

It returns the kernel's current thread, and that stays set for as long as
that thread is running. A real-time process is a *host* task that preempts
the kernel's host task mid-thread — so asking `rv9k_self()` from there
answers with whatever RV-9 thread it interrupted. The real-time process
was told it was the shell.

That is not a cosmetic confusion. It hands over another process's identity:
its pid, its path table, its place in the lock's priority inheritance. A
real-time process that cached *its* path table into the shell's thread and
then exited left the shell reading freed memory — which surfaced, much
later and three layers away, as the kernel jumping into the middle of a
string.

Only the KAL can answer the question, because only the KAL knows which host
task the kernel runs in. `rv9_kal_self_thread()` compares the two and
returns NULL for everyone else; the kernel stays ignorant of hosts, which
is the arrangement worth keeping. Every "am I an RV-9 thread?" in the KAL
now goes through it — including the lock, which had been boosting and
un-boosting an innocent thread.

Third one of this family, and the pattern is stated in §10 already: **not
every caller of the KAL is an RV-9 thread.** The first two failed loudly
and immediately. This one ran perfectly for weeks.

## 15. Reacting: real-time processes released by interrupts

A periodic process asks "what time is it?". A reactive one asks "what just
happened?", and the second is not the first sampling fast enough — a
1 kHz loop watching for an edge finds it up to a millisecond late, burns a
core doing it, and still cannot tell you when the edge actually was.

So a real-time process can now be released by an **event** instead of a
period:

```c
uint32_t both = 3;                                  /* rising and falling */
env->setstat(pin, RV9_PIO_SS_EDGE, &both);          /* arm the pin        */

uint32_t ev = 0;
env->getstat(pin, RV9_PIO_GS_EVENT, &ev);           /* which event is it? */
env->rt_declare_event((int)ev, 200);                /* no faster than 5 kHz */

for (;;) {
    int coalesced = env->rt_wait();                 /* returns when it happens */
    ...
}
```

`rt_wait()` is **the same call** as for a periodic process. That is the
design decision worth defending: a control loop should be able to change
what wakes it without being rewritten, because "released on time" is one
idea whether the release comes from a timer we own or from a world we do
not. Only the clock differs.

### Events, and why they are numbers

An event is a rendezvous between an interrupt and a thread, and it is named
by a small integer. This is OS-9's `F$Event` — deliberately, and not only
for the heritage: a numbered event is something a driver can hand out
through `getstat` without either end holding a pointer into the other.

`rv9_proc` therefore has no dependency on `rv9_io`. The process manager
takes a number and hands it to the KAL; the module does the introduction,
asking the *device* which event it signals on. Any device that can
interrupt answers the same two codes — `SS_EDGE` to arm, `GS_EVENT` to ask
— so a UART with a character waiting, or a card finishing a transfer, will
plug into this without a new mechanism. That is why the codes live in the
generic PIO settings and not in the gpio driver.

### What is measured, and against what

The interrupt handler stamps the microsecond clock **before** it wakes
anyone. The process compares that stamp with the clock when it actually
resumed. So `max_jitter_us` becomes *pin to process*: the whole cost of an
interrupt, a semaphore, a context switch and a preemption.

Two details that decide whether the number is honest:

- When several edges arrive before the process runs, the stamp kept is the
  **oldest** unserviced one, not the newest. Stamping the newest would
  quietly subtract the part of the delay that was our fault, which is the
  direction an instrument must never round in.
- Coalesced events are **counted**, not discarded silently. They are the
  aperiodic form of a missed period: more happened than was responded to.

`min_interval_us` is the sporadic task's equivalent of a period — the
shortest gap the caller promises to cope with, and what makes the load
analysable at all. A source that beats it is not throttled; it is counted,
and warned about once. A control system whose inputs are arriving faster
than its designer expected needs to be told, not silently rate-limited.

### Measured

ESP32-C5, WiFi associated, both edges of `/gpio/3`, worst case over the run:

| what is driving the pin                        | edges | worst edge → process |
|------------------------------------------------|-------|----------------------|
| an ordinary process, idle machine              | 400   | 7 µs                 |
| the same, first run after boot (radio writing NVS) | 400 | 15 µs               |
| the same, while `noise` writes flash and forks | 2000  | 13 µs                |
| a *second* real-time process at 1 kHz          | 1500  | 41–51 µs             |

No events were coalesced and none were lost in any run. The last row is the
most interesting: both processes are host tasks at the same priority, so the
waiter cannot preempt the generator and waits for it to block. Two real-time
processes at one priority level is a scheduling policy this system does not
have yet — see below.

### What this does *not* yet measure, and why

An edge that arrives **during a flash erase**. Every event source available
on this board without external wiring is software, and software that lives
in flash is stopped by the same window that would delay the response — so no
edge is produced during the stall, and none is waiting at the far end of it.
The measurements above show the interrupt path is intact and nothing is
dropped; they do not put a number on that window.

Closing it needs a source that is genuinely independent of the CPU: an
external signal generator, or a peripheral (LEDC, a timer's hardware output)
routed to an armed pin. Worth doing before anyone builds a controller that
trusts the number. The periodic side has the same hazard measured at 82 ms
(§14), and there is no reason to assume the reactive side is better until it
is measured.

### Two things this changed underneath

**A pin may now be held by more than one process.** Opening a `/gpio` unit
used to call `gpio_reset_pin` every time, which took the previous opener's
configuration with it — direction, pull, and, once there were interrupts,
the arming a process was blocked waiting on. The pin stayed open and simply
stopped doing what it had been told. The reset now happens on the first open
only, and direction is the union of what the openers asked for: if anyone
wants to drive it, it is an output, because an output on this chip still
reads back.

**`gpio_set_level` was not actually resident.** §14 claimed the GPIO write
path was reachable with the flash cache off, on the strength of ESP-IDF
mapping `gpio_set_level` and `gpio_get_level` `noflash`. It does — *if*
`CONFIG_GPIO_CTRL_FUNC_IN_IRAM` is set, and it is off by default. Marking
RV-9's own code `RV9_RT_CODE` and then calling into flash on the last
instruction is a thorough way to achieve nothing. The option is now in
`sdkconfig.defaults` and the claim is true; `nm` on the image is the check,
and it belongs in a test rather than in a habit.

### What is still missing

- **A scheduling policy among real-time processes.** They all sit at one
  host priority, so two of them round-robin. Rate-monotonic or EDF is the
  obvious next thing, and it needs the periods and inter-arrival bounds that
  are already being declared and recorded.
- **Non-real-time processes waiting on events.** An ordinary process should
  be able to block in `read()` on an armed pin — the classic shape, where a
  blocking read is how you wait for a device. The event object is already
  the right rendezvous for it; what is missing is the cooperative kernel's
  side of the wait.
- **`floods` is not in the module ABI.** It is recorded and logged, but
  `rv9_rt_report_t` did not grow to carry it, on purpose: the module
  supplies that buffer and the kernel fills it, so appending a field would
  write past the end of an older module's stack. Fields the kernel writes
  into module memory are frozen once published — which is the opposite of
  the rule for `rv9_mod_env_t`, where the module only reads what it knows
  about and appending is free.

## 16. The console: addressing a screen you have never seen

A program that wants to draw rather than scroll needs three things —
put the cursor here, use these colours, clear that much — and the usual way
to get them is to write escape sequences down the pipe and hope. That works
until the far end is not a terminal. `/term` is a 320×172 panel with a font
renderer and a framebuffer; there is no parser in it, and writing one so it
can decode instructions we ourselves just encoded would be a strange way to
spend a kilobyte.

So cursor, colour and attributes are **setstat codes**, not bytes in the
stream:

```c
m_cursor(env, p, 3, 10);                       /* row 3, column 10 */
m_colour(env, p, RV9_COL_WHITE | RV9_COL_BRIGHT, RV9_COL_BLUE);
m_attr(env, p, RV9_CON_ATTR_BOLD);
m_say(env, p, "drawn by address");
```

which is `RV9_CON_SS_CURSOR`, `RV9_CON_SS_COLOUR` and `RV9_CON_SS_ATTR`
underneath, plus `SS_CLEAR` and `SS_CURSOR_ON`, and `GS_SIZE` to ask how
much screen there is.

### Who answers, and who is handed a sequence

The driver is asked first, always. If it declines — `UNSUPPORTED` — SCF
assumes a terminal is out there and writes the ECMA-48 sequence that means
the same thing.

```
   module ──setstat──▶ SCF ──▶ driver has it?  ──yes──▶  driver does it
                               (lcdcon: moves a render position,
                                paints cells)
                                    │ no
                                    ▼
                               rv9_con_ansi() ──▶ driver->write()
                               (usbserial, and anything else
                                that is really a wire)
```

The panel never sees an escape sequence, and the UART never needs to know
what a cursor is. Both ends of that fork are cheap, and the module that
called `m_cursor` cannot tell which one it got. `screen` and `screen /term`
are the same binary drawing the same picture on a 30×8 panel and an 80×24
terminal.

NFM is the third case and it takes the short path: a network connection is
a wire by definition, so it translates without offering the driver a say. A
network card has no cursor.

### Sixteen colours, not sixteen million

`RV9_COL_BLACK`…`RV9_COL_WHITE`, or `RV9_COL_BRIGHT` in for the other
eight, or `RV9_COL_DEFAULT` for whatever the device came up as. The panel
is RGB565 and could take a triple; a terminal cannot, and a palette that
only half the devices can honour is not an abstraction. Sixteen is what
both ends actually have, and it maps to SGR 30–37/90–97 in one line.

`RV9_COL_DEFAULT` is the piece that makes this usable: a program that sets
a foreground and wants the background left alone says so, and the panel
substitutes the colour from its descriptor while a terminal sends SGR 39.
Neither has to be told what the user's background was.

### Attributes are a set, not a stream

`SS_ATTR` takes the whole set each time, and turning things off is done by
sending a set without them. This is deliberate and the translator is where
it shows: the obvious encoding of "no attributes" is `ESC[0m`, and `ESC[0m`
also throws away the colour. So `rv9_con_ansi` emits the explicit
cancels — 22, 24, 27 — and never a bare reset. A program that switches bold
off and finds its colour gone is a bug that would have been reported as
"the panel and the terminal disagree", which is the exact failure this
whole section exists to prevent.

The translator refuses to truncate, too: if the sequence will not fit in
the buffer it returns 0 and nothing is written, because a half-written
escape sequence eats the bytes that follow it. Better nothing than a
fragment.

### The panel side

`drv_lcdcon` gained a parallel attribute plane — one `{fg, bg, flags}` per
cell — and the renderer walks it columns-outer so the 16-step
foreground-over-background ramp is rebuilt once per run of equal colour
rather than once per scanline. A cell under the cursor is drawn with its
reverse bit flipped, which costs nothing and needs no separate cursor
logic. Underline is full coverage on the glyph's last row.

Fixing this turned up a latent bug worth recording: `fg` and `bg` had been
stored already byte-swapped for the panel, which was invisible while there
was one colour for the whole screen and would have made every blend in the
anti-aliasing ramp operate on reversed channels the moment per-cell colour
arrived. Colours are now stored in native order and swapped at paint time.

### Size, and the part that is a guess

`GS_SIZE` returns `rows << 16 | cols`. `/term` computes it at init from the
panel geometry and its descriptor — scale, rotation, margins — so a
different descriptor or a different panel reports a different size and
nobody has to be told. On this board it is 30×8.

Over a wire it is `RV9_CON_DEFAULT_ROWS` × `RV9_CON_DEFAULT_COLS`, 24×80,
and that is a **convention rather than a measurement**. A serial line
carries no dimensions; an xterm can be resized without a byte reaching the
board. The honest thing is to document it as a guess rather than to dress
it up.

Two things are missing and both belong to a session layer, not a device:

- **No way to be told.** SSH has a window-change message and telnet has
  NAWS. Whichever arrives first is the right place for an
  `RV9_CON_SS_SIZE` setstat, letting the session push down what it
  negotiated. It was left out on purpose — a setter nobody can call
  honestly is worse than none.
- **No resize notification.** A program that asks once and caches the
  answer is wrong the moment the window changes. `RV9_SIG_WINCH` is the
  obvious shape, and `signals_take()` is already the polling point a screen
  program checks each loop.

Until then: query at startup, never cache across a redraw. That costs one
getstat per repaint and is correct in advance rather than retrofitted.

## 17. SSH, as a character device

`rshd` handed a shell to anyone who could reach port 2300. This replaces it
with one that asks who you are, and the interesting part is not the
cryptography — it is that the cryptography fits underneath machinery that
was written for a serial port.

```
   sshd  ──open("/ssh0")──▶  SCF  ──▶  ssh driver  ──▶  path "/n0/listen/22"
    │                         │            │
    │                    line discipline   │  key exchange, host key,
    │                    echo, rubout,     │  password, channel
    │                    ^C, CR LF,        │
    │                    cursor & colour   │
    ▼
  fork("shell")
```

`sshd` is `rshd` with one string changed. That was the test of where to put
the protocol: if serving an encrypted shell had needed more than a
different device name, the layering would have been wrong.

### Why a driver and not a daemon

The obvious design is a program that speaks SSH and pipes bytes to a shell.
It was rejected for two reasons, and the second is the real one.

A module cannot reach mbedTLS — modules are freestanding blobs with no
external symbols, which is what makes them relocatable. So the protocol has
to live in the firmware regardless.

And **an SSH session is a character device**. It is a stream of bytes with a
terminal at the far end. Making it one means SCF's line discipline applies
to it, and that is not a nicety: a client that has been given a pty puts
its own terminal in raw mode and echoes nothing. Every keystroke arrives
here, and the far end displays only what is sent back. A session without a
line discipline is a session where typing appears to do nothing. SCF was
written in phase 3 for a UART, and it is exactly what this needs —
including the console setstats from §16, which reach an ssh client as
escape sequences because the driver declines them.

### The transport is a path

The driver opens `/n0/listen/22` and runs SSH over it. There is no socket
in the SSH code and no lwIP — `read`, `write`, `close`. SSH would run over
anything the I/O system can open, and the day RV-9 talks to another RV-9
over a serial line, this works there without an edit.

### One suite

| | | |
|---|---|---|
| key exchange | `curve25519-sha256` | X25519 |
| host key | `ecdsa-sha2-nistp256` | P-256, generated on the board |
| cipher | `aes256-gcm@openssh.com` | AEAD, so no separate MAC |

All three are in OpenSSH's defaults, so a stock client connects without
being told anything, and all three are accelerated in hardware here.

Two curves rather than one is not taste: mbedTLS 4 has no Ed25519 in this
build, so X25519 does the agreement and P-256 does the signing. mbedTLS 4
also removed the legacy `mbedtls_ecdh`/`mbedtls_aes` surface in favour of
PSA, which turned out to help — PSA hands X25519 keys back as thirty-two
raw bytes, which is precisely the SSH wire format, so the key exchange
needs no format conversion at all.

Absent on purpose: rekeying, compression, more than one channel, more than
one session at a time, and any cipher that is not an AEAD. A rekey request
is answered with a disconnect rather than ignored.

Absent for a duller reason: **post-quantum key exchange**. OpenSSH 10 warns
that `curve25519-sha256` leaves a session open to being recorded now and
decrypted once a quantum computer exists. It is right, and the fix is
`mlkem768x25519-sha256`, which needs an ML-KEM implementation — and there
is none in this mbedTLS, nor a Keccak to build one on. That is a project
rather than a configuration change, and worth doing before anything here
carries traffic that matters in twenty years.

### Keys

`ssh -i ~/.ssh/rv9 anything@<ip>` and no password. The trusted keys live in
`/f0/authkeys`, in the format of an ordinary `authorized_keys` file, so the
line from your own `~/.ssh` is the line that goes here. `authkey` appends
one, `authkey list` shows them, `authkey clear` revokes the lot.

Two things must both hold, and they are independent:

- the key is **authorized** — its blob appears in the file, compared as
  bytes, with no parser in the path to disagree about what a key means;
- the client **holds the private half** — it signs, and the signature
  verifies against the key it offered.

Either check alone lets anybody in. A public key is public, so trusting one
that arrives unsigned trusts whoever copied it; and a signature that
verifies against a key nobody authorized is a stranger with good
cryptography. This is the single most common way to write an SSH server
that appears to work and does not.

What gets signed includes the session id — the first exchange hash — so a
signature captured from one session cannot be replayed into another.

The file is read per attempt rather than cached, so revoking is deleting a
line, effective at the next login with nothing to restart.

**Ed25519 does not work**, and that is worth saying plainly because
`ssh-keygen` has defaulted to it for years. mbedTLS as ESP-IDF ships it has
no EdDSA at all — not disabled, absent. What verifies here is
`ecdsa-sha2-nistp256` (in hardware, `MBEDTLS_HARDWARE_ECDSA_VERIFY`) and
`rsa-sha2-256`/`512`.

RSA needed one extra thing. PSA imports an RSA public key as a DER
`RSAPublicKey`, and SSH supplies two mpints — but an SSH mpint and a DER
INTEGER have identical rules, so the bytes carry across untouched and only
the SEQUENCE wrapping has to be built.

It also needed `EXT_INFO`. Without a server advertising `server-sig-algs`,
an OpenSSH client assumes a server that has not said otherwise can only do
`ssh-rsa` — SHA-1, disabled in 8.8 — and will not offer an RSA key at all.
The key would be in the file, the client would hold its private half, and
authentication would fail with neither end saying anything useful.

An offer without a signature is a **question, not an attempt**: the client
asks whether a key is worth using and is answered with `PK_OK`, which
grants nothing. Those do not count against the retry limit, because a
client with several keys asks about each in turn and counting them would
lock out anyone whose agent holds a handful.

Password login stays enabled alongside. Locking yourself out of a board
whose only console is thirty columns by eight is a bad evening.

### The host key, and the password

The host key is generated on the board the first time it boots this
firmware and kept in NVS. A key that changed every boot would make the
client's warning about a changed key meaningless, which is the same as not
having the warning.

The password is one password for the whole board, salted and stretched
through ten thousand rounds of SHA-256 before it is stored. There is no
user database because there are no users: RV-9 has processes and no notion
of who owns one. Any name logs in; the password decides. Pretending
otherwise — accepting a name and ignoring it silently — would be worse than
saying so.

Public key authentication is the better answer and is not here yet. It
wants somewhere to keep an `authorized_keys`, which wants the storage
phase.

### What this fixed in §16

§16 said 80×24 was a stated guess for anything over a wire, because nothing
in a byte stream carries a terminal's size, and that a session protocol was
the right place to fix it. It is fixed: `pty-req` carries the client's real
dimensions and `window-change` carries them again whenever somebody drags
the corner of their window, so `getstat(RV9_CON_GS_SIZE)` on an ssh path
answers with the truth. `screen` over ssh lays itself out to the actual
window.

The guess remains for a session with no pty, which is the honest answer
there.

What is still missing is *notification*. A program that asks once and holds
the answer is wrong after a resize; the driver knows, and has no way to say
so. `RV9_SIG_WINCH` is the shape, and `signals_take()` is already the
polling point a screen program checks each loop. An `RV9_CON_SS_SIZE`
setstat is no longer pointless either, now that something exists which
could honestly call it.

### Three things this changed underneath

**A driver can be told when its device starts being used.** `init` runs at
attach, which is right for a UART and impossible for a session: being open
means somebody has connected, authenticated and asked for a shell, none of
which can happen at boot with nobody there. So `rv9_driver_t` gained
`open` and `close`, called when the device goes from nobody-using-it to
somebody and back.

That immediately produced a bug worth recording. `open_count` is raised
before the blocking open runs, so a *second* `sshd` found a non-zero count,
concluded the device was already up, skipped the handshake it thought had
happened, and handed its shell a device with no session behind it. The
shell read, failed, exited, and the daemon did it again as fast as it
could. A device whose driver does work on first open is a session, and
sessions are not shared by independent openers — a second open is refused.
Sharing by `dup2` and `fork` is untouched, because that raises a reference
count and never goes through `open`.

**A driver can hold a path of its own.** Every other path is in a process's
table, which is right for a process and wrong for a driver: only
stdin/stdout/stderr are inherited across `fork`, so the connection `sshd`
opened would have been a meaningless number in the shell doing the reading.
A *detached* path is held by pointer and lives until its holder closes it.
It is the mechanism any stacking driver needs.

**SCF was swallowing driver errors.** Its read loop treated "no bytes" as
"nothing typed yet" and slept, before testing the error — so a driver
reporting a failure *and* zero bytes, which is the normal way to say a
connection has gone, left the reader waiting at a prompt nobody would ever
type at. Nothing before this could fail a read, so the bug had never had a
chance to matter.

### And one that cost an afternoon

The first working handshake failed at the last step with `incorrect
signature`. Everything either side of it was right — the client parsed the
host key well enough to print a fingerprint from it.

SSH's ECDSA signature is computed over the exchange hash **as a message,
not as a digest**: H is hashed again with SHA-256 before ECDSA sees it.
Signing H directly produces a signature of exactly the right shape that
every client rejects. RFC 5656 says so plainly; it just does not read like
it means what it says.

## 18. Raw input, and an editor

`ed /f0/notes`. Arrows, home/end, page up and down, `^S` to save, `^K` to
cut, `^X` to leave. Three thousand bytes of module.

The same binary edits in a 236-column SSH window and on the panel at
thirty by eight, because it asks the path it was handed how big it is and
never assumes. It asks again on every redraw, which costs one getstat and
means dragging the corner of a terminal reflows it at the next keystroke.

### The thing that was actually missing

Two pieces were already in place — §16 gave cursor and colour that work
the same on a framebuffer and a terminal, §17 gave a session that knows
its real size. The missing one was not obvious until an editor needed it.

**SCF reads a line.** It buffers until return, echoes as you type, handles
rubout, and discards anything that is not printable. That is exactly right
for a shell and exactly wrong for anything that draws: an arrow key is
`ESC [ A`, and the escape was being thrown away before any program could
see it. An editor built on top of that could not have a cursor key.

So `RV9_SS_RAW`: a read hands over whatever arrived, as soon as it
arrived, unechoed and unfiltered.

It lives on the **path**, not the device. Echo and newline translation are
device settings — they were there first and they describe the wire — but
raw is a claim about how one program intends to read, and putting it on
the device would let a program leave somebody else's shell in a strange
state. It is inherited across `fork` with the path, which is what makes
setting it on stdin mean anything at all.

### Escape sequences without a timed read

The usual way to tell `ESC` from the start of `ESC [ A` is a read with a
short timeout, and RV-9 has no such thing.

It does not need one. A sequence is recognised only when the whole of it
is already in hand: terminals send `ESC [ A` in a single write, so an
`ESC` sitting at the end of the buffer really was somebody pressing
escape. The ambiguity that motivates the timeout does not survive contact
with how terminals actually behave.

### Drawing, and the cost of a setstat

Every `m_cursor` is a setstat, which is a write, which over SSH is a
packet. A full redraw of a 53-row window is 53 packets. So the editor
redraws one line for ordinary typing and the whole screen only when it has
to — a scroll, a newline, a resize. That is the classic approach and it is
here for a reason that is specific to this system rather than inherited
from tradition.

The text is one flat buffer with newlines in it, and everything else —
which line we are on, where it starts — is recomputed by scanning. Eight
kilobytes is small enough that scanning is free, and an index would only
be a second thing to keep correct.

### And the bug that was not in the editor

The first run looked right and then, after a while, the shell reappeared
underneath the editor and the two of them fought over the keyboard.

`shell.c` waited **thirty seconds** for a command and then printed a
prompt anyway. It had not stopped the command; it had arranged for two
processes to read one terminal. Nothing had noticed before because nothing
had ever legitimately run for half a minute — every command so far
finished immediately or was a daemon nobody typed at. An editor is the
first program a person sits inside.

The shell now waits `RV9_WAIT_FOREVER`, which is what `rshd` and `sshd`
were already doing in all but name with their hour-long timeouts.

A second one surfaced the same night, from the other direction: `sshd`
stopped on *any* failure to open `/ssh0`, on the reasoning that neither
cause got better by retrying. Two do not — no password, and another sshd
already listening. Everything else is one connection that went wrong, and
a server that retired over that is a server anyone can switch off from
across the network by connecting and hanging up. It now stops only on the
two permanent causes and waits a second before listening again.

### Measured

Driven over SSH from a scripted pty, reading back the escape stream:

| | |
|---|---|
| typing a character | one row redrawn, status, cursor |
| `ESC [ A` with a remembered column | lands where it should |
| resize 24×80 → 40×100 | next keystroke redraws 39 rows, status at row 40 |
| 45 seconds idle | not one unsolicited byte |
| `^X` with changes | warns, then leaves on the second press |
| save | `/f0/edtest` is 23 bytes, which is exactly the text |
| three dropped connections | `sshd` still serves the next real login |

## 19. A window you draw on by writing SVG to it

```
pic > /w0
cat /f0/picture.svg > /w0
```

`/w0` is a device. You write a picture to it and the picture appears. OS-9's
window devices took drawing commands written to a path; this is the same
idea with a format that already exists, so nothing above the device needs a
graphics API, a context, or a handle — `pic` emits SVG and does not know
what a panel is. Redirect it to a file or down a socket and the same bytes
go there.

**SVG, and deliberately not CSS.** Presentation attributes only: `fill`,
`stroke`, `stroke-width` on the elements themselves. A style language means
a cascade, a selector engine and a box model — an enormous amount of
machinery to arrive back at "this shape is red".

### There is no framebuffer, and that decided everything

320×172 at two bytes a pixel is 110 KB. This board has about forty free. So
the panel is painted in bands eight rows deep: fill a band, push it down the
SPI bus, move on.

A renderer with a framebuffer draws shapes in any order and composites as it
goes. A banded one has to answer, for each band, "what is in you?" — which
would normally mean compiling the document into a display list, and sizing,
allocating and maintaining that list.

Instead the **SVG source is the display list**. The document is held as
text and re-read once per band. Re-reading a few kilobytes twenty-two times
costs far less than the memory a display list would need, and there is no
second representation that can fall out of step with the first. The parser
had to be written anyway; this is the only thing that uses it.

### The panel belongs to neither device

`/term` is a text console and `/w0` is a graphics window and there is one
piece of glass. A driver that owns hardware another driver also needs is a
driver that has to know about the other one, so the ST7789 moved into
`panel.c`: brought up by whoever asks first, handed out as geometry and a
blit, serialised by a lock so two devices cannot interleave halves of an SPI
transfer. `lcdcon` lost forty lines and gained nothing it has to think
about.

### Memory only while it is open

The document buffer, the band, the coverage row and the point list come to
about nine kilobytes — a quarter of the free heap. They are allocated in the
driver's `open` and freed in its `close`, so a device that is used
occasionally costs nothing the rest of the time. Measured: free heap sits at
31 KB and dips to 20 KB while a picture is being drawn.

This is what the driver open/close hooks from §17 were for. They were added
so an SSH session could wait for a login; the second user of them turned out
to be memory.

### Anti-aliasing, and a bug that hid inside it

Coverage is computed with four subsamples per pixel row and exact
fractional span ends horizontally. On a 1.47-inch panel an unantialiased
diagonal is unmistakably a staircase, so this is not a luxury.

Circles become polygons, with the segment count following the radius. The
first version took the nearest of sixty-four tabulated angles, which is
wrong in a way that survives every check that looks at size: a circle of
twenty-three segments got its vertices at *uneven* angles and came out
visibly lumpy rather than round. The radius was right, the area was right,
and it looked like a potato. The table is interpolated now.

That bug was found by compiling the renderer on the host and looking at the
output, which is worth more than the hour it took to arrange — `raster.c`
has no ESP dependencies at all, and the device driver needs about forty
lines of stubs.

### The one that was not a bug at all

Six of the fixes above are real and stand. None of them was the reason SSH
had become unreliable.

The reason was power save. ESP-IDF leaves the station dozing between
beacons; that costs latency, so turning it off looked like an obvious win
and was made an hour before anyone measured SSH. With `WIFI_PS_NONE` this
board loses its access point outright -- reason 200, beacon timeout,
association dropped, reassociated, dropped again -- and the symptom
surfaces a layer up as connections that fail for no visible reason.

With the default restored: twenty scripted sessions in twenty.

Two lessons, both cheap to state and expensive to learn. **Measure before
the change as well as after** -- there was no before-reading for SSH on
that network, so there was nothing to compare against and the regression
was invisible as a regression. And **suspect your own recent changes
first**: the hunt ranged across the SSH transport, the channel layer, NFM,
the process table and the allocator before returning to a one-line default
altered the same afternoon.

### Two bugs that only the glass could show

Both were invisible to every test that did not involve looking at the
panel, and both were found by asking someone to look at it.

**The picture was drawn and then eaten.** `/term` repaints only the rows it
believes have changed. The first thing written to the console after a
picture scrolled it, marked every row dirty and painted text over the
whole thing — which is indistinguishable from the window device never
having worked.

One panel and no framebuffer leaves exactly one coherent model: whoever
painted last is what you see. `rv9_panel_take()` reports when ownership
actually changed, and a device that repaints incrementally takes that as
its cue to repaint all of itself, because what is on the glass is somebody
else's picture and every row of its own idea of the screen is stale. One
pointer compare per flush.

**The blit was not finished when it returned.** `esp_lcd_panel_draw_bitmap`
*queues* a transfer; the DMA engine reads the caller's buffer afterwards.
Every caller here paints into one buffer and reuses it immediately, so band
N+1 was written over band N while N was still going out the wire. It
appeared as horizontal bands of wrong pixels across the picture.

This was a latent bug in the **console**, not something the window
introduced: `lcdcon` has always reused one row buffer across queued blits.
It got away with it because consecutive text rows tend to hold similar
content, so the corruption had nothing to show. A renderer painting
twenty-two wildly different bands in a row does.

`rv9_panel_blit` now waits for the SPI completion callback on a semaphore
before returning. It costs the transfer time it was always going to
cost — about 2.6 ms for a full-width strip at 40 MHz — and makes the
buffer safe to touch on return, which is what every caller already
assumed it was.

### Measured

| | |
|---|---|
| a picture on the panel | ~59 ms, including forking the program |
| document | up to 4 KB |
| heap while open | 31 KB free, dipping to 20 KB |

`tools/hosttest/run.sh` builds the renderer for this machine and asserts
against it: the rasteriser's fills, half-open edges in both axes,
anti-aliasing in both directions, clipping, even-odd against nonzero,
strokes leaving interiors alone and the byte swap; and the path parser's
commands, relative forms, implicit linetos, curve bulges, subpath holes,
arcs closing into round circles, and that unknown commands stop a path
rather than misreading the rest as coordinates.

It also renders to a PPM you can open, which is how the lumpy circles were
caught. That matters more than the assertions: a picture can be
geometrically perfect and still be wrong in a way only an eye catches, and
the panel is the one part of this system no test can reach.

### `<path>`, and the arcs

`M L H V C S Q T A Z`, absolute and relative, with subpaths and
`fill-rule`. Curves are flattened to line segments before the rasteriser
sees them, so it never learns that anything was curved -- adding paths
needed nothing from it except letting a shape have more than one contour.

**A shape is now a list of contours**, and that is what makes a hole a
hole: the scanline has to see the outer ring and the inner ring at once, or
the winding rule has nothing to cancel against. Filling contours one at a
time can draw two rings and never a donut.

**Arcs are done by bisection, not trigonometry.** Divide out the radii and
the ellipse becomes a unit circle, where the midpoint of a short arc
between two unit vectors is just their sum, normalised. Halving four times
gives sixteen points and never needs a sine, a cosine or an arctangent --
which on a chip with no floating point is the difference between thirty
lines and a page of fixed-point trigonometry. The major arc takes the
negated midpoint; an exact half-turn, where the two ends give no midpoint
at all, takes the perpendicular on the side being travelled.

Measured: a chart with gridlines, a filled area, a cubic series, bars, a
donut and a stroked zigzag is 1,077 bytes of SVG and draws in well under a
second, start to finish over SSH in 0.9 s.

### One more stray-point bug

The donut came out with a wedge cut from its edge to its centre.

`M160 86 m -40 0` is how a circle gets written, and the `M` starts a
subpath that the `m` immediately abandons. That one-point subpath was too
short to be a contour and was dropped -- but its *point* stayed in the
shared array. Contours are consecutive runs of that array, so a point
belonging to no contour shifts every contour after it by one, and the ring
was drawn partly from the stranded centre point.

The fix is to take the points back out, not merely to stop counting them.
It is the third bug in this renderer of exactly one kind: two things
disagreeing about how big something is.

### `<text>`, and the font that was private

A chart without labels is a picture of a chart, so this is not a flourish.

The font is the console's own — 10x20 cells with four bits of coverage per
pixel — and it moved into a shared `font.h` to get here. It was `lcdcon`'s
private business until something else needed to draw a character, and the
alternative was a second copy of the geometry constants beside a second
extern. Two copies of a number like *the baseline is at row 15* is how a
renderer comes to disagree with itself about where text sits.

That number had to be found by looking: `H` and `x` stop at row 14, `g` and
`p` descend to 17, so the baseline is 15 of 20. The console never needed
it — it puts a glyph in a cell and the cells line up by construction — but
SVG positions text by its baseline, so it has to be written down.

Glyphs carry their own anti-aliasing, so there is no scanline conversion:
each destination pixel samples the glyph and blends by what it finds.
Labels are usually *smaller* than twenty rows, and nearest-neighbour
downscaling of an anti-aliased face looks like gravel, so each destination
pixel takes nine samples in a 3x3 grid and averages them. At label sizes
that costs nothing and keeps thin strokes grey rather than missing.

**`font-size` and `text-anchor` inherit**, and getting that wrong was
instructive: reading them only from the element they appear on *looks like
it works*, because the text still draws. It just silently ignores every
group meant to style it — which is the normal way to write SVG, one group
per axis. The symptom was labels at the wrong size in the wrong place, with
nothing in the parser to suggest why.

One font, one face. `font-family` is accepted and ignored, because there is
exactly one in the system and pretending otherwise would be a lie told in a
parser.

### What it does not do

- **x-axis-rotation on arcs**, which is parsed and ignored. A rotated
  ellipse needs the full endpoint-to-centre conversion with a rotation
  matrix, and nothing that draws a pie chart or a map outline asks for one.
- **Text families, weights, styles and rotation.** One face, upright.
- **Documents over 4 KB.** Enough for a chart, not for a map. The way out is
  already implied by the design: the source is re-read once per band, so it
  need not be in RAM at all -- it could be re-read from a file, and the
  limit would become storage rather than memory.
- **Rotation and skew.** `transform` handles translate and scale, which is
  what a dial or a plot needs; anything else means carrying a full matrix
  through the point pipeline.
- **Opacity**, gradients, patterns, clipping paths.

## 20. Waking a cooperative kernel from an interrupt

The native kernel is cooperative: `waitq_push`, `waitq_pop_best` and
`block_on` re-link thread lists, and an interrupt landing in the middle of
one leaves it in pieces. That is why the ISR-safe primitives were stubs,
and the comment on them said making the wait queues interrupt-safe was
real work.

It was the wrong problem. **An interrupt does not need to touch the wait
queues at all.**

What a waiter waits on is the *count*. An interrupt can raise that safely
in four instructions with interrupts masked. Waking is merely how a
sleeping thread finds out, and that can be left as a note for the kernel
to act on in thread context, where the lists are nobody else's business:

```
   interrupt            count++          note the wait queue
                           |                     |
   thread context          |            drain at the next
                           |            turn of the scheduler
                           v                     v
   a thread about      sees the count      a thread already
   to block            and never blocks    asleep is woken
```

So `rv9k_sem_give_from_isr` raises the count, checks whether anything is
waiting, and if so pushes the wait queue onto a small ring. `drain_pending`
empties that ring at the top of `reschedule()` and of the kernel's serve
loop, calling `waitq_pop_best` in thread context as usual. The queue's
send does the same with its item and its `not_empty` queue.

**The latency is a scheduling round, not an interrupt**, and that is the
honest description: measured at effectively zero when the kernel is
running, and up to one host tick when it is idling in `vTaskDelay(1)`.
That is right for a semaphore. It is nowhere near enough for real-time
work, which is exactly why real-time processes do not come through here --
they are hosted by the preemptive scheduler in `kal_rt.c` and measure
pin-to-process in microseconds (§15).

The other half is that the *thread* side had to become interrupt-safe too.
`count` is now read-modified-written under a mask in `rv9k_sem_take` and
`rv9k_sem_give`, and the queue's `count`, `head` and `tail` likewise in
send and recv -- a task decrementing while an interrupt increments loses
an update otherwise. The kernel masks interrupts itself, with `csrrci` on
`mstatus`: it is below the seam, has no host to borrow a critical section
from, and a kernel that owns the machine should not be asking permission.

A ring that fills counts the loss rather than hiding it (`rv9k_pending_lost`).
Nothing is actually lost when it does -- the count or the item has already
landed, so nobody blocking afterwards misses it; only a thread already
asleep waits for the next one.

Tested against a real interrupt rather than a call from a task: a timer on
ISR dispatch fires while a thread is blocked, and the test asserts it is
woken promptly rather than on its timeout. Both the semaphore and the
queue. That test exists because this primitive had none, which is how
nobody noticed it did nothing.

## 21. Telemetry, and a chart that lied

A chart drawn once is a report. The other thing a small panel on a machine
is for is a number that moves, watched while the machine runs -- and
almost nothing §19 optimised for is what that needs.

`gauge` is that: it redraws continuously and says how fast it managed,
because the useful question about a telemetry display is not whether it is
pretty but whether it keeps up. **Thirteen frames a second, 76 ms a
frame**, which is comfortably above the roughly three-per-second a human
observer can act on. So for watching a dial, `/w0` was already finished.

### The cost is duty, not smoothness

At 3 Hz the display would still spend 76 ms of every 333 on itself: about
a quarter of the machine, to move a line a few pixels. Most of it redraws a
grid and some labels that never change.

Of that, ~14 ms was pure waste -- `rv9_panel_blit` spun while the SPI
transfer ran. It yields now, which costs nothing in wall-clock and hands
those milliseconds to anything else that can use them. It yields rather
than sleeps because the wait is 636 us, far shorter than a tick, and the
console clears the panel at init from a context that cannot block at all.

The remaining ~62 ms is parsing the document once per band and rasterising
every shape into every band it touches, changed or not. **Dirty-band
updates** are the lever -- a scrolling trace dirties a narrow slice and the
grid dirties nothing -- and they are deliberately not built, because they
only pay on a machine where a quarter of the CPU matters.

### Watching a loop without being one

`rt_stats` reports the caller's own timing, which is right for a loop
checking itself and useless for a display: a program that draws pictures
has no deadlines worth watching. So real-time statistics come through
`sysinfo` as `RV9_SYS_RT`, one record per task, and a panel can watch a
control loop from outside it.

That was built on `rv9_rt_stats_by_index`, which had been declared,
implemented, and used by nothing -- the same category as the ISR-safe
primitives in §20, and the same lesson: an unused interface is an untested
one.

### The chart that lied

The first version scaled the plot to the range of the data on screen,
which is the obvious way to show texture in a signal that barely moves.

It looked like a heartbeat monitor. It was a lie.

The loop executes in **one to two microseconds**, so what filled the plot
was a one-microsecond flicker -- the last bit of a microsecond counter --
stretched across a hundred and ten pixels. The numbers printed beside it
were correct throughout. What was invented was the *shape*, which is what a
glance actually reads.

This is a truncated bar chart arrived at by good intentions, and the guard
against it was written and set far too low: the band was widened only when
the observed range fell below four microseconds, which a signal living at
one to two sails straight past.

A duration has a meaningful zero, so **the axis starts at zero**. The top
follows what has been seen, with headroom, and never falls below a floor
that keeps a trivial signal looking trivial. A loop using two microseconds
of a twenty-thousand microsecond budget now sits just off the baseline,
and would visibly climb if it ever stopped doing so.

It is the failure mode that matters most for machine telemetry. An
operator reads the silhouette, not the digits, and a display that
manufactures drama from noise teaches people to ignore it -- so that the
once it means something, nobody looks.

### Still honest about

- **`max_jitter_us` is monotonic**, worst-ever rather than current, so as a
  trace it is a staircase that flattens. Per-activation lateness is not
  exposed; that would be a small addition to `rv9_rt_stats_t`.
- **`control` is too well behaved to be interesting**, using a hundredth of
  a percent of its budget. Demonstrating that the display shows something
  when there is something wants a loop doing real work, or one deliberately
  overrunning.

## 22. The floor of a stack

Phase 4 made small stacks easy — `stack_size` in a module's `build.conf`,
and a measurement to size it against — without making them safe. That is a
poor trade: a `stacks` report showing 60 bytes spare invites cutting to
zero, and the failure mode was a silent overwrite of whatever the allocator
had put underneath, surfacing minutes later as an unrelated crash.

### A guard, not a wall

The lowest four words of every stack are painted with `RV9K_STACK_PAINT`
along with the rest of it, and never legitimately written: a stack grows
down, so the deepest thing a thread does lands there first.

`reschedule()` checks them on the way out of every thread. That is the only
moment the question can be asked cheaply — a running thread keeps its stack
pointer in a register, so there is nothing in memory to inspect until it
stops, and every thread stops here.

Four words is not a wall. A single large local steps straight over it.
Prevention needs the PMP, which is phase 7's last step.

### A place to land

Detection on its own is worth much less than it looks. By the time the
guard is noticed the write has happened, and what it landed on was whoever
the allocator put underneath — so the report arrives alongside a second,
silent failure in an unrelated process, and the useful half of the news is
buried under the useless half.

So every stack is allocated with 128 bytes underneath it that belong to
nobody: `RV9K_STACK_PAD_WORDS`. The thread is never told — `stack` points
above it, `stack_words` excludes it, and `stacks` reports the size the
module asked for. It is not more stack; it is somewhere for a modest
overrun to land. The ordinary case, a call chain a frame or two too deep,
now damages nothing at all, and the guard above it still fires.

That makes the report worth having: the failure is attributed *to the
thread that caused it*, by name, and nothing else is wrong.

### Killing without touching

A faulted thread is marked `RV9K_FAULT_STACK` and `RV9K_DEAD`, and the
scheduler switches away from it — but it must not *save* it. `rv9_ctx_switch`
pushes fourteen words onto the outgoing stack, which for a thread that has
already run off the bottom means fourteen more words of somebody else's
memory. One victim's overflow would become two.

So `switch.S` gained `rv9_ctx_load`: the half of a switch that loads without
saving. The registers of a thread that will never run again are worth
nothing, so they are dropped rather than written somewhere harmful.

### The kernel does not say so

`stack_fault()` prints nothing. The kernel is below the seam, it has no
console, and it has never heard of a process — it does not know that the
thread it just killed was `pid 7 ('gauge')`. It records the fact and moves
on.

The layer that *does* know is the process manager, and it finds out by
asking: `rv9_task_alive()` and `rv9_task_fault()` through the KAL.

### The funeral is held by a passer-by

A process normally ends by returning through `proc_trampoline`, which is
where its paths are closed, its module unlinked and its status recorded. A
thread the scheduler killed never returns, so none of that happens: its
paths stay open, its module stays linked, and its parent waits forever for
a status nobody will write.

The tempting fix — have the scheduler call the process manager — is the
deadlock we already paid for once, taking the process lock from inside
`reschedule()`. Instead, anybody who *looks* at a process may notice the
task is gone and finish the job: `rv9_proc_wait`'s poll loop, and the ager,
which is the one thing that walks every process on a timer and so catches a
faulted process with nobody waiting on it. Whoever gets there first claims
it with `collecting` and does the work outside the lock, because closing
paths reaches into the I/O manager and the two locks must never be taken in
both orders. The parent gets `-RV9_PROC_ERR_FAULT`, which is negative and
is not a status any module returns.

### Holding the corpse

The first version had a race that would have been very hard to find. A
thread's handle is a pointer into a fixed table; `reap_dead()` frees the
stack and releases the slot; the next `fork` reuses it. A watcher still
holding the old handle then asks after a corpse and is told a healthy
stranger is alive and well — so it waits forever, which is the bug the
whole exercise was meant to remove.

A faulted thread now holds its slot after its stack is returned. The memory
goes back immediately, because that is the expensive part; the slot and the
name stay until `rv9k_thread_release()`, which the process manager calls
once it has read the fault off. Threads that end normally are never held.

### Testing it without wrecking the heap

The obvious test is a runaway recursion, and it is the wrong one: it writes
hundreds of bytes below the stack, so a test of the detector becomes a test
of how much heap corruption the system survives.

What the detector watches is the guard, so the test writes the guard and
nothing else — `smasher_thread` takes its own `rv9k_self()->stack` and
scribbles on the four words it is not allowed to touch. The effect on the
check in `reschedule()` is identical to a real overrun; the effect on the
rest of the heap is nil. A bystander thread runs alongside to prove that
killing one thread does not disturb the others, and the slot-reuse check
covers the race above.

Those tests cannot run in the shipped build: they call `rv9k_init()`, which
is fine when RV-9 is a guest and fatal when the kernel being torn down is
the one underneath you. They run in a `CONFIG_RV9_KERNEL_NATIVE=n` build —
42 passed, 0 failed.

### `smash`, and the recursion that wasn't

The kernel test proves the detector. It does not prove the funeral, which
is the part with the locks in it, so there is a module: `smash` runs off
its own stack on purpose, on the real board, through the whole chain.

It descends one small frame at a time and asks `sysinfo` after every one
how much of its own stack is left, stopping the moment the answer is under
eight bytes — by which point the guard has been written and nothing below
it has. Self-calibrating rather than guessed, because the number of frames
that fits in a stack is not knowable from inside the module, and guessing
high is the runaway recursion again. It refuses to run where the size
reads zero, which is the host backend saying it does not know.

The first two runs reported "the measurement never fell": sixty-four levels
deep and not one byte of stack consumed. `return descend(...)` is a tail
call, and at `-Os` GCC turns it into a jump back to the top with the frame
reused. Reading the frame *after* the recursive call forces it to outlive
the call, and the descent became a descent. A test that cannot fail is not
evidence, and this one had been quietly passing nothing.

What the board prints now:

```
rv9> smash
descending...
E (14054) rv9-proc: pid 9 ('smash') killed: stack overflow
smash returned -6
rv9> echo hello
echo: hello from a forked module
```

`-6` is `-RV9_PROC_ERR_FAULT`: the shell's `wait()` returned rather than
hanging. Three runs in a row cost 816 bytes of heap, which is four retained
process descriptors and nothing else — the paths were closed, the module
unlinked and the statics freed by a passer-by.

## 23. A floor under the heap

The board died like this:

```
ESP_ERROR_CHECK failed: ESP_ERR_NO_MEM at phy_track_pll_init
abort() was called
Rebooting...
```

The window, an SSH session and a control loop, all at once. Nothing RV-9
did was wrong — it allocated what it needed and got it. The failure landed
on the WiFi PHY, which asked next and could not be told no, because
ESP-IDF's internals do not return NULL when they run out; they abort. A
reboot, in a layer RV-9 does not own, caused by somebody else's request.
On a vehicle that is the whole system stopping because a display wanted a
buffer.

### It is not RV-9's reserve

The asymmetry is the whole design. RV-9's allocations come through the KAL
and *can* be refused — a NULL propagates to "no memory to start it" and the
shell carries on. ESP-IDF's cannot. So the last 12 KB are never offered to
the side that can take the news.

`CONFIG_RV9_HEAP_FLOOR` bytes are simply subtracted from what RV-9 believes
it has. `rv9_heap_free` still says what exists; `rv9_heap_available` says
what may be spent, and that is the number that decides whether the next
process starts. Reporting the first as though it were the second is how a
system walks confidently into a wall, so `free` prints both.

The floor does not make more memory exist. It chooses which of two failures
happens, and only one of them leaves a system running.

### One copy of it

Memory moved to `kal_mem.c`, built whichever kernel backs the KAL, because
allocation on this board comes from ESP-IDF either way. The two backends
had carried identical copies since phase 0 and had already drifted once.

Two places allocate without going through `rv9_alloc` and both had to be
brought in: the native backend's thread stacks, which need internal RAM,
now use `rv9_alloc_internal`; and `xTaskCreate` on the FreeRTOS backend,
which allocates its own stack and knows nothing of the floor, so
`rv9_task_create` asks first. A stack is the largest single thing a new
process wants, and it is exactly the request that should be refused rather
than granted out of the reserve.

`rv9_alloc_critical` spends the reserve on purpose. There is one thing
worth spending it on — making a failure legible — and nothing a module can
reach should call it.

### Growing a record without breaking what reads it

`free` needed three new numbers, which meant three new fields on
`rv9_sys_mem_t`. The old code began `if (len < sizeof(rv9_sys_mem_t))
return -1;` — so growing the record would have made every module in the
store fail to read it, having asked for the length it was built against.

It now fills what the caller asked for, up to what it knows: fields may be
appended, never reordered or removed, and a short caller gets a correctly
filled prefix. That is a patch on one record, not a mechanism. The
extensible manifest is the next job.

### Testing a reserve without exhausting the machine

The KAL self-test raises the floor above the entire heap rather than trying
to run out of memory, which would be a test that damages what it runs on.
It checks the refusal is a NULL and not an abort, that it is counted, that
DMA memory is refused too — same RAM, different capability mask, and
treating the pools as separate would let the reserve be spent three times
over — that `rv9_alloc_critical` still works, and that lowering the floor
gives the memory back. 45 passed, 0 failed.

The graceful-refusal end of it was checked on the board with a deliberate
52,500-byte floor, leaving 556 bytes:

```
rv9> mdir
mdir: no memory to start it
rv9> free
free: no memory to start it
```

The shell is still there. That is the entire claim.

### What it is not

A floor, not a budget. It stops RV-9 taking ESP-IDF's last bytes and says
nothing about one process taking another's — any program may still consume
everything RV-9 is allowed to have. A per-process limit, and a reserve
inside the reserve for real-time work, are both still missing.

And 12,288 is a starting point rather than a measurement: the size of the
thing that aborted, plus margin. Sizing it properly means watching `low
water` under the heaviest load the system will really carry.

## 24. What a program says it needs

> A program should be able to describe what machine resources it requires
> before RV-9 agrees to run it.

The fixed header holds four numbers — static size, stack hint, type, entry
— and they were never going to be enough. A resource contract wants heap
ceiling, execution class, period, deadline, minimum inter-arrival, worst-
case execution, required devices, exclusive versus shared ownership,
failsafe state, capabilities, which compiler built it. Adding each of those
to the header in turn means breaking every module in the store, in turn.

So the header points at an optional list of tagged values instead. What was
`reserved1` is now `manifest_offset`; zero means no manifest, which is
every module built before this existed, so nothing had to be rebuilt to
keep working. The list lives between the name and the code:

```
uint16_t tag
uint16_t len        bytes of value, padding not counted
uint8_t  value[len]
padding to the next multiple of four
```

Four-byte alignment for a two-byte header costs a couple of bytes per entry
and buys aligned numeric values, which matters when the producer is a
Python script and the consumer is a RISC-V core reading flash.

### The bit that says "you must understand this"

An unknown tag is normally skipped. That is what makes the format worth
having: a compiler can emit tomorrow's field into today's system and both
sides stay honest.

But some requirements cannot be quietly dropped. *This program must never
allocate.* *This device must be mine alone.* A loader that skips one of
those has agreed to a contract it does not understand, which is worse than
refusing the module.

So the top bit of the tag says which kind it is, and an unknown mandatory
tag is a refusal — `RV9_MOD_ERR_CONTRACT`, "requires something
unsupported". The bit lives in the tag rather than in a flags field so that
the two versions of a field are simply different tags: the producer decides
per value whether being understood matters, and `control` marks exactly one
of its six — `heap_max = 0`.

### Declared, not yet enforced

Sixteen tags are registered and most have no consumer. That is the point,
not a gap: the numbers are the agreement between the compiler and RV-9, and
a program may describe itself completely to a system that acts on part of
it. Enforcement arrives later without anything being rebuilt.

Two act today, and both replace a guess with the program's own figure:

- **`RV9_MTAG_STACK`** when the header's `stack_size` is zero, ahead of the
  8 KB default that nobody chose.
- **`RV9_MTAG_PERIOD_US`** for a real-time process forked without one.

The second is the more interesting of the two, because it moved a number to
where it belongs. `control` used to carry `#define DEFAULT_PERIOD_US 1000`
and `rt` used to default to 1000 as well — the same figure written twice,
in two places that could not see each other, neither of them the control
law. Now `rt control` passes zero, the process manager resolves it from the
manifest, and `env->rt_declare(0)` means "the period I was admitted at".
The rate of a control law is a property of the control law:

```
rv9> rt control
control: 2000 activations at 1000 us     <- from build.conf
rv9> rt control 2000
control: 2000 activations at 2000 us     <- an operator overriding it
```

### Writing and reading

`build.conf` gained the contract keys, so declaring one is a line of text
next to the module's source rather than a change to the build:

```sh
desc="periodic PI control loop with honest jitter reporting"
class=realtime
heap_max=0
period_us=1000
deadline_us=1000
wcet_us=50
mandatory="heap_max"
```

`tools/modinfo.py` reads it back, because a format nobody can inspect is a
format nobody will trust:

```
control
  stack          1536
  manifest
     desc           periodic PI control loop with honest jitter reporting
    !heap_max       0 (no heap at all)
     class          realtime
     period_us      1000
     deadline_us    1000
     wcet_us        50
```

The declared WCET is 50 µs and the loop reports 26–27 µs worst observed,
which is the beginning of item 13: a claim RV-9 can check rather than
believe.

`mkmodule.py` also accepts a numeric tag, so a producer newer than these
tools can emit a field neither of them knows.

### Testing data written by somebody else

A manifest is read off flash, which makes it hostile input: a length that
runs past the end of the module, an offset pointing back into the header,
an entry that does not advance. None of those can be produced by
`mkmodule.py`, so a test that reads only well-formed modules tests nothing
that matters.

`main/module_test.c` builds its images in memory instead — including the
broken ones and the unknown-mandatory refusal, neither of which would
otherwise be reachable without a way to get a deliberately bad module onto
the board. One bounds-checked walker serves the finder, the numeric
accessors and the contract check, so there is one place to get the bounds
right. 17 passed, 0 failed.

### One record that still cannot grow this way

`rv9_sys_mem_t` gained three fields for the memory floor, and the reader
had to be taught to fill a short caller's prefix rather than reject it.
That is a patch on one record. The sysinfo array records — `rv9_sys_module_t`,
`rv9_sys_stack_t` — are worse, because the caller's buffer length is
divided by the record size to get a count, so growing one silently changes
the stride an older module reads with. They need the same treatment the
module header just got, and have not had it.

## 25. Asking, rather than starting

> The compiler describes what a program promises and what it requires.
> RV-9 determines whether the physical machine can honour that contract.

The manifest gave RV-9 the first half. This is the second: `fork_rt` now
refuses before it allocates anything.

Ordinary processes are still merely started, and that is right — an
ordinary process that runs late is slow, and nothing else depends on its
timing. A real-time process that runs late is wrong, and something else
usually does.

(That holds for *timing*. §26 adds admission on what a program says it must
own, and that applies to every process: two programs driving one output is
wrong at any priority, and the scheduler has nothing to do with it.)

### Four ways to say no

Each is a separate error code rather than one refusal, because each is
actionable by a different person:

| refusal | what it means | who fixes it |
|---|---|---|
| `CONTRACT` | the declaration contradicts itself | a `build.conf` line |
| `NOSLOT` | all four real-time slots are taken | stop something |
| `NOMEM` | its stack and heap cannot be guaranteed | free memory, or declare a smaller stack |
| `UTILISATION` | the CPU is already promised | nothing, until something finishes |

All four, on the board:

```
rv9> rt control 200
E admit 'control': deadline 1000 us is longer than its 200 us release interval
control: its declaration contradicts itself

rv9> rt iolat & rt iolat & rt iolat
E admit 'iolat': wants 300 permille, 600 already promised, ceiling 700
iolat: the CPU is already promised
real-time promised  60.0% of 70.0%

rv9> rt control & (x4) ; rt control
E admit 'control': all 4 real-time slots are taken
control: no real-time slot free -- stop one first
```

The contract check is the cheap half of a resource certificate: an
operator typing `rt control 200` is asking for a 200 µs release from a
program that says it may take 1000 µs to finish, and arithmetic settles
that without running anything.

### The ceiling is headroom, not a theorem

70%, and it deliberately is not a rate-monotonic bound. RV-9's real-time
tasks all run at one host priority, so the classic bound does not describe
them. What the remaining 30% is for is everything that is not in the sum at
all: WiFi, the panel, the SPI driver, RV-9's own kernel, and every ordinary
process. Admitting real-time work up to the last percent starves the system
the real-time work depends on.

### Three kinds of number, kept apart

`rt` with no arguments answers the question rather than printing usage:

```
real-time promised  60.0% of 70.0%
slots               2 of 4
declared            2
measured            0
unaccounted         0
```

Only **declared** is a promise being kept. **Measured** stands in for work
whose author did not say, using the worst execution it has actually shown —
which is a floor on its true cost and never a bound, so the report says the
total is a floor whenever any of it rests on one. **Unaccounted** is work
that has neither said nor run, which an event-driven process is until it
declares its minimum inter-arrival from inside.

Collapsing those into a single "60% used" would hide whether the other 40%
is actually free. That distinction is the whole value of the number.

### What it cost elsewhere

**The shell learned `&`.** It had been typed at this shell for months and
silently handed to the module as an argument — which is why `gauge &` held
the terminal until it finished. There is no job table and no notification;
`procs` is where a background job is looked at. What it buys is two things
running at once, without which none of the above could be seen.

**Two numbers moved to where they belong.** `iolat` was taking the 8 KB
default and using 740 bytes of it; it now declares 2,048. `rt` was taking
8 KB and using 720. Three real-time processes did not fit before that, and
the first attempt at the utilisation demonstration was refused for memory
instead — the admission check working correctly and telling me something I
had not asked about.

### A bug the demonstration found

`stacks` reported `iolat` with a 34-megabyte stack.

A real-time process runs on the host's scheduler, so its task handle is a
FreeRTOS TCB and not an `rv9k_thread_t`. The native KAL was casting every
handle to a thread regardless, and reading whatever sat at that offset.
`rv9_task_stack` had done this since real-time processes existed; the
stack-guard work added `rv9_task_alive`, `rv9_task_fault` and
`rv9_task_reap` to the same mistake, and those are worse — a healthy
real-time process whose borrowed bytes happened to read as `RV9K_DEAD`
would have had its funeral held while it was still running, and
`rv9_task_reap` would have written into a live TCB.

`rv9k_is_thread()` answers exactly: the kernel's threads live in one fixed
array, so "is this pointer inside it, at an element boundary" is a total
answer rather than a heuristic. Every handle-taking function in the native
KAL now asks first — including `rv9_task_delete` and
`rv9_task_priority_set`, which had the same latent hole.

The wrong number was visible for one reason: `stacks` prints what it is
told. Measurement caught a bug that had been silently corrupting nothing
in particular for weeks, and would eventually have corrupted something
specific.

## 26. Who owns the motor

Everything below the I/O manager is happy to be used twice. Two processes
open `/gpio/2` and the driver hands each of them the pin. Two processes open
`/pwm0/3` and the driver allocates each of them an LEDC channel — two
channels, both wired to the same physical output. Nothing fails, no error is
reported, and the pin does whatever the last writer said.

On a machine that prints, that is a curiosity. On one that moves, it is a
control loop and a diagnostic somebody left running, arguing through a
servo. The hardware has no way to prefer either.

### The resource is the unit, not the device

`/gpio/2` and `/gpio/3` are separate resources on one device, because they
are separate pins. So a claim is keyed by the full path as opened, not by
the device — which also makes a *file* a resource, and an exclusive open of
one a lock, for free.

Comparison is exact. Whether `/r0/notes` and `/r0/NOTES` are one resource or
two is a question about the file manager, and this layer does not
second-guess it.

### One record per owner

Five processes sharing `/term` is five records. A single record with a count
would be smaller and would answer *is it busy*, which is the question a lock
asks. The questions actually worth asking are *who has it* and — the one
coming next — *whose device was this when it died*. Neither survives being
reduced to a count.

```
rv9> owns
resource                  owner  refs  held as
/pwm0/3                   9      2     exclusive, reserved
/n0/listen/22             sys    1     shared
/ssh0                     6      1     shared
/term                     7      1     shared
/uart0                    7      2     shared
```

`sys` is a driver's own hold — `rv9_io_open_detached`, which `sshd` uses for
its listening socket. It belongs to no process, so no process exit can take
it away.

### Two kinds of reference, and why

A claim is refcounted, and the references come from two places.

Each **open path** holds one, released when the path closes. Each
**manifest reservation** holds one, taken at fork *before the program runs*
and released when it dies.

The second is what makes the guarantee meaningful. A program that declares

```
exclusives="/pwm0/3"
```

owns that output for its lifetime, not for the duration of one open — so
there is no window between its opens in which something else can take the
actuator. And it means the refusal happens at fork, where it costs nothing:

```
rv9> hold &
holding /pwm0/3 as pid 9
[9] hold

rv9> hold
E (18045) rv9-claim: /pwm0/3 belongs to pid 9; pid 11 cannot be given it
E (18046) rv9-io: admit 'hold': it needs /pwm0/3 alone, and pid 9 has it
hold: a device it needs alone is owned (see 'owns')

rv9> pwm 3 1200
W (19048) rv9-claim: /pwm0/3 belongs to pid 9; pid 12 may not open it
/pwm0/3: owned by another process (see 'owns')
```

The second refusal is a fork that never happened. The third is an open
refused before the driver was asked for anything — which matters, because by
the time `ledc_channel_config` has run, the second channel is already
driving the pin the caller is about to be told it cannot have.

### `device` is checked too, and differently

`device` means *needed, shared*, and only its existence is checked. A
program that names a device this machine does not have is not going to work
on it, and finding that out at fork beats finding out at the first open,
three seconds into a startup sequence:

```
rv9> hold                       # with devices="/sd0" declared
E rv9-io: admit 'hold': it needs /sd0, which this machine does not have
hold: it needs a device this machine does not have (see the log)
```

### Dying is not a way to keep the motor

A program that ends by returning lets go of things because it is asked to.
A program stopped by the scheduler is not asked anything, and that is the
case the whole mechanism exists for. The exit hook releases reservations on
every path out of a process, including the ones nobody planned:

```
rv9> hold crash
holding /pwm0/3 as pid 8
now dying without letting go...
descending...
E (14057) rv9-proc: pid 8 ('smash') killed: stack overflow
hold returned -6

rv9> owns
resource                  owner  refs  held as
/n0/listen/22             sys    1     shared
...                             # /pwm0/3 is free
```

`hold crash` chains to `smash`, which runs off its own stack and is killed
by the guard from §22. The claim is gone; the next `hold` gets the pin.

Note what is *not* claimed: a path reference outlives its owner. If a
process dies while a child still holds an inherited path to the resource,
the claim stays until that path closes. That is the honest answer — the
device is still open — and it is why the two reference kinds are counted
separately rather than both being released at exit.

### Three things this exposed

**Chaining was a hole.** `chain()` keeps the pid and swaps the module
underneath it, and nothing re-read the new module's manifest. A program
could have started as something harmless and continued as something that
wants the actuator. The chain path now runs the same admission the fork
path does; claiming what the process already owns is a second reference to
the same record and costs nothing.

**A failed `rv9_task_create` leaked its path table.** The fork hook had
already run, so the process owned a table and whatever it had reserved — and
the trampoline that normally reports the exit is precisely the thing that
failed to start. Nothing would ever have told the I/O manager. Pre-existing;
found by asking who releases a claim on each path out of `fork_common`.

**The pid now comes before admission.** A reservation is made *for* a
process, so the number has to exist before the check — otherwise the check
and the claim are two separate moments and two programs can both pass the
check. A refusal therefore spends a pid, which is the right way round: the
alternative is a claim with nothing to release it.

## 27. Where a device is left

Ownership answered *whose was this*. This answers *and what should it be
left at*, which is the same row of the same table — a failsafe for a device
nobody owns is not a thing that can exist.

R9's §15 states the requirement exactly, and it is a strong one:

> It must compile to declarative safe-state operations that RV-9 or a
> trusted supervisor can perform **without executing code in the failed
> component**. A failsafe may name owned devices and constant safe
> configurations, but may not allocate, wait, call arbitrary functions, or
> depend on the failed component's private state.

So `RV9_MTAG_FAILSAFE` is not a string describing an intention. It is a
number and a device name, repeated once per actuator:

```
uint16_t tag = RV9_MTAG_FAILSAFE
uint16_t len = 4 + strlen(path)
uint32_t value
char     path[len - 4]
```

Nothing in that can allocate, wait, or call anything, because there is
nothing in it but a constant and a name. That is not a limitation worked
around; it is the reason the mechanism is trustworthy. A failsafe is
applied with its program already dead — frequently because it ran off its
own stack — and anything richer would be asking the corpse for help.

```
# build.conf
exclusives="/pwm0/3 /gpio/2"
failsafes="/gpio/2=0"
```

### You may only promise to park what you own

A failsafe is recorded against an existing claim, so the check is the data
structure rather than a rule somebody has to remember:

```
rv9> hold                       # with failsafes="/gpio/7=0" declared
E rv9-io: admit 'hold': it promises to leave /gpio/7 at 0, but never claimed it
hold: its declaration contradicts itself (see the log)
```

`CONTRACT`, not `BUSY` — this is a manifest disagreeing with itself, and
the fix is a `build.conf` line rather than stopping something else. Note
that `CONTRACT` is now reachable by an ordinary fork and not only by `rt`.

### On every way out, not only the bad ones

R9 distinguishes `on stop` (cooperative, while healthy) from `failsafe`
(applied to the wreckage). RV-9 does not need to: `on stop` runs inside the
module, so it has already happened by the time RV-9 sees an exit at all.
What is left is the same question either way — this device had an owner, it
no longer does, and the owner said where to leave it.

Applying it only on faults would mean the safety path is the one that
almost never runs.

### Demonstrated, not asserted

`hold` drives `/gpio/2` high, chains to `smash`, and is killed by the §22
stack guard:

```
rv9> pin 2 1
/gpio/2 := 1, reads 1

rv9> hold crash
holding /pwm0/3 and /gpio/2 as pid 9
/gpio/2 is now 1; RV-9 owes it a 0
now dying without letting go...
descending...
E rv9-proc: pid 9 ('smash') killed: stack overflow
W rv9-io: failsafe: /gpio/2 left at 0
hold returned -6

rv9> pin 2
/gpio/2 = 0
```

The pin is driven high on purpose so that 0 afterwards is evidence rather
than the value it happened to have anyway.

### A promise a device cannot keep

A failsafe outlives its program only on a device that holds its state when
the last path closes. `rv9_driver_t.retains` now declares which those are;
`/gpio` does, `/pwm0` does not — it gives the hardware channel back and
stops driving, which is its own safe state. Declaring a value for a
non-retaining device is not refused, because the value still holds while
the program lives, but admission says so:

```
W rv9-io: admit 'hold': /pwm0/3 does not hold its state when released;
          its failsafe lasts only until then
```

### The bug this found, which was the point of measuring

`/gpio` claimed to hold its level past close. It did not, and had not since
interrupts were added. Two defects in `gpio_unit_open`, both undoing what
`gpio_unit_close` had carefully preserved:

- `gpio_reset_pin()` ran on every *first* open — meaning every time the
  open count went 0→1, not once per pin. It restores the IOMUX routing and
  the pull-up, which is precisely what the first open exists to undo.
- direction was `s_output_count[unit] > 0`. With nobody open that is zero,
  so a reader arriving after a writer had gone reconfigured the pin as a
  plain input and stopped driving whatever the writer left on it.

```
rv9> pin 2 0
/gpio/2 := 0, reads 0        # true inside the open
rv9> pin 2
/gpio/2 = 1                  # and gone by the next one
```

`pin 5 1` to enable something and any later `pin 5` to check it would drop
the enable line — read-only observation with a side effect, on the exact
kind of signal this machine exists to hold steady. Two sticky per-pin flags
fix it: reclaim once per pin rather than once per generation of openers,
and let an output stay an output.

The failsafe work did not cause this. It made it *visible*, because a
failsafe is the first thing in RV-9 whose whole purpose is to still be true
after everybody has let go.

### What is still missing

`RV9_MTAG_CAPABILITY` remains registered with nothing behind it. And a
failsafe is applied when the *process* stops — not when it misses a
deadline while still running, which is R9 §15.3's `DEADLINE` fault. RV-9
detects overruns and counts them; it does not yet stop a component for
them. That is the next piece of §15.3, and it needs the component to be
stoppable from outside, which is `RV9_SIG_STOP` with nothing to send it.

## 28. A published value

One process computes something; another has to see it. RV-9 had no answer
to that at all — paths, signals and events, and none of them carries an
observation.

That is the gap under the whole of R9's reactive layer. §16.1 maps a
real-time component onto one RV-9 process and the reactive supervisor onto
*another*, so `MOTOR_CONTROL.speed` in a `watch` block crosses a process
boundary. `watch`, `state` and `transition` all read values produced
somewhere else. Without this they have nothing to observe.

R9 §18 states the contract:

> The R9 runtime representation of an exposed set should be fixed-size and
> preallocated. Each atomic publication should carry at least: a validity
> indication for first publication; a monotonically increasing publication
> sequence; a timestamp associated with the physical observation; the
> coherent set of exposed values. […] It must work with RV-9 process
> isolation and must not require allocation or blocking locks on a
> real-time path.

### A publication is a device

The obvious answer is shared memory, and it dies at phase 7: PMP isolation
exists precisely to stop one process handing another a pointer. The second
answer is a message queue, and it has the wrong semantics — `watch` wants
*the current value*, not every value, and a queue must either grow without
bound or throw things away. Both are wrong answers to "what is the speed
now".

So a publication is a cell on a device:

```
/pub0/CONTROL      pfm over pubmem, 16 cells of 64 bytes
```

A path survives isolation, because a read and a write go through the I/O
manager, which sits above the seam. And it arrives with naming, ownership,
`owns`, lifetime tied to the process, a directory, `del`, and an already
RT-resident transfer path — none of which a new mechanism would have had.
R9 §16.1's own instruction is that R9 should use RV-9's native mechanisms
"rather than recreate an operating system inside its runtime"; this is that,
taken literally.

`expose speed, error, output` becomes one `write` of one struct to one
path. Atomic because it is one call and one copy.

### The four layers, again

| layer | what it does |
|---|---|
| PFM | the discipline: names, sequence, coherence, one writer |
| pubmem | one job — where the store is. Forty lines |
| `/pub0` | the binding, as a descriptor module |

The driver is that small on purpose. A cell in a PMP region shared with an
isolated process, or one in memory that survives a restart, is a different
driver answering the same `arena` call and nothing above it changes.

### Coherence without a lock

One write is atomic on its own. Seeing half of a *set* — the new speed with
the old current — is the harder problem and the one §18 is about. A
seqlock solves it, and the asymmetry falls the right way round:

- the writer bumps a counter to odd, copies, bumps it to even. It never
  waits, never allocates, and never takes a lock.
- a reader takes the counter, copies, takes it again, and retries if they
  differ.

The entire cost of contention lands on the observer, which is the process
that can afford it. That is the reason for choosing this over a mutex, and
it is what makes the real-time half of §18's contract keepable.

Retries are bounded at eight and then counted, not waited out. On one core
a preempted reader needs exactly one; more means publications are arriving
faster than a snapshot can be taken, which is a fact about the system worth
having in `pubs` rather than a reason to sit in the I/O manager with a
deadline running.

### What every publication carries

```c
typedef struct {
    uint32_t seq;        /* 0 = never published; then one per publication */
    uint32_t len;
    uint64_t stamp_us;   /* when the observation was made */
} rv9_pub_t;             /* the value follows */
```

`seq` is §18's validity indication and its publication sequence in one
number: zero means the first `expose` has not happened, which is exactly
"not yet externally available" and needs no `unknown` type.

`stamp_us` is when the reading was *taken*, not when it was handed over.
Those differ by however long the computing took, and a reactive layer
deciding how stale a value is needs the first. A publisher passing zero is
saying "now".

The same struct goes both ways, deliberately: what comes out of one cell
can be written into another unchanged, which is what a bridge or a recorder
needs.

### One writer, many readers — and not by the claim table

Two processes publishing one value is not a race to be won; it is two
answers to a question with one reader. But the claim table cannot express
this. `RV9_MODE_EXCL` shuts out *everybody*, and one writer with many
readers is the entire shape of a publication. So the rule lives in PFM,
which is where the discipline belongs, and a second publisher gets
`RV9_IO_ERR_BUSY` at open.

### Cells outlive their publishers

A cell is not freed when the process that declared it exits. That is the
property that makes publication worth having *after* a failure and not only
during normal running: whatever investigates a stopped machine reads the
last thing the stopped component said, and when it said it.

Not automatic is not never — `del /pub0/NAME` clears a cell nobody has
open, so a component that will not run again does not hold one until the
next reboot.

### Not polling

R9 §21 asks that a watcher re-evaluate when a value changes rather than
poll. `RV9_PUB_GS_WAIT` blocks until the cell moves past a given sequence.

It waits on a *sequence*, not on an edge, so a publication that lands while
nobody is waiting is still seen afterwards — and several that arrive
together coalesce into one wakeup, which is also what §21 wants.

The wake costs the publisher almost nothing. Each waiter has an `armed`
flag, cleared by the wake, so a loop publishing at 1 kHz into a cell whose
observer works at 50 Hz makes fifty semaphore calls a second and reads a
word the other nine hundred and fifty times. The publication itself is
resident; the wake is not, and cannot be — so a control loop with the flash
cache off still publishes and simply does not wake anybody until the cache
is back. That is the right way round.

### Demonstrated

`control` now publishes what it measured every period, as one write:

```
rv9> pub CONTROL 0 0 0
/pub0/CONTROL <- 0 0 0

rv9> watch CONTROL 5 &
[9] watch

rv9> rt control
rt: started control as pid 11
seq    age_ms values...
3      0      123       1000      123
4      0      218       877       110
5      0      292       782       101
6      0      351       708       95
7      0      397       649       89
control: 2000 activations at 1000 us
  worst jitter   10 us
  worst execute  38 us
  overruns       0

rv9> pubs
name                 seq   bytes   cap  age_ms  by   rdrs  torn
CONTROL              2002  12      64   3001    -    0     0
```

Two processes, one publishing at 1 kHz and one observing, with the observer
seeing coherent triples: 13 µs worst execution warm, 30–38 µs on the first
run of a boot, no overruns, against a declared 50.

`pubs` shows the cell still holding its last value with nobody publishing
it, which is the state an operator arriving after the fact actually wants.

### The measurement that changed the code

It did not start there. Publishing straight from inside the loop reached
**49–50 µs** on the first run of every boot — the entire declared budget,
three times running, and not a fluctuation. Warm it was 20 µs.

The difference is the first call through a path: the write that pulls the
I/O manager's code into cache. Thirty microseconds of a control loop's
budget, spent once, on warming up.

The fix is one throwaway write before `rt_declare`, and it is not a trick.
It is the initialisation-versus-execution split RV-9 has always had (§10,
and alignment note 8) applied to the thing that had just been added to the
loop: *anything a real-time loop will touch should be touched once before
the loop makes a promise about how long it takes.* With it, cold drops to
30–38 µs and warm to 13.

Worth recording because the declaration was never wrong — 50 was always the
number, and 50 was always reached. Only measurement said which side of it
the loop was on.

### What this does not do

R9 §19's inputs — the supervisor writing `MOTOR_CONTROL.target_speed` — are
this same object with the ownership reversed, and need nothing added. What
is genuinely missing is a *name* R9 can rely on: cells are created by
whoever opens one for writing, so two components agreeing on `CONTROL` do
so by convention. Declaring published cells in the manifest, the way
devices are declared, would let admission catch a watcher naming a
component that will never publish. That is the next thing here.

## 29. Stopping what will not stop

§27 ended on two gaps that turned out to be one. A failsafe was applied when
a process *ended*, but nothing could end a process that did not end itself:
`RV9_SIG_STOP` existed and nothing could send it. And R9 §15.3 calls a
missed deadline a fault, which RV-9 counted and did nothing about — because
doing something means stopping a component from outside, which was the
first gap again.

Both are closed. `kill` asks and then insists, and a real-time program that
declares a missed deadline fatal is stopped at the late activation, with its
failsafe applied and the reason in the process table, in R9's order.

### Asking, then insisting

ABI 13 adds two calls:

```c
int (*signal)(int pid, uint32_t signals);   /* a request; the process decides */
int (*kill)(int pid);                       /* not a request */
```

The `kill` command uses them in the only order worth using:

```
rv9> deaf &
[13] deaf
rv9> kill 13
pid 13 did not stop when asked; stopping it
pid 13 killed
```

It sends `RV9_SIG_STOP` and waits two seconds, because a process that stops
when asked cleans up after itself — puts its terminal back, closes what it
opened, says goodbye. Only then `kill`. `kill -f` skips the asking. `deaf`
is a module that reads its signals and ignores them, which is the case the
whole thing exists for.

A killed process's paths are still closed, its claims still dropped and its
failsafes still applied: those are RV-9's to do however a process ends
(§27). What it loses is its own cleanup, and R9 §15.4 says that is correct —
orderly stop runs `on stop`, failure applies `failsafe`, nothing runs both.

### Not wherever it is

"Stop it now" cannot mean "stop it wherever it is". A process stopped while
holding a lock takes the lock with it, and the next thing to want that lock
waits forever. Killing a diagnostic would hang the control loop — a second
failure caused by recovering from the first.

So each kind of process is stopped where it is known to hold nothing.

**An ordinary process** is an RV-9 thread, and the kernel is cooperative: a
thread that is not running is parked at a switch point. That is a safe place
to stop it *unless it holds a lock there*. Both lock kinds now count on the
thread — `rv9_lock` in the KAL, `rv9k_mutex` in the kernel — and
`rv9k_thread_stop` refuses a thread whose count is not zero. The process
manager asks again every millisecond for half a second. A lock is normally
held for microseconds; one held for half a second is reported rather than
waited out.

The kernel already had `rv9k_thread_kill`, which does not ask. It is still
there for the one caller that wants it — a thread ending itself — and the
new call leaves the corpse held, fault `RV9K_FAULT_KILLED`, exactly as a
stack fault does (§22), so the process manager can read what happened before
the slot is reused. The funeral is then held on the spot by whoever called
`kill`: under RV-9's own kernel nothing sweeps the table on a timer, and a
killed process nobody waits on would otherwise keep its devices, which is
the opposite of the point.

**A real-time process** is a host task, and nothing above the KAL can see
where a host task is parked or what it holds. There is exactly one place it
is known to hold nothing: between activations. So `rv9_rt_stop` sets a flag
and gives the task's release semaphore, which wakes it at once if it is
waiting. Its `rt_wait` returns `RV9_RT_STOPPED` — and the process manager's
wrapper never hands that to the module. The process ends *inside* the call:
releases stopped, paths closed, failsafes applied, table updated, task
deleted, deep in the module's stack, which nothing returns into.

The same wrapper is how a deadline fault ends a process, and it is why the
loop never gets a vote: returning a fault to the code that has just been
judged would hand it the decision.

### Two races worth closing

Both are the kind that never shows in a test and would eventually show in a
machine.

A thread slot is reused as soon as its thread ends. `kill` reads a process's
task handle and stops it; if the process exits between those two steps, its
slot can be handed to a new thread, and the new thread is the one stopped.
So the check and the stop happen under the process lock, which a process
exiting has to take.

`rv9_rt_stop` gives a semaphore that a real-time task deletes when it
releases its slot. The task runs above everything, so it can preempt the
giver between the lookup and the give. The stop and the release both hold
off the scheduler across the lines that matter — giving with no wait is
permitted while it is held off, and the switch it earns happens on resume.

### A deadline, measured as one

Execution time — what RV-9 has always recorded — is not what a deadline is
about. It starts when the task gets the CPU. A loop released 800 µs late
that works for 300 µs has executed for 300 and *answered* in 1100, and only
the second is late. So every activation now records its **response**: its
lateness at release plus its execution. A miss is detected in two places:

- at **completion**, when the response exceeds the deadline — the earliest
  moment it is known, and the moment nothing about it can change;
- at **release**, when whole periods went by with no activation at all.
  Those releases missed their deadlines outright, and that is known before
  the next activation starts rather than after it ends.

The deadline is the declared `deadline_us`, or the period when none is
declared, cut to the actual interval if a module overrides its period. The
time from `rt_declare` to the first `rt_wait` is initialisation, not an
activation, and is not held to anything: nothing released it.

### Lateness had been measured from the wrong place

Lateness used to be the gap since the *previous wakeup*, minus a period.
That forgives lateness that accumulates: a loop late by 500 µs and then by
600 reports 100 for the second, because it only counts what got worse. As a
statistic that was tolerable. As the thing that decides whether a process is
stopped, it would have let a loop drift arbitrarily far behind its schedule
one forgivable step at a time.

Each task now keeps `due_us`, the time its next release *should* happen,
stamped before the timer is started and advanced by whole periods. Lateness
is measured against that. Stamping before the start makes the schedule, if
anything, a microsecond early — so lateness errs towards "late", which is
the direction an instrument deciding faults has to err in.

`control` against the new measure, four runs: worst jitter 17–46 µs, worst
execution 23–33 µs, no overruns. Its response is under a tenth of its
1000 µs deadline.

### What a miss means is the program's decision

A new manifest tag, `RV9_MTAG_ON_DEADLINE`, says whether a miss is a fault:

```
on_deadline=fault          # report is the default
mandatory="heap_max on_deadline"
```

Absent means `report`: every miss is counted, in `RV9_SYS_RT` alongside the
worst response, and nothing is stopped. That is what RV-9 did before, and it
is the right answer for programs whose job is to *measure* lateness —
`evlat` stopped at its first late event would measure nothing. `control` is
left on `report` for the same reason: it is the loop that reports honest
jitter, and a flash write stalling the machine is exactly what it exists to
show rather than be stopped by.

A program whose late output is wrong output says `fault`, and should say it
mandatorily: a system that does not understand the tag would otherwise run
it under the lenient policy, which is the contract it was written to refuse.
A value above `fault` is refused at admission rather than guessed at.

### In R9's order

R9 §15.1 requires three things in a fixed order — actuators safe, then the
fault published, then never released again — and says the order is a
requirement, because an observer reacting to a fault may command something.

`finish()` is the one place a process ends on its own task, and it is that
order: the release source goes first, so there is no next activation; the
exit hook second, which parks the devices; and only then are the status,
the fault and the state written. The process table's `fault` field — what
was `reserved` in `rv9_sys_proc_t`, same size — cannot be read before the
pin is parked, because it has not been written. The log shows it in order:

```
W (2768) rv9-io: failsafe: /gpio/2 left at 0
E (2769) rv9-proc: pid 3 ('lateloop') missed its 2000 us deadline: answered
             in 5014, 0 releases skipped; stopped and not released again
```

Collecting a killed thread follows the same rule, which meant moving its
fault code: it used to be written when the corpse was claimed, before the
failsafes ran.

### Demonstrated

`lateloop` drives `/gpio/2` high, declares `on_deadline=fault` with a 2 ms
deadline, and on its 50th period does 5 ms of work:

```
rv9> rt lateloop
rt: started lateloop as pid 13
rt: lateloop missed its deadline: failsafes applied, stopped, not released again
rt returned 1
rv9> pin 2
/gpio/2 = 0
rv9> procs
pid par name        state   base eff ended
...
13  12  lateloop    exited  15   15  DEADLINE
12  11  rt          exited  8    8   status 1
...
2   0   lateloop    exited  15   15  killed
1   0   deaf        exited  8    8   killed
```

And a real-time loop killed from the shell while it owns the pin — `ontime`
makes `lateloop` never late, so something else has to stop it:

```
rv9> rt lateloop ontime &
[12] rt
rt: started lateloop as pid 13
rv9> pin 2
/gpio/2: owned by another process (see 'owns')
rv9> kill -f 13
pid 13 killed
rt: lateloop was killed between activations
rv9> pin 2
/gpio/2 = 0
rv9> kill 13
kill: no process 13
```

The line after `lateloop`'s loop — which says RV-9 let a late loop carry on
— never printed. `procs` has a new column for how a process *ended*, which
on a machine that moves is the one that matters: returning, being killed and
missing a deadline are three different stories about the same actuator.

The first draft of `rt` passed its child's `-RV9_PE_DEADLINE` up as its own
status, and the shell dutifully printed `rt: missed its deadline and was
stopped` under the line saying which program had. `rt` now reports and
returns 1.

### Tested

`fault-test`, 28 checks at boot, on real modules judged from outside:

- a thread holding an `rv9_lock` and asleep is **not** stopped, is still
  alive, and is stopped once it lets go; the lock is then free (had the
  refusal been wrong, the boot would stop at that line);
- `deaf` is asked to stop and does not, is killed, reads
  `-RV9_PROC_ERR_KILLED` and `killed` in the table, and a second kill or a
  signal to it finds nothing;
- `lateloop` killed between activations ends within 50 ms of being asked,
  gives its release slot back, and has its pin parked;
- `lateloop` late on period 20 ends by itself with `DEADLINE`, pin 0, slot
  gone.

The pin is driven to 1 by the test before each, and read back as 1, so 0
afterwards is RV-9's doing and not the pin's resting state.

### What this does not do

**A real-time loop that never reaches `rt_wait`** is not stopped by
anything in this section. Deadlines here are detected at completion and at
release, and a loop spinning in its body does neither — while starving,
on one core, the shell that would type `kill`. §30 is about that loop.

**The fault is published in the process table, not in the component's
cell.** R9's `watch MOTOR_CONTROL.faulted` expects it where the component's
values are. PFM already knows which cells a process was writing (§28), so
marking them faulted when it dies is small; it is not done.

**A process killed partway through an open** — `/n0` waiting for a
connection, say — is not leaked: `rv9_io_open` puts the path in the
process's table *before* it lets go of the lock to wait, so the exit hook
finds and releases it. But releasing it asks the file manager to close a
path whose open never finished, and no file manager has been checked for
what it does with one. That is a question for each of them, not yet asked.

**Anyone may kill anyone.** `RV9_MTAG_CAPABILITY` still has no consumer.

**Only under RV-9's own kernel.** The FreeRTOS backend answers
`RV9_ERR_UNSUPPORTED` for stopping a thread: it cannot say whether a task
holds a lock, and guessing yes is how a system deadlocks while recovering
from something else. Real-time processes stop on either.

## 30. The loop that never comes back

Everything in §29 acts when a real-time task comes back to `rt_wait`. The
worst way a control loop can fail is to not come back: stuck in a loop in
its body, with the one call where RV-9 could stop it never reached. At the
real-time priority on a single core that loop also takes the machine —
the radio, the shell, and the `kill` somebody would type into it.

So a task that does not come back is now found from outside, and stopped
from outside.

### Found by a watchdog

A timer interrupt every 2 ms looks at each task that is inside an
activation — released, and not yet back to wait — and flags it for one of
two things:

- **past its deadline**, when it declared a miss fatal. A deadline that has
  passed with the work unfinished is a miss *now*, not when the work
  finishes, which for a loop that has stopped is never;
- **runaway**: in one activation for more than 250 ms, *and* found on the
  CPU in at least half the watchdog's samples over that time. The second
  condition is what tells a loop that spins from one blocked in a slow
  write — which is slow, not broken, and is left alone.

Runaway applies to every real-time task, whatever it declared. `report`
means a late period is counted and the loop carries on. It is not leave to
stop waiting and take the machine.

A flagged task that does come back to wait ends there, as in §29. One that
does not is handed to a supervisor task, which runs at the real-time
priority so that time slicing gives it a tick beside the runaway rather
than leaving it waiting behind the very thing it exists to stop.

### Stopped only where it holds nothing

§29's whole difficulty was that a task stopped while holding a lock takes
the lock with it. A real-time task is a host task, and nothing above the
KAL can see what a host task holds — which is why §29 only ever stopped
one between activations.

But an RV-9 module has no libc, no globals, and no callbacks from the
system: everything it can reach arrives through `env`, and every one of
those calls returns before the module's next instruction. So **a task whose
program counter is inside its own module's image is running only its own
code, and holds nothing at all.** Not a lock, not a mutex, not the heap, not
half of a publication. That is a property of what a module is, and it is
checkable from outside.

The program counter of a task that is not running is on its stack. On this
port every switch, voluntary or not, goes through the interrupt entry, which
leaves a full frame at `pxTopOfStack` — the first word of the task's
control block — with `mepc` first. `rv9_rt_seize` holds off the scheduler,
reads it, and if it is inside the module's image suspends the task. The
caller then owns it, and the process manager runs §29's `finish()` from
outside, in R9's order: releases gone, failsafes applied, then the table.
The task is deleted before its module is unlinked, since its program
counter is in that module.

A task anywhere else — the I/O manager, a driver, the allocator — may be
holding something, and is not suspended. The supervisor asks again every
tick. A loop spinning through system calls is inside the system most of the
time and in its own code some of it, and is caught at one of those
instants; on the board it took about 8 ms.

### Lowered, but not always

A runaway that cannot be seized has to be lowered, or it keeps the machine
while the supervisor waits for an instant to catch it. The first version
lowered every flagged task it could not seize, and the boot log showed what
that costs:

```
W rv9-proc: pid 3 ('lateloop') is past its deadline and still running, inside a
            system call: lowered, and stopped when it is back in its own code
W rv9-io: failsafe: /gpio/2 left at 0
```

`lateloop`, 3 ms past a 2 ms deadline, caught in the middle of reading the
clock, dropped to idle priority — and finished its last milliseconds, and
reached its failsafe, whenever nothing else wanted the CPU. A loop flagged
for its deadline is usually a few instructions from coming to wait and
ending itself there. Lowering it delays exactly that, and the safe state
with it.

So only a runaway is lowered — or a loop flagged for its deadline that is
*still* not caught a runaway's worth of time later, which is a runaway
whatever it was flagged for.

### The panic that was the point about IRAM

The first boot with the watchdog in it passed four of its tests and
panicked in the fifth:

```
Guru Meditation Error: Core  0 panic'ed (Cache error).
MEPC    : 0x420cdfca  RA      : 0x408047a8
0x420cdfca: xTaskGetCurrentTaskHandle at tasks.c:4987
0x408047a8: watch_isr at kal_rt.c:470
```

The watchdog interrupt runs while the flash cache is off — that is what an
interrupt marked resident is for — and WiFi wrote its calibration to NVS
during the test. `xTaskGetCurrentTaskHandle` is linked into flash. Every
other call in `watch_isr` was then checked against the linked image, and
it was the only one; the handler now reads `pxCurrentTCBs[0]`, which is what
the port's own interrupt entry reads, and which is in RAM.

It passed on the boot after the panic, which is the unsettling part. A
failure that needs a flash write to land inside a 2 ms window during a
half-second test shows up on some boots and not others. Two consecutive
clean boots were required before calling it fixed, and the symbol check is
the actual evidence.

### A number that was wrong on the way

With the watchdog flagging first, `rt_wait` returned the flag before
recording the activation that had just ended — so the fault report carried
the *previous* activation's response: `answered in 13`, about a loop that
had just spent five milliseconds. The flag is now returned after the record
is made, and the same case reads `answered in 5013`.

### Demonstrated

`runaway` runs on time for 20 periods and then never waits again, holding
`/gpio/2` high with a failsafe of 0. It declares nothing about deadlines.

```
rv9> rt runaway
rt: started runaway as pid 16
rt: runaway stopped waiting for its releases: stopped from outside, failsafes applied
rv9> pin 2
/gpio/2 = 0
rv9> rt runaway syscalls
rt: started runaway as pid 19
rt: runaway stopped waiting for its releases: stopped from outside, failsafes applied
rv9> procs
pid par name        state   base eff ended
19  18  runaway     exited  15   15  RUNAWAY
16  15  runaway     exited  15   15  RUNAWAY
```

Each took about half a second from the command: 200 ms on time, 250 ms
running away, the rest the shell. The session they were typed into is an
ordinary process at priority 8, running over WiFi, and survived both.

### Tested

Three more cases in `fault-test`, each judged on the pin, the status, the
table, the release slot, and the time from fork to stopped:

| case | reason | stopped after fork |
|---|---|---|
| `lateloop spin` — never finishes period 50 | `DEADLINE` | 503–505 ms (500 on time) |
| `runaway` — spins in its own code | `RUNAWAY` | 453 ms (200 + 250) |
| `runaway syscalls` — spins through `getstat` | `RUNAWAY` | 458–463 ms |

49 checks in all, on two consecutive boots.

### What this does not do

**A task stuck in firmware for good is lowered, never stopped.** A driver
that never returns holds the task outside its own code indefinitely, and
nothing here will suspend it there. Its failsafe waits with it.

**`kill` does not use this.** It stops a real-time process at its next wait
(§29). One blocked in a system call — not past a fatal deadline and not
using the CPU — is not stoppable by `kill` until the call returns.

**A declared WCET is still a claim, not a budget.** What is enforced is the
deadline, and the runaway limit. An activation that runs past its WCET but
finishes in time is not even counted. Counting it would be cheap; stopping
for it would need a reason beyond the one number.

**The runaway limit is one figure for every task,** 250 ms, not derived
from anything the task declared.

**The seizure depends on the port's frame layout.** It is confined to one
function in the KAL and commented as such, but a change to how ESP-IDF's
RISC-V port saves context would need it checked again.

**The watchdog costs a 500 Hz interrupt** from the first real-time
declaration onward, whether or not anything is running.

## 31. A publication with a contract

§28 built publications and left two things open. Cells were named by
convention — two components agreed on `CONTROL` by both spelling it that
way, with nothing checking and nothing stopping a third program writing
into it. And when a component stopped badly, §29 and §30 recorded the
reason in the process table, which is not where anything watching the
component is looking.

Both are the same missing idea: a publication is a contract between two
programs, and neither end had a way to state it.

### Declared, not conventional

Two manifest tags, both full paths like every other resource:

```
publishes="/pub0/CONTROL"      # control's build.conf
watches="/pub0/CONTROL"        # a supervisor's
```

`publishes` reserves the cell at fork, making it if it does not exist. The
reservation belongs to that process for its lifetime, and while it stands:
a second program declaring the same cell is refused at fork
(`RV9_PROC_ERR_BUSY`), and any other process opening it to write is refused
at open. RV-9 already had one-writer-at-a-time; what it did not have was
*which* writer, decided before anybody runs.

`watches` is refused at fork when nothing on this machine provides the
cell: `RV9_PE_NOPUB`. "Provides" means either the cell exists now, or some
module in the directory declares that it publishes it — which is the
decidable form of "will never publish". A publisher that has not started
yet is not a refusal: start order is not a contract, and a supervisor
started before its control loop is ordinary.

That check reads manifests out of the store for modules that are not in
memory, at most a kilobyte each, with the walk bounded by patching the
copied header's length to however much was actually read. It costs tens of
milliseconds across a full store, which is why only admission does it, and
only for a program that declares a dependency on another program.

### The fault, published where the values are

R9 §15.1's second step is that a faulted component *publishes* that it has
faulted. The process table is published state, but `watch MOTOR_CONTROL`
does not read the process table — it reads the cell. So the cell now
carries the reason:

- `rv9_pub_info_t.fault` is `RV9_FAULT_*`, in bytes that used to be
  `reserved`, so the record kept its size;
- the fault is itself a publication: the sequence advances by one, so a
  watcher blocked in `RV9_PUB_GS_WAIT` wakes, and one that had seen the
  last value is told something changed;
- **the value and its stamp are left exactly as the component published
  them.** The last thing a failed control loop measured is the most useful
  thing it leaves behind, and a fault that erased it would destroy the
  evidence;
- opening the cell to write again clears it. That is the component, or its
  replacement, back in service;
- the reason is R9's, not RV-9's. A runaway (§30) is published as
  `DEADLINE` — R9 decided a component that stops coming back to wait has
  missed its deadline, and needs no word of its own — while the process
  table and the log keep `RUNAWAY` for whoever is working out how.

The ordering R9 requires is why this needed a second hook rather than the
existing exit hook: the exit hook runs *before* the process table knows the
reason — it is what applies the failsafes. So the process manager now tells
the I/O manager once more, after the table is written, on every path a
process ends by. Failsafe, then table, then cell.

### What a cell says now

```
rv9> rt lateloop ontime &
rt: started lateloop as pid 58
rv9> watch LATELOOP 300 &
seq    age_ms values...
270    0      269
271    0      270
rv9> kill -f 58
W rv9-io: failsafe: /gpio/2 left at 0
W rv9-proc: pid 58 ('lateloop') killed between activations
W rv9-pfm: /pub0/LATELOOP: its publisher, pid 58, stopped (killed); the cell
           says so and keeps its last value
  publisher stopped: killed (the value above was its last)
rv9> pubs
name                 seq   bytes   cap  age_ms  by   rdrs  torn  note
LATELOOP             272   4       64   3002    -    1     0     killed
```

The three log lines are in R9's order, and the watcher learned from the
cell rather than from the log. `pubs` gains a note: the fault if there is
one, otherwise who declared the cell.

### Tested

`fault-test` is 66 checks. The new ones, on real modules:

- `lateloop`'s cell is reserved for it at fork and published into;
- killed, its cell says `killed` and is no longer reserved;
- late, its cell says `DEADLINE`, still holds the last period number it
  published, and its sequence is exactly one past that value — 21
  publications, then the fault;
- a watcher that had seen the last value is told something changed;
- opening the cell to write again clears the fault;
- `control` reserves `/pub0/CONTROL`; nothing else may write it; a second
  `control` is refused at fork;
- with the cell removed entirely, a program watching `/pub0/CONTROL` is
  still admitted, because control's manifest declares it — the store scan;
- one watching a cell nothing provides is refused, and admitted once a
  module that publishes it is on the machine.

The last three fork modules built in memory by the test: what is under test
is admission, and the alternative is modules in the store that exist only
to be refused.

### What the tests exposed, which is not about publication

Driving this from a shell over SSH, `procs` failed to start with "no
memory", and `sshd` exited. Measured afterwards: ten forks cost about 2.5
KB that never comes back, some 250 bytes each, which is the process
descriptor RV-9 keeps for every process that has ever exited. It is kept so
that a parent can still ask how its child ended, and nothing ever decides
that nobody will ask again.

So the process table grows for as long as the machine runs. On a machine
meant to run for months that is the wrong shape, and it is not a leak in
the ordinary sense — it is a lifetime nobody has defined. Deciding when a
process may be forgotten is the next thing worth doing here.

### What this does not do

**A cell's size is not declared.** Capacity is a property of the device (64
bytes on `/pub0`), so a component publishing a larger set fails at write
rather than at admission.

**`watches` reserves nothing.** There are four waiter slots on the device,
handed out when an observer first blocks; a fifth watcher is refused then
rather than at fork.

**"Provides" is a question about the machine, not about the running
system.** A cell declared by a module nobody ever starts admits its
watchers, which then wait forever — correctly, because that is what a
supervisor does while its component is down.

**The fault's publication moves the sequence without changing the value.**
A reader that only compares sequences sees "something changed" and reads
the same numbers. That is deliberate: the change is the component's state,
not its value, and `RV9_PUB_GS_INFO` says which.

## 32. A machine that stays up

Everything before this section was about a machine doing the right thing.
This one is about a machine still being there to do it after a week —
nobody beside it, nothing rebooting it, every way in being the network.

It began with a finding in §31: driving the board over SSH, `procs` failed
to start with "no memory" and `sshd` exited. That turned out to be five
separate defects, each of which degrades a long-running machine rather
than crashing it, which is what made them easy to miss.

### 1. The process table never shrank

Every process that had ever exited kept its descriptor — about 250 bytes,
for as long as the machine was up — so that a parent could still ask how it
ended. Nothing ever decided nobody would ask. Ten forks cost 2.5 KB that
never came back.

Exited processes now live in a bounded history, forgotten in the order that
costs least:

- beyond **16**, those whose status has been collected, or whose parent has
  gone and never will collect it — oldest first;
- beyond **32**, any — so a background job nobody waits on is remembered
  for a while, not for ever.

Nothing is forgotten while anybody holds it. A descriptor now carries a
reference count, held by the process's own task while it runs and by
anything that keeps a pointer to it across a lock release: a waiter, `kill`,
the stack-fault collector, the watchdog's handler. The public calls that
returned raw descriptor pointers are gone in favour of copies,
`rv9_proc_info` and `rv9_proc_list`, because a pointer handed out yesterday
may name freed memory today.

On the board, after forty forks: **17** exited processes remembered, and
forty more forks cost **0 bytes**. With forty uncollected children, **33**
remembered, the oldest forgotten, and waiting on it returns `NOTFOUND`
rather than hanging.

### 2. Pids were a sixteen-bit counter

65,535 forks is eighteen hours for a machine running one job a second. Once
descriptors can be forgotten, a wrapped counter would eventually hand out a
number that still names a remembered process, and a `wait` or `kill` would
land on the wrong one. Allocation now skips any pid in use, running or
remembered, and never issues 0. Tested by moving the counter to the edge:
65534, 65535, then 1.

The same reuse reached PFM: a cell remembers the process that last wrote
it, so that a fault can be published into it (§31). A later process given
the same number could have marked an old cell with its own fault. The cell
now forgets its writer once that writer's end has been processed — which
needed the end reported to file managers exactly once, fault and all, so it
is: after the table, on every path out including a refused fork.

### 3. A background job kept the session

`/ssh0` is a device with a session: one client, established on first open.
The I/O manager refused a second open while any path to it existed, which
was right for a second `sshd` and wrong for this: `deaf &` typed over SSH
inherits the terminal, and after `exit` it still holds a path. So every
later login was refused — and `sshd` read that refusal as another `sshd`,
and retired for good.

A session now ends when the program that established it says so:

- `RV9_SS_HANGUP`, a setstat the I/O manager answers itself. `sshd` issues
  it when its shell ends;
- the device's session number moves on. A path remembers the session it was
  opened in, and one from an ended session gets `RV9_IOE_IO` without
  reaching the driver;
- **the job holding it keeps running.** A control loop started over a
  wireless link must not stop because the link did.

The hard part is a job that is *inside* the driver when the session ends —
writing to the terminal, or blocked reading it. Freeing the session under
it would be a use-after-free. So calls into a session device are counted
(`busy`), in the order that makes the count trustworthy: increment, then
check the session, so a hangup that moves the session on and then reads the
count sees every call that could still reach the driver. With nothing
inside, the session closes normally, goodbye to the client and all. With
something inside, the driver's new `hangup` shuts the socket — without
freeing it — so a blocked read returns, and the session is freed only once
`busy` reaches zero. If it never does, the session is abandoned rather than
freed under a caller, and the log says so.

On the board, a session left with `deaf talk &` running exits, and the next
login gets a prompt; `procs` in it shows the talker still alive.

### 4. Killing a process mid-open lost what the open had built

The first test of supervision killed `sshd` while it waited in `accept()`,
and every `sshd` after it failed with:

```
W rv9-nfm: cannot listen on 22: errno 112
```

Address in use. The listening socket had lived in a local variable of the
open, on the killed thread's stack, and was never closed; the session
structure, twelve kilobytes, went with it. §29 had flagged "killed partway
through an open" as unexamined. This was the examination, and the answer
was worse than a leak: SSH was gone until reboot.

§29's rule was that a thread holding a lock is not stopped. An open that
is waiting holds something too — state only it can take down — so it now
counts as holding: `rv9_task_hold` around the blocking part of every open.
And because such an open may wait forever, a refused kill also marks the
thread cancelled. NFM's accept, connect, read and write loops check that
and give up, closing their socket on the way out; the open returns, the
hold is released, and the retried kill lands. A kill that gives up clears
the mark, so a process left alive does not find its own waits failing.

Tested with `deaf accept`, which waits on a port inside an open: killed in
**2 ms**, a second copy then listens on the same port, and the heap is within
half a kilobyte of where it started.

### 5. Nothing restarted the way in

The console shell was always restarted when it ended. The two network
shells were started once. On a machine nobody is beside, that is exactly
backwards — and `sshd` had just shown two separate ways to die.

`init` now supervises all three alike: notices when one ends, restarts it
after a pause that doubles while it keeps failing (1 s up to 60 s) and
resets once it has stayed up for a minute. `sshd` no longer exits for a
missing password either; it says so once and waits, so setting one with
`passwd` brings SSH up without a reboot.

```
rv9> kill -f 7
W rv9-proc: pid 7 ('sshd') killed
W rv9: sshd (pid 7) ended with -12; starting it again in 1000 ms
```

and the next SSH login gets a prompt.

### And then the memory itself

With all of that fixed, one SSH session with one background job still left
too little heap to fork `procs`. Not a leak — three login cycles returned the
heap to within eight bytes — but a machine with 29 KB free at idle, and a
session costing 10 KB of it. `stacks` said where:

| | given | used |
|---|---|---|
| `sshd` (after handshakes) | 8192 | 2512 |
| `shell` (serving a session) | 8192 | 1400 |
| `rshd` | 8192 | 1148 |

All three had the 8 KB default, chosen long ago because interrupt frames
land on whatever stack is current. The high-water marks above were taken
with WiFi busy and so include them. They are now declared at 4096, 4096 and
3072 — roughly three times what was used — and an overflow is still caught
by the guard, and now also restarted by `init`.

Idle heap went from **29.1 KB to 44.1 KB**. The same session with a
background job, `rt control`, `pubs` and `stacks` in it forked everything
it was asked to, with nothing refused, and the heap came back to 43.6 KB
when it was over.

### Tested

`proc-test`, 11 checks: forty forks leave the history bounded and forty more
cost nothing; uncollected children are kept longer, then forgotten, and a
forgotten pid says so; pids survive the wrap. `fault-test` grows to 73 with
the open that was killed while waiting. The hangup, supervision and memory
figures above are from the board over SSH and serial, not from a test at
boot, because they need a client on the far end.

### What this does not do

**Hangup is only for session devices.** `rshd`'s connections are ordinary
sockets; a background job keeps one open until it exits, costing a socket
but not blocking the next login.

**A background job reading a terminal still competes with the shell for
it.** `deaf listen &` swallowed the lines typed after it, including `exit`.
That is what an interactive terminal with two readers does anywhere; RV-9
has no foreground process group to arbitrate it.

**Cancellation reaches the network loops only.** A process killed inside a
different blocking open — a driver that waits on hardware with no end — is
refused for as long as it waits, and the kill reports a timeout rather
than stopping it.

**The history sizes and the stack sizes are measured figures, not derived
ones.** 16 and 32 are policy; 4096 and 3072 are three times this week's
high-water. A stack that grows with new features needs measuring again,
and `stacks` is where to look.

**One login in one test run showed no prompt** and the next two did. It did
not recur in any later run; it is recorded rather than explained.

## 33. Which loop goes first

§29 made a missed deadline able to stop a loop. That made an old
simplification dangerous: every real-time task ran at one host priority,
and tasks sharing a priority take turns a scheduler tick at a time. So a
loop that was late because another loop was using the CPU was stopped as
though the lateness were its own.

R9 §13 had already said what should decide it: *RV-9 should derive
scheduling priority from the complete admitted workload and its periods,
deadlines, minimum intervals, and execution bounds. A faster period alone
is not always enough.* This does that.

### The false fault, and how often it happens

The first pair built to show it: a fast loop with 500 µs to answer each
5 ms release, a miss declared fatal, beside a slow loop spending 20 ms of
every 100 working. At one priority:

```
E rv9-proc: pid 5 ('fastloop') missed its 500 us deadline: answered in 1072,
            0 releases skipped; stopped and not released again
```

On a machine that moves, that is actuators parked because of a scheduling
decision nobody made on purpose.

It was also not what happened every time, and the test built on it failed
to fail on one boot in four. The explanation assumed the fast loop always
waited for the next scheduler tick. This host wakes a task of equal
priority at once most of the time, so the fault needs the less usual case
where the slow loop runs first. A second pair was built to make that case
common — the fast loop now does 2 ms of work in every 5, due in 3 — and
still passed at one priority on two boots of two, with worst jitter of 812
and 327 µs where the derived placement gave 14 and 12. On the next boot it
was stopped.

So the honest statement is: at one priority, a loop can be made to wait
for another loop's work, which makes its response worse every time and
occasionally fatal. The test checks the first, which is reliable, and
counts the second as an instance of it.

### Two priorities, because that is what there is

The first thing was to find out how many priorities there are to give.
The radio's priority is set inside a binary library, so the board was
asked:

```
I rv9-rt: host task wifi         priority 23
I rv9-rt: host task esp_timer    priority 22
I rv9-rt: host task sys_evt      priority 20
I rv9-rt: host task rv9-kernel   priority 22
I rv9-rt: host task rv9-rtwatch  priority 24
```

Exactly one host priority is above the radio. A ranking of real-time tasks
finer than that would put all but the top of it underneath WiFi anyway,
which is not a ranking worth pretending to have. So there are two:

- **urgent**, 24: above everything the host runs;
- **routine**, 21: above RV-9's kernel and every ordinary process, below the
  radio and the host's timer task.

RV-9's kernel host task had been at 22, above where routine now sits; a
shell that outranks a control loop is not a real-time system. It moved to
19 — still above the network stack, now below the host's event task,
whose callbacks are short. Measured afterwards over SSH, with the heavy
loop using a fifth of the CPU: every shell command answered in under a
tenth of a second.

### Placement, from response-time analysis

Admission now places the whole real-time workload, not only the task
being admitted:

1. everything starts urgent. A set whose urgent tasks all meet their
   deadlines together stays there — every workload of one loop does;
2. while some urgent task cannot, the least urgent (the longest deadline)
   moves to routine, where the urgent tasks no longer wait for it;
3. then every routine task must meet its deadline too, charged for all the
   urgent tasks and all its routine peers.

"Meets its deadline" is response-time analysis: R = C + Σ ⌈R/Tⱼ⌉·Cⱼ over the
tasks that can run ahead of it, iterated until it stops moving or passes
the deadline. C is the declared worst-case execution, or the worst measured
when nothing was declared — a floor, as the load report has always said.
Tasks sharing a priority are each charged for all of the others, because
that is what time slicing does to them. A task with no release bound (an
event source with no minimum interval) cannot be analysed: it stays urgent
and is counted as unaccounted, as before.

If a running loop's placement changes, it is moved while it runs:

```
I rv9-proc: pid 2 ('heavyloop') now runs routine, bound 38000 us
I rv9-proc: admit 'fastloop': urgent, response bound 2500 us against 3000
fastloop: 400 on time, worst jitter 14 us
```

(`heavyloop` does 15 ms of work declared as 18; `fastloop` 2 ms declared as
2.5. The heavy loop's bound is its own 18 ms plus the fast loop's 2.5 ms
for every 5 ms it is kept waiting: 38 ms, against a 100 ms deadline.)

A task the watchdog has flagged is not moved: it was lowered on purpose
and is about to be stopped (§30).

### A refusal utilisation could not make

If no placement meets every deadline, the fork is refused with
`RV9_PE_UNSCHEDULABLE`, naming who would be late. The 70% ceiling is still
there, for the host's work that is in neither sum, but it no longer stands
in for a schedulability test, which it never was:

```
E rv9-proc: admit 'st-rt-tight': with it admitted, pid 6 ('fastloop') would
            answer in 3100 us against a 3000 us deadline, however the
            real-time work is placed
I rv9-proc: admit 'st-rt-fits': routine, response bound 3100 us against 6000
```

Both of those use 6% of the CPU, beside a fast loop using 50%. One has a
2.8 ms deadline, and whichever of the two goes first makes the other wait
too long: 2500 + 600 does not fit in 3000, nor 600 + 2500 in 2800. The
other has 6 ms and fits underneath, with a bound of 3100 µs computed
exactly as by hand.

### Seen from the shell

`rt` with no arguments now lists each loop with its placement, the bound
admission worked out, and what it has actually done:

```
rv9> rt
real-time promised  29.0% of 70.0%
slots               2 of 4

slot  period  deadline  runs     bound  worst  misses
0     100000  100000    routine  26200  20015  0
1     5000    500       urgent   200    117    0
```

The bound and the worst side by side is the point: a declaration that
measurement keeps approaching is a declaration to look at. (That table was
captured with the first version of the pair — 100 µs of fast work, 20 ms
of heavy — and the figures are from it.)

### Tested

`sched-test`, 11 checks. The pair with priority derived: the heavy loop is
moved below the fast one when the fast one is admitted, and the fast loop
meets every deadline. The same pair with the derivation switched off, as a
control: the fast loop's worst response, read from the live statistics
near the end of its run, must be later than it was when placed — and a
fault counts as later. Without the control the first result would prove
nothing; with a control that only sometimes fails, as the first version
had, it proves nothing either. Then admission, with modules built in
memory: the tight candidate refused, the loose one admitted at routine with
a bound of 3100 µs, and the running fast loop undisturbed.

### What this does not do

**Routine bounds exclude the host.** The radio and the timer task run above
routine work and declare nothing, so a routine loop's bound is a bound on
RV-9's workload only. Deadlines long enough to be placed routine are
normally long enough not to notice; nothing proves it.

**Blocking is not in the analysis.** A loop waiting on a lock held by a
lower-priority task is covered by the lock's priority inheritance, not by
the arithmetic.

**Placement is revised on admission, not on exit.** A loop moved to routine
stays there after the loop that caused it has gone. That is always safe —
nothing becomes less schedulable — but not always as good as it could be.

**Admission is not atomic across simultaneous forks.** Two real-time
programs admitted at the same instant are each checked against the other's
absence, as the utilisation check always was.

**The explicit `priority` escape hatch in R9 §13 was not implemented here.**
It is now, as a declared placement; see §36.

## 34. What a compiler may rely on

R9's design names three contracts between the language and the system.
The second — the manifest, compiler to RV-9 — has existed since §24, and
the third — measured reality, RV-9 back to the compiler — is everything
`procs`, `rt`, `pubs` and the fault cells report. The first had never been
built: *the RV-9 toolchain supplies a machine-readable target profile
describing its ABI, module format, supported manifest entries, execution
classes, and operations known to be real-time safe.*

Alignment note 7 had been marked "documented in prose, `RV9_RT_CODE` in
source; not readable" since it was written. A compiler cannot read prose,
and should not be asked to read source.

### Two halves, because two different things know the answers

What every RV-9 build offers is known to its sources. What *this board*
offers — which devices, where its radio sits, how many real-time slots — is
known only to the board. So there are two profiles:

- `docs/target/rv9-profile.json`, generated from the sources by
  `tools/mkprofile.py` and committed beside them;
- `profile`, a command that prints the board's half as JSON, from two new
  sysinfo records, `RV9_SYS_DEVICES` and `RV9_SYS_LIMITS`.

### Derived, not written

A profile maintained by hand is wrong the first time somebody forgets it,
and the thing reading it is a compiler that will believe it. So nothing in
the static profile is typed in:

- **Manifest tags** come from `tools/mkmodule.py`, the producer, and are
  checked against `module.h`, the consumer. A tag either side lacks is an
  error. Each tag carries its number, encoding, whether it repeats, and
  its value names.
- **"Enforced"** is true only where some component source outside the header
  refers to the tag. Six are registered with nothing in the firmware behind
  them — `desc`, `static`, `class`, `capability`, `compiler`, `runtime` —
  and the profile says so rather than letting a compiler assume they mean
  something. (`desc` is read, but only by `tools/modinfo.py` on the host.)
  The first draft counted boot tests as consumers, and `desc` came out
  "enforced"; tests are no longer counted.
- **Calls** are every field of `rv9_mod_env_t` in order, with the ABI version
  that added it, parsed from the struct's own `--- ABI n ---` markers.
- **Real-time safety** is read off `RV9_RT_CODE` on the function that
  actually implements each call, found by following the assignments in
  `rv9_mod_env_init` and the process manager's overrides. `rt_wait` and
  `time_us` are `yes`; `read` and `write` are `device`, bounded when the
  device's file manager and driver are; everything else is `no`. File
  managers and drivers are read the same way, from their registration
  tables: `pio` and `pfm` are resident, `scf`, `rbf` and `nfm` are not, and
  of the drivers only `gpio` is.
- **Faults** each carry their R9 name, and a fault RV-9 adds without one
  stops the generator. That mapping is a language decision; the script
  refuses to make it by default.
- **Limits** — slots, the runaway threshold, the watchdog period, the
  utilisation ceiling, the process history — are read from the `#define`s
  that set them.

The attribute is the claim, and the profile says how far it goes: what an
entry function calls is its implementer's responsibility. `pfm_write` is
resident, and deliberately calls a wake-up that is not.

`--check` regenerates in memory and fails if the committed file differs.
It runs with the host tests, so a change to the ABI that forgets the
profile fails there rather than in a compiler.

### The board's half

```
rv9> profile
{
  "profile": "rv9-board",
  "module_abi": 13,
  "realtime": {
    "slots": 4,
    "utilisation_ceiling_permille": 700,
    "runaway_ms": 250,
    "watchdog_us": 2000,
    "host_priority": { "urgent": 24, "routine": 21, "radio": 23, "kernel": 19 }
  },
  "memory": { "heap_floor": 12288 },
  "processes": { "history": 16, "history_max": 32, "max_paths": 8 },
  "devices": [
    { "name": "/pub0", "filemgr": "pfm", "driver": "pubmem", "retains": true, "sessions": false },
    { "name": "/gpio", "filemgr": "pio", "driver": "gpio", "retains": true, "sessions": false },
    ...
  ]
}
```

The radio and kernel priorities are asked of the running host, not
remembered from §33. A toolchain captures it the way anything is captured
from RV-9: `printf 'profile\nexit\n' | ssh board`, or `profile > /r0/…`.

### RUNAWAY, decided

R9 settled the question §30 left open: a component that stops coming back
to wait has missed its deadline, and the language needs no word of its
own for it. PFM publishes a runaway into its cell as `DEADLINE`; the
process table and the log keep `RUNAWAY`; the profile records the mapping
in `faults`. `fault-test` checks both sides — table `RUNAWAY`, cell
`DEADLINE` — for both runaway cases, and 76 checks pass.

### What this does not do

**The real-time claim is one level deep.** It is the attribute on the entry
function, not an analysis of what that function reaches.

**Device-level latency is not in it.** R9 lists "machine-readable device
latency and ownership metadata" among its open items; the board profile
says what each device is built from, not how long it takes.

**Status and setting codes are not described per device class.** A compiler
learns that `getstat` exists and is not real-time safe, not which codes a
`pio` device answers.

**The board's half travels as text over a shell.** There is no structured
query protocol, and `format` is 1 because the shape of both files is
expected to change as the compiler starts reading them.

## 35. When memory runs out

Until now RV-9 had one heap floor (§23): below 12 KB, every allocation is
refused, for everyone. That keeps the machine from dying of exhaustion,
but not the parts of it that matter most. A background job that spends
the heap down to the floor leaves a control loop unable to start, and in
the worst case leaves a dying loop's failsafe path with nothing to run on.
Before this section, `deaf &` five times over SSH made `kill` itself fail
with "no memory". A shell that cannot kill the thing that ate the memory
has no way out.

R9's contract is that real-time work is admitted against what the
machine can *guarantee* it. So memory now has classes and budgets.

### Three floors, not one

Every allocation is asked against the floor of the class of the thread
making it:

| class | floor | who |
|---|---|---|
| general | floor + 8 KB reserve | ordinary programs, shells, daemons |
| realtime | floor | real-time processes, and anyone forking one |
| system | floor / 2 | init, the failsafe path, unregistered host tasks |

RV-9 threads carry their class in the thread structure. Host tasks (wifi,
esp_timer) have no RV-9 thread, so `kal_mem.c` keeps a small registry for
them, and a host task nobody registered counts as system: the radio stack
cannot be told to fail, and refusing it gains nothing.

The class is set where the reason for it is known:

- `fork_rt` raises the caller to realtime for the fork. Otherwise a
  general shell asking for a control loop would be refused at the general
  floor, and the reserve would protect nothing.
- The real-time trampoline sets realtime for the process itself.
- `apply_failsafes` runs at system and restores the old class afterwards.
  Parking an actuator is the last thing that must still work.
- Init is system from before the module directory is built.

### Budgets

Modules cannot allocate. Everything a process costs is spent for it by
RV-9, and nearly all of that at fork: stack, statics, and descriptor. So
a process's footprint (stack + statics + descriptor + 96 bytes of
allocator headers) is known before it runs. The footprint is charged to
the process and to each of its nearest eight ancestors, which are tracked
by serial number rather than pid, so a reused pid is never charged for a
dead process's children. A fork that would take any of them over budget
is refused with `RV9_PE_BUDGET`, and the refusal names whose budget it
was.

Budgets nest. A shell's budget covers the commands it runs, and `sshd`'s
covers every session's shell and whatever that shell starts. The default
is 32 KB, enough for a shell to hold `ed` (17 KB with its statics) with
room to spare. A program that needs more says so in its manifest with a
new tag, `mem_max` (0x0013). `sshd` and `rshd` declare 40 KB. The charge
is returned when the process ends, not when it is collected, so a
remembered descriptor in the history does not hold budget.

`budgets` lists every live process's footprint, what it and its
descendants hold, and its budget. The profile (§34) gains
`processes.budget_default` and a `memory` section with the reserve.

### The shell keeps a way out

`procs` and `kill` are the two commands you need most when memory is gone,
so the shell has its own copies. The process table lives in the shell's
statics. If forking either command is refused for memory or budget, the
shell does the work itself. Over SSH, with background `deaf` jobs
started until one was refused, the lines that mattered were:

```
deaf: no memory to start it
(no memory to start procs; the shell's own)
(no memory to start kill; the shell's own) killed
```

and `budgets` showed `sshd` holding 13552 of its 40960. In that run the fifth `deaf` hit the general floor before it hit a budget.
General memory ran out first, and that is the case the fallbacks exist
for.

### Tested

`mem-test` runs last at boot and uses up memory on purpose (boot log,
13/13):

```
--- when ordinary memory is gone ---
(took 105 x 512 bytes; 8612 left above the floor)
  pass  and was refused with the real-time reserve still there
  pass  an ordinary program cannot start now
  pass  but a control loop is still admitted
(took 8 x 512 more; 2004 left above the floor)
  pass  the loop is stopped with every reserve above the floor gone
  pass  and its pin was still parked
(heap -40 bytes lower than before)
--- a program that forks without end ---
(started 4 children before the budget refused one)
(second run: heap 0 bytes lower)
mem: 13/13 passed
```

General work stopped with the 8 KB reserve intact. A control loop was
still admitted by an ordinary caller. A real-time hog then took the
reserve down to 2 KB above the floor, and a dying loop's pin was still
parked under the system floor. `forkbomb` (12 KB budget) was stopped at
four children both times, with nothing lost on the second run.

`heap_mark` logs the heap at each stage of boot so these costs can be
seen, not guessed: 92 KB free after bringup, 82 KB before services, and
48.5 KB (28 KB for programs) once the services are up.

### What it costs to use the machine

Measured over serial with an SSH session opened and closed:

| state | free | for programs |
|---|---|---|
| idle | 37912 | 17432 |
| session open, idle | 30828 | 10348 |
| `free` forked inside the session | 23896 | 3416 |
| session closed | 37548 | — |

Nothing leaks. A session costs about 7 KB, and a command forked inside it
about 7 KB more. That leaves **3.4 KB for programs inside a session with
one command running**. This is the most important thing this section
found. The protections work, but the margin they protect is thin on this
board. A second command in the same session is refused, and the shell
falls back to its own `procs` and `kill`.

### What this does not do

**Headroom is not tuned.** Many modules still use the 8 KB default stack
when they need far less (only the daemons have been right-sized, from
measured high-water marks), and each of those is 8 KB that could be 3. Right-sizing the rest is the cheapest
memory this board will ever get. Likewise, the 8 KB reserve and the 32 KB
default were chosen by reasoning, not measured against a real control
workload.

**Idle drifts.** Free memory at "services up" was 48.5 KB, but an idle
board some minutes later had 37.9 KB. Wifi buffers and lwIP state are the
likely cause, but that has not been shown.

**Budgets count what RV-9 spends at fork, not everything.** Buffers a
driver or file manager allocates on a process's behalf (a socket, an open
file's cache) are not charged to it. They are bounded by the floors, not
by the budget.

**Classes are per thread, not per allocation.** A general thread that
calls into a resident path allocates at its own class. Only the paths
named above change class.

**No compile-time check.** A compiler could total the footprint of a
component tree and check it against `budget_default` from the profile
before it ever reaches a board. The data to do that is now published, but
nothing uses it.

## 36. The escape hatch, kept honest

R9 §13 asks that priority be derived, and keeps an explicit `priority 10`
as an escape hatch. §33 left the hatch out. With two levels there is no
number to honour, and a hatch that lets a program choose its own
level defeats the analysis that makes admission mean something.

The answer R9 accepted is that the hatch should be a **constraint on the
placement, not an override of it**.

### `placement`

A new u8 manifest tag, `placement` (0x0014):

| value | meaning |
|---|---|
| `derived` (0) | the default: wherever §33's analysis puts it |
| `urgent` (1) | never below the radio |
| `routine` (2) | never ahead of it |

`place_rt` starts a routine-pinned task at routine instead of urgent,
and when it looks for a task to move down, it skips pinned ones.
Everything else is unchanged. Both levels are still analysed, every
bound is still computed, and if the pins leave some loop unable to meet
its deadline, the program is refused `UNSCHEDULABLE` as any other would
be. The log says so:

```
admit 'st-rt-pinu': with it admitted, pid 10 ('fastloop') would answer in
3100 us against a 3000 us deadline, however the real-time work is placed
around the declared placements
```

A value RV-9 does not know is refused `CONTRACT`, for the same reason an
unknown `on_deadline` is: running it under some other placement would be
agreeing to a contract nobody offered.

`urgent` is also the answer to R9's second open question from §33, whether
a component can say it must never run beneath the radio. It can now, as a
constraint rather than a number.

### What pinning can cost

A pin can make a workload unschedulable where the derivation would have
fitted it. `st-rt-fits` (10 ms, 6 ms deadline, 600 µs) was admitted at
routine beside `fastloop` in §33. The same loop pinned urgent is refused.
Above the fast loop it would push the fast loop to 3100 µs, and moving
the fast loop down instead makes it no better. That is the hatch working:
the program gets the level it asked for or it does not run, and nobody
else's deadline pays for it.

### Tested

`sched-test` now has 15 checks. The four new ones:
- A loop pinned routine runs routine even alone, where the derivation
  would put it at urgent.
- The pinned-urgent `st-rt-pinu` is refused beside `fastloop`.
- An unknown placement (7) is refused.
- The fast loop was not disturbed by any of it.

The target profile publishes the tag and its value names.

### What this does not do

**Placement is not shown in `rt`.** The table's level column comes from the
KAL's statistics, which know where a task runs but not why. The admission
log says `(declared)`.

**A pin does not bound the radio.** `urgent` keeps a loop out from under
the radio. It does not make routine bounds include the host's work, which
remains true of every routine loop as §33 says.

## 37. Stacks, sized by what they were seen to do

§35 ended on the number that mattered: inside an SSH session with one
command running, 3.4 KB was left for programs, and a second command was
refused. The cheapest memory on the board was stacks. Thirty-five modules
still took the 8 KB default because nobody had measured them, and
`stacks` could not measure them either: it sees only the living, and most
commands are gone within milliseconds.

### Measuring the ones that do not stay

A process's stack is scanned where nothing later can reach it: at the
end of `finish()`, on its own thread, after the exit hook, the failsafes
and the publication have run on that same stack. The figure goes on the
module's directory entry as a peak since boot. `RV9_SYS_STACK_PEAKS` (11)
returns one 22-byte record per module that has run, and `stacks` prints
them under the live table:

```
since boot       given   peak  runs
chart           3584   2144     4
control         2560   1368     3
echo            2560   1032    82
gauge           4096   2748     2
pic             3584   2096     4
rt              2560   1240     1
...
```

The record is small on purpose. `stacks` is a command you want to run
when memory is short, and 64 records cost it 1,408 bytes.

### The rule

A module's stack is its measured peak plus a kilobyte, rounded up to 512
bytes, with 2,048 as the floor. The margin exists because a peak covers
only the paths that were exercised. About 900 bytes of every peak is RV-9
ending the process, so no command's stack can be smaller than that
however little it does. Where a module's real work could not be
exercised safely, it keeps a larger stack and its `build.conf` says why:
`wifi` and `scan` (4 KB, ESP-IDF WiFi calls on the caller's stack),
`passwd` and `authkey` (3 KB, hashing and flash writes).

### What the first rule got wrong

The first pass measured every command with its usual arguments and cut
`gauge` to 2,560. The verification pass then stopped it:

```
gauge: started a 50 Hz control loop to watch
E (127794) rv9-proc: pid 106 ('gauge') killed: stack overflow
gauge: stopped by the scheduler (see the log)
```

On its first measurement `gauge` had not reached the work it does. Run
bare on an idle board it does two things that land on its own stack:

- **It admits a control loop.** `fork_rt` runs response-time analysis on
  the *forking* process's stack. From `rt` that path peaks at 1,240.
- **It draws.** `/w0` renders when the path is closed, on the writer's
  stack. `pic > /w0` peaks at 2,096 against about 1,000 for `pic` alone.

Measured doing both, `gauge` peaked at 2,748, and it now has 3,840. The
guard and pad from §22 did what they were built for: one process was
stopped, named in the log, and nothing else was touched.

Measuring it properly hit a second trap. Opening `/w0` allocates about
12 KB of buffers, so with `gauge` held at 8 KB for measurement the window
could not be opened, `gauge` returned early, and the peak described the
failure path. The measurement size had to be small enough for the real
path to run.

### Tested

After the final sizes, every command was run twice over serial, and once
more over SSH with the same list. There were no stack faults, and every
boot suite passes: kal 45, conform 23, mod 17, io 44, pub 38, fault 76,
proc 11, sched 15, mem 13. Two full command passes left the heap 132
bytes lower, which is the process history rather than a leak.

### What it bought, and what it did not

Across the 39 modules whose stacks changed, the stacks given went from
293,376 bytes to 99,072. Thirty-five are smaller, and four are larger
because they had been cut too thin by hand: `control` had 164 bytes
spare, `lateloop` 228, `tiny` 8, and `rt` had never been measured
admitting a loop. A typical command now costs 5.6 KB less each time it
runs. That figure is exact: it is stack given, not a heap reading.

The heap readings cannot show it. Free memory on this board swings by
several kilobytes with WiFi and lwIP buffers. In one run, memory for
programs after an SSH session closed was 3.4 KB *higher* than before it
opened. Differences between readings are noise at the scale being
measured, so no before-and-after heap figure is claimed here.

What can be shown is which commands start inside an SSH session. Before
resizing, `pic`, `chart`, `screen`, `edgegen` and others were refused
there. Now they run. Still refused in a session:

- **`ed`**, whose 9 KB is statics, not stack
- **drawing to `/w0`** (`pic > /w0`, `gauge`, `evlat`'s window): opening
  the window allocates about 12 KB of buffers
- **`downloaded`**, loaded from flash into RAM to run
- **anything at all beside two background jobs**: with two `deaf &`
  running, even `free` was refused. The shell's own `procs` from §35
  still answered.

So stacks were the cheapest memory, and they are now spent carefully.
They were not the whole of the problem. What is left in a session is
the session itself, the window's buffers and large statics, and those
are the next places to look.

### What this does not do

**A peak is a floor.** It covers the paths a script exercised. The margin
is a judgment, and the guard is what makes being wrong survivable rather
than silent.

**The caller pays for the system.** Drawing, admission and ending a
process all use the calling process's stack. For a compiler sizing a
generated program from its call graph, those are costs outside the
program that it must still add in. Rendering `/w0` on its own task would
remove one of them, and is not done.

**Peaks are not kept across a reboot**, and measuring again after a change
means running the commands again.

## 38. The timers that stopped

One boot in §37 never joined the network. Its state said "connecting"
forever, with no error, and the next reset connected normally. It looked
like a missing timeout, and the first fix was one. That fix did not
work, and finding out why turned up a fault underneath all of WiFi.

### What it looked like

- A boot that sat at "connecting" with no disconnect reason. The warning
  that came with it appeared on 2 of 28 captured boots.
- `scan` answering "no networks found" with the home network in range,
  and the board unreachable for the rest of that session.
- The first link watch below, tested with `wifi try` on a network that
  does not exist: 150 seconds at "connecting", no retries, and not one
  line from the watch that was meant to catch exactly that.

### Finding it

Each wrong theory was cheap to kill on the board, and each is recorded
here because each was plausible:

- **Two connects at once.** The radio's start event and `net_connect()`
  both called `esp_wifi_connect()`, and one was refused. That was real,
  and both ways of doing it are gone. With it gone, the stall stayed.
- **The WiFi driver's INFO logging blocking.** Not one `wifi:` line
  appeared after `wifi try`. Removing the log level change changed
  nothing.
- **Real-time work.** The watch's timer died at different moments on
  different boots, often during the fault and memory tests. But a control
  boot with no real-time activity, no test suites and no watchdog lost it
  after the first callback too.

Debug counters, not logs, gave the decisive facts. The `esp_timer`
task's callbacks stopped for good while the task sat *blocked*, neither
suspended nor starved: the shell, at a lower priority, kept answering,
and loopback TCP kept working. Nothing was waking it.

### Why

ESP-IDF's `esp_timer` keeps two lists, ISR-dispatched and task-
dispatched, on one hardware alarm. From `timer_alarm_handler()` in
`esp_timer.c`:

```c
esp_timer_impl_try_to_set_next_alarm();
isr_timers_processed = timer_process_alarm(ESP_TIMER_ISR);
...
if (isr_timers_processed == false) {
    vTaskNotifyGiveFromISR(s_timer_task, &xHigherPriorityTaskWoken);
}
```

`esp_timer_impl_try_to_set_next_alarm()` treats the earlier of the two
alarms as the one being serviced and discards it. When the interrupt is
serviced late, so that a task timer and an ISR timer are both due, the
task's alarm is the earlier one. It is discarded, the ISR timer runs, and
because an ISR timer ran the task is not woken. The task is the only
thing that re-arms its own alarm, so from then on no task-dispatched
timer on the board runs again. The WiFi driver's scan and connect timers
are task-dispatched.

RV-9's kernel tick was an ISR-dispatched `esp_timer` at 1 kHz, so an ISR
timer was due within a millisecond of every alarm. Any interrupt held
off that long could lose the wake-up: a flash write, a long critical
section, the stress tests at boot. That is why the time of death varied.

### The fix

- **The kernel tick left `esp_timer`.** It is now a FreeRTOS tick hook,
  at the same 1 kHz, from the host's own tick interrupt.
  `xPortSysTickHandler` calls the hooks unconditionally, so it still
  fires while the scheduler is suspended. A static assert holds
  `RV9K_TICK_HZ` to `CONFIG_FREERTOS_HZ`.
- **A guard for what remains.** The real-time release timers and the
  watchdog still dispatch from the ISR, because they need its precision,
  so the loss is still possible. A 100 ms task-dispatched heartbeat is
  watched from the FreeRTOS tick. If it goes 300 ms without beating, the
  tick wakes `esp_timer`'s task directly. A wake with nothing due costs
  one empty pass, so a false alarm is harmless. Each wake is logged with
  a running count.
- **`esp_timer`'s own yield API.** The ISR-dispatched callbacks in
  `kal_rt.c` used `portYIELD_FROM_ISR()`. They now call
  `esp_timer_isr_dispatch_need_yield()`, which is what ESP-IDF specifies
  for them.

### A link that stays up

The watch that started this now works, because the timers it depends on
do:

- An attempt with no answer for 20 s is restarted. If even that brings
  nothing, the next tick connects directly.
- After five failures it gives up for 30 s, doubling to 5 minutes, then
  tries again. When the failed network was only being tried, it goes
  back to the remembered one.
- `wifi try <ssid> <password>` connects without saving
  (`RV9_NET_SS_TRY`), so a network being tried cannot replace the one
  that works.
- A manual disconnect now stays disconnected. Its own event used to
  start a retry.

### Tested

One boot, driven over serial:

```
suites: kal 45, conform 23, mod 17, io 44, pub 38,
        fault 76, proc 11, sched 15, mem 13
scan after the suites:          networks found, link still up
scan after a real-time loop:    networks found, link still up
wifi try rv9-no-such-network:
  disconnected: reason 201, retry 1 ... retry 5
  giving up on 'rv9-no-such-network' for now: reason 201; trying again in 30 s
  'rv9-no-such-network' failed; going back to remembered '<home network>'
  up after 94 s; SSH answers
panics: 0
```

Before the fix, the same sequence left `scan` finding nothing and the
fallback never happening. Eight consecutive resets on the watch firmware
all joined the network in about 2.1 s. That alone proves little, since
the old warning had appeared on only 2 of 28 boots.

On a later boot the guard woke the task twice:

- **At 1.0 s**, during WiFi start-up and before any real-time work. Most
  likely a false alarm: start-up can keep the task off the CPU that long.
- **At 26.8 s**, five seconds after services came up, with no real-time
  loop running. This one was probably real. The watchdog's 2 ms
  ISR-dispatched timer started at the first real-time declaration (2.9 s
  into boot) and never stopped, so it was a standing trigger for the loss
  whether or not any loop was running.

So the watchdog's timer now runs only while a real-time slot is claimed.
`claim_slot()` arms it and `release_slot()` stops it when none is left.
Both take one lock, and the idle count is of claimed slots rather than
active ones, so a release cannot stop the timer between a claim and its
arming. On the next boot, every suite passed, five idle minutes after
services brought no guard wake at all (the only wake was the start-up
one at 1.0 s), and `rt fastloop` then ran normally, the watchdog re-armed
for it.

### What this does not do

**The underlying ESP-IDF behaviour is not fixed.** It stopped mattering
here when §41 moved the release timers and the watchdog off `esp_timer`
too. RV-9 now puts no ISR-dispatched timers on it at all, which removes
the trigger. The guard stays, recovering within about 300 ms if anything
else ever pulls it.

**Recovery takes up to about 300 ms.** A task timer due in that window
runs late, once.

**It may explain more than it was found for.** The roadmap's open item,
short SSH sessions losing their output on a poor link, has not been
retested. lwIP does not use `esp_timer`, but the WiFi driver does, and
retransmission on a poor link leans on it.

**Fallback is slow.** Five retries at about ten seconds each, each a full
scan of both bands, plus the first 30 s of backoff, is about a minute and
a half before the board returns to the network that works.

## 39. What a session costs

§35 found that inside an SSH session, with one command running, 3.4 KB
was left for programs. §37 made commands cheaper. This section makes the
session itself cheaper.

### Where a session's memory goes

Three things exist for as long as a session lasts:

- **The driver's session state**, `ssh_t`, allocated when `/ssh0` is
  opened. It is mostly buffers: `in` and `frame` at 2,560 bytes and more,
  `out` at 2,560, the window's `pend` at 2,048, `obuf` at 1,024, and the
  key exchange's copies `v_c` and `i_s` at 768. That is about 12 KB, and
  budgets do not see it, because a driver allocates it rather than a fork.
- **The session's shell**: its stack, statics and descriptor.
- **`sshd`'s own stack**, which is paid once, session or not.

### What was cut

- **`out` went from 2,560 to 1,280 bytes.** Incoming packets can be large,
  such as an OpenSSH KEXINIT full of post-quantum names or an RSA-4096 key
  offer, so `in` and `frame` stay as they were. `frame` also holds incoming
  ciphertext. Outgoing packets never are large: channel data goes out in
  chunks of at most 1,024 bytes, and the largest other thing sent is the
  server's own KEXINIT, at most 512. `ssh_send_data` now caps chunks to
  `out` itself rather than to `SSH_BUF_MAX`. Static asserts hold both
  facts, and a packet that did not fit would fail `ssh_packet_send()`
  cleanly rather than overrun.
- **Stacks, measured inside a live session.**

  | | used | was | now |
  |---|---|---|---|
  | session shell | 1,440 | 4,096 | 2,560 |
  | `sshd` (handshake included) | 2,520 | 4,096 | 3,584 |
  | `rshd` | 1,136 | 3,072 | 2,560 |

  The session shell's figure includes its output being encrypted on its
  own stack, since `ssh_flush` runs there when `obuf` fills.

A session now costs about 2.8 KB less, and the daemons 1 KB less at all
times.

### Tested

All boot suites pass. In a live session, with ordinary commands, drawing
and `stacks` run inside it:

```
pid  name     size  used  spare
34   shell    2560  1448  1112    (the session's)
32   sshd     3584  2520  1064
31   rshd     2560  1148  1412
```

`budgets` shows `sshd` holding 11,504 bytes, against 13,552 before, and
the session's shell 4,200 against 5,736. Those are exact figures, unlike
heap readings.

The §35 demonstration, re-run: background `deaf` jobs are started inside
a session until one is refused. It used to stop at five. It now stops at
**eight**. The shell's own `procs` and `kill` then answered with memory
exhausted, on the smaller stack, and the serial log shows no stack fault.

### What this does not do

**`in` and `frame` are still 5 KB between them.** Decrypting in place
would remove one, and depends on how the PSA implementation handles
overlapping buffers.

**The key exchange's copies (768 bytes) live for the whole session.**
They are needed only during the handshake.

**Drawing to `/w0` still cannot run in a session.** Opening the window
allocates about 12 KB, and a session does not have that.

**`ed` still cannot run in a session.** Its 9 KB is statics, the text
buffer itself.

## 40. A port that keeps answering

The roadmap has carried an open item since phase 9: short SSH sessions
sometimes lose their output. It was blamed on a poor link. A test on a
good one found two separate things.

### Refused between sessions

Thirty short sessions run back to back (`free`, then `exit`, each):

```
before:  ok 18, refused 12, no output 0
```

Every refusal came just after the previous session ended. NFM made a
listening socket for each open of `/n0/listen/<port>` and closed it as
soon as it had accepted one connection. So between one connection and
the daemon's next open, nothing listened on the port at all. For `sshd`
that gap is the whole session, plus the up-to-three seconds `ssh_close`
waits for the client to hang up, plus the loop back round. A client
arriving then was refused.

Now a listener outlives its connections. The first open of a port binds
and listens. Later opens accept from the same socket, and a connection
arriving between them waits in the backlog. A listener is kept while
anything uses it: a connection it accepted is still open, or someone is
waiting in accept. It stays for two seconds after the last use ends,
which is ample, since `sshd` opens again within milliseconds of closing a
session. After that it is closed at the next NFM open or close, so a port
nobody serves goes back to refusing rather than holding connections
forever. Four listeners at most.

```
after:   ok 29, refused 0, no output 1
```

Refusals went from 12 in 30 to none, across three runs.

### The fault test, adjusted

`fault-test` kills a process blocked in accept and checks that the port
can be listened on again and that nothing stayed allocated. The first
still holds; the second failed at 1,028 bytes against its 1,024 limit,
because the listener is now kept on purpose. The test waits out the grace
period and lets NFM reap before it measures. On three boots it passed 76
of 76 with the heap slightly higher than before.

### Output lost after it was sent

With refusals gone, about one session in thirty still got no output at
all, not even the shell's banner. Its client trace at `-vv` matched a
session that worked, line for line.

Temporary per-session counters in the driver settled where the bytes
went. For the empty sessions they were identical to the good ones: the
10 input bytes received, 274 bytes written by the shell, 257 sent on the
wire before close and the last 17 at close, nothing refused, no flag set
early. The board had sent everything.

It was the goodbye. `ssh_close` sent exit status, channel EOF, channel
close, and then straight away `SSH_MSG_DISCONNECT`. When the last channel
data and that disconnect arrive in one read, the OpenSSH client queues
the data for its stdout, handles the disconnect, and exits before the
queue is written. That is the roadmap's "short sessions lose their
output". A slow link bunches packets together, which is why it was
first seen on one. The same disconnect made every session exit 255 rather
than with its status.

The channel close is what ends a session. A client whose only channel is
closed closes the connection itself, and `ssh_close` already drains and
waits for exactly that. The disconnect is now sent only if the client is
still there when that wait runs out.

```
60 sessions:  ok 59, exit 0: 59, one empty (exit 255, unexplained)
60 sessions:  ok 60, exit 0: 60   (serial log and client messages kept)
```

A long session and the §35 budget demo behave as before. The one empty
session in the first run came while the client was logging errors only
and the serial log was not captured, so its cause is not known. An earlier
run did show one session lost to a real WiFi dropout ("disconnected:
reason 1"), which the §38 link watch recovered.

### Also seen: a real-time release skipped

Two boots in this work failed real-time checks because a loop went more
than one whole period without running ("1 releases skipped", "2 releases
skipped"): the fault test's first `lateloop` once, and the scheduling
test's derived pair once. It is not new. Counting every captured boot
log, a nonzero skipped release appears in 3 of 32 boots before the §38
timer fix and 2 of 10 after, too few to tell the rates apart. It is an
intermittent latency spike of 10 ms or more under a priority-24 task. Its
cause turned out to be `esp_timer` again, and it is fixed; see §41.

## 41. The clock a control loop is released by

§40 recorded a real-time loop occasionally going a whole period or more
without running: "2 releases skipped", "35 releases skipped", in 5 of 42
boot logs. For a control loop that is the worst kind of fault. It is
stopped for a deadline it would have met, and nothing it did caused it.

### Reproducing it

`rt lateloop ontime` runs a 100 Hz loop for a minute and stops it on any
missed deadline. Run seven times back to back over SSH, four or five of
the seven were stopped.

### Measuring it

A skipped release says the task ran late, not why. Two instruments were
added, and both stay.

- **The stall, split in two.** The release interrupt stamps the first
  release since the task last woke. On waking, the task divides its
  lateness into how late that interrupt ran against its due time, and
  how long after it the task got the CPU. The worst stall is kept in the
  real-time statistics and logged beside the deadline fault.
- **Interrupts held off, caught in the act.** The FreeRTOS tick is an
  interrupt at 1 kHz. A gap between two ticks well over a millisecond is a
  window in which interrupts were masked, measured, and the task current
  when the late tick runs is the one that closed it. Windows over 5 ms are
  logged with that task's name.

What they said, every time:

```
worst stall: its release interrupt ran 32884 us late, then it waited 67 us for the CPU
worst stall: its release interrupt ran 88693 us late, then it waited 175 us for the CPU
interrupt-off windows logged: 0
```

The task was never the problem: it had the CPU within a fraction of a
millisecond of its interrupt. Interrupts were never masked, since the
tick kept its millisecond throughout. Only the release interrupt was
late, by 20 to 89 ms. And each stall was followed, exactly 300 ms later,
by the §38 guard waking `esp_timer`'s task.

### Why

The same place as §38, from the other side. `esp_timer` keeps its two
lists on one alarm, and `esp_timer_impl_try_to_set_next_alarm()` discards
the alarm it was called for on entry. When that alarm belonged to an
ISR-dispatched timer, the release, and nothing on the ISR list turns out
to be quite due when checked, `timer_process_alarm()` does not arm the ISR
list's alarm again. So the next hardware alarm is whatever the task list
wants. The guard's own heartbeat is 100 ms, and WiFi's timers are
similar. The release waits for that alarm. When it comes, the overdue
release runs, and because an ISR timer ran, the task list's wake-up is
skipped. That is the lost wake the guard found 300 ms later.

It was tested before anything was rebuilt. With the heartbeat shortened
from 100 ms to 20 ms, the worst stall fell from 88.7 ms to 12.4 ms.

### The fix

The releases and the watchdog no longer share a timer with anything.

- **One GPTimer** counts microseconds, at interrupt priority 3, above the
  level-1 interrupts `esp_timer` and most drivers use.
- **A table of at most five entries**, one per real-time task and one for
  the watchdog, holds when each is next due. The alarm handler runs
  everything due, advances each past any whole periods already gone
  (the task counts those from the clock when it wakes), and arms the
  alarm for the soonest. It never arms one in the past: it reads the
  count back and moves the alarm on if the count got there first.
- **It keeps running with the flash cache off.** `GPTIMER_ISR_CACHE_SAFE`
  and `GPTIMER_CTRL_FUNC_IN_IRAM` are on, so the handler and the re-arm
  both work while the radio writes NVS.

Event-driven tasks are unchanged: their releases were always the
device's own interrupt. With this, RV-9 puts no ISR-dispatched timer on
`esp_timer` at all.

### Tested

All boot suites pass: kal 45, conform 23, mod 17, io 44, pub 38,
fault 76, proc 11, sched 15, mem 13. The same seven rounds:

```
before:  4 or 5 of 7 rounds stopped; a guard wake after every stall
after:   6 of 7 ran their full minute; no deadline missed; no guard wake
```

The seventh round's loop also finished its minute: its failsafe was
applied at the end, as for every round. The link then dropped ("reason
1") before the SSH client had its output, and the client waited out its
own timeout. The §38 watch reconnected.

### What this does not do

**Flash is not the cause, and was checked.** Before the measurements
pointed at `esp_timer`, a loop was run beside repeated `/f0` writes. It
survived them, and stopped in a quiet period with no writes at all.

**The detector's floor is 5 ms.** A masked window shorter than that goes
unreported. Nothing here needed it to be finer.

**Priority 3 is not the top.** Interrupts at higher levels, and critical
sections anywhere, can still delay a release. What is gone is a delay
built into the timer service.

**Five entries.** Four real-time tasks, the slot limit, and the watchdog.
A fifth periodic task would need a larger table, as it would need a
slot.

## 42. The card

The microSD slot was the last hole in phase 5: RBF was proven on a RAM disk
and on a flash partition, and `/sd0` was a driver-shaped gap waiting for a
card to exist. One arrived.

### The driver

`sdspi` is a block driver like the other two: `geometry`, `read_blocks`,
`write_blocks`, and nothing else. ESP-IDF's SD-over-SPI host does the card
protocol; this drives it, one sector at a time through a DMA-capable
bounce buffer, because a caller's buffer may be anywhere.

The card shares the display's SPI bus -- clock on 7, data in on 6, the
card's chip select on 4 and the display's on 23 -- which sounded like the
hard part and was not. Two devices on one bus with a chip select each is
what the SPI driver already arbitrates, so a redraw and a sector write
cannot overlap. What did need doing: the bus was brought up by the panel,
configured with no data-in line, because a display never answers. It is
now brought up by whichever of the two attaches first, with the card's
data-in line included.

There is no card-detect pin on this board, so a card is found by trying to
initialise one. No card means `/sd0` does not attach, and a program asking
for it is told the machine has no such device -- which is truer than a
device that fails every read.

### What a big volume broke

The card is 62,333,952 sectors. Two things in RBF had never met a volume
that size, and both were found by using it rather than by reading it.

- **Creating the first file took nineteen seconds.** `alloc_sector`
  walked the volume a sector at a time asking "is this one free?", and
  each question read a whole bitmap sector. The first 15,221 sectors are
  metadata, so it read the same bitmap sector fifteen thousand times. It
  now walks the bitmap instead: read a sector, take the first clear bit,
  write it back. Four reads. A create takes **0.2 s**.
- **The root directory was two sectors, 32 files**, the same on a 16 KB
  RAM disk as on a 32 GB card. It is now sized at format time from the
  volume: 2 sectors below 64 K sectors, 8 below 1 M, 16 above -- 256
  files. Not larger, because every sector of the root is read when a name
  is looked up and missed.

Both are why the layout is decided at format time and recorded in the
identification sector: `/r0` and `/f0` kept their own layout and were not
touched.

### Formatting, and when not to

Mounting a volume RV-9 does not recognise formats it. That is right for a
RAM disk, which is empty every boot, and it is how a fresh card becomes
RV-9 storage by being put in the slot -- as this one did, on the boot
after it was inserted.

It also means RV-9 claims the card. There is no FAT here, so a card
holding RV-9 files is not a card a PC will read. For the other case there
is now `format`, which empties a volume that RV-9 *does* recognise:

```
rv9> format /sd0
format: this empties /sd0 completely, and there is no undo.
say so: format /sd0 yes
rv9> format /sd0 yes
formatting /sd0 -- on a large card this takes a minute
/sd0: empty
```

Two words, because there is no undo. It is a file manager's setstat rather
than a driver's -- the volume is RBF's, not the card's.

### Tested

On the board, with a 30,436 MB card:

```
/sd0: card of 30436 MB, 512-byte sectors
/sd0 mounted: volume 'rv9', 62333952 sectors      (1.2 s at boot)
format /sd0 yes                                    60.5 s
echo ... > /sd0/t1.txt                              0.2 s
40 files created                                    8 s
dir /sd0                                           43 files
```

After a reset: the card mounted in 1.2 s, all 43 files were still there
and read back correctly, every boot suite passed (kal 45, conform 23,
mod 17, io 44, pub 38, fault 76, proc 11, sched 15, mem 13), and memory
was where it always is.

### Since: room, and programs from the card

Two things the card made worth having.

**`df` answers how much room a volume has.** `RV9_GS_SIZE` answers for a
file; there was no way to ask a volume, which mattered little on a 16 KB
RAM disk. It is a file-manager getstat on the device itself, counted from
the bitmap when asked rather than kept running -- a wrong cached number
would be worse than a slow true one.

```
rv9> df /r0
/r0  size 16 KB  used 2 KB  free 14 KB
rv9> df /f0
/f0  size 1024 KB  used 46 KB  free 977 KB
rv9> df /sd0
/sd0  size 31166976 KB  used 7661 KB  free 31159315 KB
```

The card's 7,661 KB in use with nothing on it is the metadata: 15,221
sectors of identification, bitmap and root directory. Counting takes about
ten seconds there, because it reads all 15,218 bitmap sectors, and it holds
the volume's lock while it does -- see what this does not do.

**Programs on the card are commands after a reboot.** `autoload()` was
already generic over a device and was called for `/f0`; it is now called
for `/sd0` too. A board with no card opens nothing and pays nothing.

### What this does not do

**`df` on a large card takes ten seconds and blocks the volume.** It holds
the mount lock for the whole count, so nothing else on that volume moves
meanwhile. Keeping a running free count would fix it, at the cost of a
number that could drift from the truth.

**Formatting a fresh card delays the boot it happens on.** Writing 15,218
bitmap sectors one at a time took about a minute, inside device attach,
so that boot took a minute longer. It happens once per card, and nothing
says so while it is happening.

**Allocation is still linear in the worst case.** The hint means a filling
volume does not rescan from the start, but a full one still walks every
bitmap sector before reporting that it is full.

**No FAT, and no partition table.** RV-9 takes the whole card as one RBF
volume.

**The card is not hot-pluggable.** It is found at attach, and there is no
card-detect line to notice one arriving or leaving. Insert it, then boot.

## 43. A fault that would not come back

The overnight soak in §42's week dropped the WiFi link 132 times in nine
hours, about one every three and a half minutes, almost all reason 1
(unspecified). Every session failure in that run followed from it. This
section is what came of chasing it, including the part where it stopped
happening and the cause was never found.

### The control that mattered

A laptop sat on the same access point, the same channel, through the same
hours, and dropped nothing in 22.9 hours -- at -75 dBm, while the board
dropped repeatedly at -58 dBm. Signal strength was not it, and the
environment looked innocent.

That reading was too quick. The system journal later showed the access
point steering that laptop between two radios at 20:49, mid-soak, with a
WNM "disassociation imminent" request -- and doing nothing of the sort on
any later evening. The network was not identical between the run that had
drops and the runs that did not, which is exactly the sort of difference
a control is supposed to expose and this one nearly hid.

### Narrowing it, and failing

Each run is twenty minutes or more, on the board, with the serial log kept:

| what ran | drops |
|---|---|
| idle, nothing at all | 0 |
| 638 short SSH sessions, no loops | 0 |
| a 100 Hz loop, 19 rounds, no network use | 0 |
| both at once, loop on serial and sessions over the air | 0 |
| the soak's own shape: loop rounds *inside* SSH sessions | 0 |
| the original script again, 2 h | 0 |
| the original script again, 5 h quiet daytime | 0 |
| the original script again, 4 h busy evening | 0 |

Over twelve hours of the same load that produced a drop every few
minutes, and not one drop since. Three theories died on the way:

- **The real-time loop starving the radio.** A loop runs above the WiFi
  task, and since §41 its release interrupt runs above the radio's
  interrupt level. Plausible, and wrong: the loop alone is clean.
- **Session churn.** Also wrong: 638 sessions alone are clean.
- **Memory pressure.** The driver now logs free heap at every disconnect;
  the one drop ever caught that way had 84 KB free.

### What was added on the way

Nothing was fixed, because nothing was found. What is left behind is
instrumentation, which is the honest product of a hunt like this:

- **Disconnects say what memory looked like.** A reason code alone could
  not distinguish exhaustion from anything else.
- **The low-water mark now has two numbers.** `free` reports the lowest
  free memory since boot and since the machine started serving. They
  differ because the boot suites spend memory on purpose -- `mem-test`
  drives the heap to the floor to prove a control loop is still admitted
  -- which pins the since-boot figure at about 12 KB and tells you
  nothing about the machine as it runs. ESP-IDF keeps one counter, not
  two, so the figure from before the re-base is kept in the KAL and the
  all-time answer is the lower of the halves.

That second instrument exists because of a mistake worth recording: the
soaks reported a low-water of about 2,500 bytes, and this was written off
as the memory suite doing its job. The boot logs say `mem-test` bottoms
out at the floor, around 12,400. Something in ordinary running takes it
far lower, and now there is a number that can see it.

### Where it stands

Not reproducible, not explained, and not claimed to be fixed. What is
known: it is not the loop, not the sessions, not memory, not signal
strength, and not idle-versus-busy. What correlates is the evening it
happened: the drop rate fell hour by hour as the household wound down --
42 in the first hour, then 15, 18, 15, 12, 4, 12, 8, 4, 2 -- and the
access point was steering clients that night and has not been since.

If it returns, the board now records free memory at each drop, and the
first thing to check is whether the access point is moving clients
around again.

## 44. How long forever is

Step 4 of the migration is `wifi_osi_funcs_t` — the radio blobs running on
RV-9's primitives instead of FreeRTOS's. Before writing any of it, both
sides were read properly: what the blobs actually call, and what RV-9
actually has. That comparison said, unambiguously, that step 4 is not next
(§45). It also found two faults in what is already here, and neither was
in the part anyone was looking at.

### A contract with two implementations, and no test where they differ

`RV9_WAIT_FOREVER` is the KAL's way of saying a wait has no deadline. The
FreeRTOS backend has always translated it: `portMAX_DELAY`, a value that
kernel reserves for exactly this. The native kernel had nothing, so the
value fell through to the ordinary conversion from milliseconds to ticks:

```c
#define RV9K_MS_TO_TICKS(ms)  ((ms) * RV9K_TICK_HZ / 1000)
```

At 1 kHz that reads as an identity and is one for every value anybody
tests with. `0xFFFFFFFF * 1000` overflows thirty-two bits before the
divide can undo it, and what comes out the other side is 2,294,725 ticks.
**Waiting forever was waiting thirty-eight minutes.**

The same arithmetic is in FreeRTOS's own `pdMS_TO_TICKS`, which multiplies
in `TickType_t`, so the reference backend had a version of it too: a
timeout of 4,294,968 ms — seventy-one and a half minutes, the longest wait
expressible short of forever — converts to **zero** ticks. The longest
possible wait returned immediately, having waited not at all.

Both are fixed by widening the intermediate, and forever is now its own
case in the kernel rather than a very large number. "No deadline" cannot
be spelled as a tick value in any case: every tick value is a legal
deadline once the counter wraps, so a blocked thread now carries a flag
saying whether its deadline means anything.

### Why the suite did not catch it

§9c said the conformance suite is the acceptance test for the native
kernel, and that a contract with two implementations is what forces the
contract to be written down precisely. That worked — it found a recursive
mutex deadlocking against itself in the *reference*.

It did not find this, because it only ever passed timeouts of 0, 100 and
2000 ms. Those are the values a person types while writing a test: small,
round, and comfortably inside every representation. The two values where
the implementations disagreed were at the far end of the range, and
nothing went there.

Four checks now do, and they are structured so they can fail rather than
hang: a thread is parked on a semaphore nobody has given, the suite
confirms it is still parked a moment later, then gives the semaphore and
confirms the wait ended with a *give* rather than a timeout. Both values
are tested, against both backends.

Putting the old arithmetic back was worth the two minutes it took. It did
not produce a neat row of FAILs — the board panicked and rebooted, fifty-
two times, on an illegal instruction inside the parked thread. So the fault
was never merely "a long wait ends early"; a timeout past seventy-one
minutes was a way to crash the machine, and no caller had happened to ask
for one yet.

### The other fault: a header that had stopped being true

`kal.h` said, of `rv9_sem_give_from_isr` and `rv9_queue_send_from_isr`:

> NOT IMPLEMENTED under the native kernel, which refuses with
> `RV9_ERR_UNSUPPORTED` rather than pretend.

Both have been implemented since §41. The header had simply not been
changed, and the comment directly beneath it is the account of a driver
that waited on a semaphore nothing could give and turned a 1.2 second boot
into fourteen — a fault caused by a caller believing the wrong thing about
these two functions.

A header is not documentation about code; for anyone calling across a
seam it *is* the code. This one had been wrong for longer than it was
right, sitting on top of its own cautionary tale.

## 45. What the radio would need

`wifi_osi_funcs_t` is the table ESP-IDF hands the WiFi blobs so they can
create a task, take a mutex and post to a queue without knowing whose
kernel they are on. Satisfying it with RV-9's primitives is the step where
the radio stops depending on FreeRTOS. Before starting, both halves were
measured rather than assumed: the blobs were disassembled and every
indirect call through `g_osi_funcs_p` resolved to a field, so "the driver
uses this" below means it was seen to, and "does not" means it was looked
for and is absent.

### The numbers

The WiFi stack creates **exactly one task**: `ppTask`, from
`pp_create_task`, with a 3 KB or 6 KB stack. Its priority is not a
constant anywhere in ESP-IDF — the blob asks `_task_get_max_priority()`
and subtracts two. On this build that is 23 of 25: **second from the top**,
above everything except the two real-time classes.

### What is genuinely missing

Most of the table is not about scheduling at all — PHY, clocks, NVS, coex,
logging, thirty-odd fields that pass straight through to ESP-IDF and
always will. Of the rest:

| | |
| --- | --- |
| semaphores, mutexes, task create/delete/delay | already there, thin wrappers |
| event groups | RV-9 has nothing like them — **and the blobs never call them** |
| `_queue_create`, `_queue_send_to_front`, `_task_create` | dead on this chip |
| software timers | **584 call sites, and RV-9 has no timer service at all** |
| `_task_yield_from_isr` | cannot be honoured as specified |
| `_is_from_isr` | RV-9 has no "am I in an interrupt" predicate |
| `_wifi_thread_semphr_get` | needs a second task-local slot; the I/O manager owns the only one |

Event groups looked like the largest gap on paper and are a phantom: five
stubs that abort loudly is the correct implementation. The timers are the
opposite — `_timer_disarm` alone is called 217 times and `_timer_done`
172, and they look like a hardware shim while being a second scheduler.
That is the one to plan for.

### Why this is not the next step

Three reasons, none of which an adapter layer can absorb:

**The kernel is cooperative and the radio is not optional about being
scheduled.** `ppTask` blocks on a queue, which is polite, and then runs
blob code that will never call `rv9k_preempt_point()`. Under RV-9's
scheduler any compute-bound thread stalls it until it volunteers, and a
WiFi task that misses beacons gets the station disassociated.

**An interrupt cannot switch to it.** `_task_yield_from_isr` is called
from the MAC receive interrupt, immediately after `_queue_send_from_isr`,
and it means *switch now*. RV-9's answer is to raise the count and leave
the waking for thread context (§9b). That is right for a semaphore and the
wrong latency for a radio.

**It would lose its standing.** Hosted as an RV-9 thread it runs inside
`rv9-kernel` at host priority 19, down from 23 — beneath `esp_timer`,
beneath both real-time classes — and subject to aging, so a starved shell
thread could transiently outrank it. Raising the kernel task above 23 was
considered and rejected in §33, because it would put the shell above a
control loop.

### The roadmap, corrected

This document's §9 listed `wifi_osi_funcs_t` as step 3 and
`components/rv9_kernel/include/rv9/kernel.h` listed it as step 4, behind
owning the CPU. The header was right, and the ordering is load-bearing
rather than cosmetic: the radio needs preemption and an interrupt that can
reach the scheduler, and both of those are what owning the machine buys.
The list at the end of this document has been corrected to match.

## 46. Where the reserve goes

§43 ended with an open question. Free memory sat flat at around 38 KB for
hours at a time, and the low-water mark said something had taken it to
2,500 bytes. The instrument that could see it was built at the end of §43;
this is what it saw.

A three-hour soak, sampled every two minutes:

| elapsed | heap free | low water | refused |
| --- | --- | --- | --- |
| 13 s | 37972 | 27876 | 3 |
| 2661 s | 50708 | 11268 | 3 |
| 5685 s | 37992 | 9888 | 4 |
| 8779 s | 37972 | 9348 | 5 |
| 10853 s | 40296 | 4968 | 5 |

Free memory does not move. The low-water mark ratchets down all night, and
the count of allocations the floor refused climbs with it. So nothing is
leaking; something is taking a large amount and giving it straight back,
and occasionally several of those coincide.

### Found by taking it apart

From a fresh boot, one thing at a time, reading the low-water mark after
each. The numbers are what remained free at the worst moment:

| | low water |
| --- | --- |
| idle, one session to ask | 27800 |
| after 20 sessions, one after another | 25952 |
| after 5 sessions at once | 25320 |
| **after one real-time loop round** | **16784** |
| a loop round with 5 sessions across it | 13064 |
| the same again, twice more | 11204 |

Sessions are nearly free — twenty of them cost 1.8 KB between them, and
running five at once costs barely more than running them singly. **Forking
a real-time process costs 8.5 KB**, all of it returned when the process
ends: a host task and its stack, the slot, the process record, the path
table.

Nothing here is a leak and nothing is surprising in isolation. The deep
excursions are coincidences — a real-time fork while two sessions are
being established — and over a night of a hundred and fifty rounds and
eight hundred sessions, the rarest coincidences are the ones the low-water
mark remembers.

### The part worth keeping

The soak's worst reading, 4,968 bytes, is **below the floor**. `free`
reports a 12,288-byte reserve that RV-9 will not allocate into, and this
went through it.

That is not the floor failing. It is the floor working, and §23 says so in
advance: the reserve is not for RV-9's benefit but for everything RV-9
cannot ask — WiFi, the PHY, lwIP, ESP-IDF's own internals, which allocate
straight from the heap and abort when they cannot. Keeping RV-9 out of the
last twelve kilobytes is what leaves them there to be taken. The refusal
counter climbing from 3 to 5 across the soak is the record of RV-9 being
told no at exactly those moments, and going without instead.

So the answer to §43's question is that the memory goes where it was
reserved to go, and the machine stayed up for three hours while it
happened.

## 9. Migration to a native kernel

The point of the KAL. When the personality layer is working and the design has
been validated by use:

1. Implement the RV-9 scheduler, timers and memory allocator natively
2. Its own timer interrupt, so preemption does not need anyone's cooperation
3. Own the CPU from reset; the host scheduler goes away
4. Implement `wifi_osi_funcs_t` against RV-9 primitives — the blobs never
   know — and port lwIP `sys_arch`, one file
5. Add PMP-based process isolation, which the 8-bit systems of the 1980s never had

Steps 1 and 2 are done; see §9a and §9b. `wifi_osi_funcs_t` was listed
here as step 3 for a long time, ahead of owning the CPU. It is behind it,
and §45 is the measurement that says why.

Throughout, the FreeRTOS build stays alive as a reference: any behavioural
divergence is a bug in the new kernel, and you have a working system to diff
against. This is a substantially better position than bringing up a scheduler
with no oracle.

---

## 10. Risks

| Risk | Assessment |
| --- | --- |
| RAM exhaustion once WiFi + lwIP are resident | Highest. May force XIP everywhere and a very lean path table. |
| Blob assumptions about interrupt latency | Real but survivable; NuttX/Zephyr are proof it works. |
| Module relocation complexity | Medium. Fixed addresses are the escape hatch. |
| Scope — this is a large project | Managed by phasing: steps 1-4 are useful on their own. |

---

## 11. Open questions

- PIC modules or fixed load addresses?
- Native filesystem on the SD card, or FAT forever?
- Does the LP RISC-V core get a role, or stay unused?
- Shell: a traditional one, or something new?
