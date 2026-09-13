# RV-9 — Design

A small modular operating system for RISC-V, in the spirit of Microware OS-9.

Status: phases 0-6 complete; phase 7 steps 1-2 done, and **the whole
system now runs on RV-9's own kernel** — processes, I/O, storage,
networking and the shell, all scheduled by rv9_kernel — KAL on FreeRTOS (27/27 conformance tests
passing on hardware), module format/directory/loader, and processes with
priority aging, all verified on hardware. See docs/roadmap.md.

---

## 1. What this is

RV-9 is an attempt to rebuild what made OS-9 pleasant — position-independent
memory modules, a unified I/O model, real preemptive multitasking in a very
small footprint — on a modern RISC-V microcontroller with WiFi.

It is **not** a clone. No binary compatibility with OS-9 modules, no attempt to
run existing OS-9 software. The ideas are worth stealing; the 1980s encodings
are not.

### Goals

- A module format and module directory that make code a first-class runtime object
- The OS-9 I/O model: file managers, drivers and device descriptors as separate,
  independently loadable pieces
- Preemptive priority scheduling with aging
- Networking that feels like the rest of the I/O system, not a bolted-on socket API
- A kernel small enough to understand completely

### Non-goals

- POSIX compatibility
- Running someone else's binaries
- Beating FreeRTOS at anything measurable

### The honest motivation

The author likes RISC-V and liked OS-9. That is sufficient.

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
take a large bite out of it. OS-9 ran well in 64 KB, so the kernel is not the
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

The OS-9 I/O model, kept nearly intact, because it is better than what most
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
| `/sd0` | RBF | sdspi | not yet written |
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
device is adding a module — no kernel rebuild, exactly as OS-9 intended.

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
  be able to block in `read()` on an armed pin — the OS-9 shape, where a
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

## 9. Migration to a native kernel

The point of the KAL. When the personality layer is working and the design has
been validated by use:

1. Implement the RV-9 scheduler, timers and memory allocator natively
2. Satisfy the KAL with them; the personality layer does not change
3. Implement `wifi_osi_funcs_t` against RV-9 primitives — the blobs never know
4. Port lwIP `sys_arch` to RV-9 — one file
5. Add PMP-based process isolation, which real OS-9 on a bare 6809 never had

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
- Shell: OS-9-flavoured, or something new?
