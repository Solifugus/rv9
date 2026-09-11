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
