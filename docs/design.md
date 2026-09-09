# RV-9 — Design

A small modular operating system for RISC-V, in the spirit of Microware OS-9.

Status: phase 0 complete — KAL implemented on FreeRTOS, 27/27 conformance
tests passing on hardware. See docs/roadmap.md.

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

Open question: relocation. Either PIC-compiled modules, or fixed load addresses
assigned per module at build time. OS-9's position independence came from the
same pressure on the 6809, so the precedent is apt. **To be decided in phase 1.**

---

## 6. Processes

- Preemptive, priority-scheduled, with **aging** so low-priority work cannot starve
- `fork` (new process from a module), `chain` (replace current), `exit`, `wait`
- Each process: module reference, static data area, stack, path table, priority,
  state, parent, exit status
- No memory protection in early phases; PMP-based isolation arrives with the
  native kernel

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

### Planned devices

| Descriptor | File manager | Driver | Notes |
| --- | --- | --- | --- |
| `/term` | SCF | lcdcon | console on the ST7789 panel |
| `/uart0` | SCF | uart | serial console |
| `/sd0` | RBF | sdspi | FAT initially, native fs later |
| `/n0` | NFM | wifi+lwIP | network as a path, not a socket API |

`/term` on the LCD is the first milestone that will actually feel like an
operating system.

---

## 8. Networking

WiFi association and lwIP run underneath a **network file manager**, so network
endpoints are opened, read and written like any other path. A raw socket API
may exist beneath it, but it is not the interface the system presents.

Ordering matters: this comes *after* the I/O manager is solid, because NFM is
the proof that the I/O abstraction was designed correctly rather than shaped
around one device.

---

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
