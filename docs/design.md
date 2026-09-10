# RV-9 — Design

A small modular operating system for RISC-V, in the spirit of Microware OS-9.

Status: phases 0-6 complete, phase 7 steps 1-2 done — KAL on FreeRTOS (27/27 conformance tests
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
