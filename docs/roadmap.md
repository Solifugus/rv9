# RV-9 — Development Plan

Phases are ordered so that each one produces something that works and is worth
having on its own. Nothing here requires the phase after it to be useful.

Estimates assume evenings-and-weekends pace, not full-time.

---

## Phase 0 — Skeleton  ✅ done

**Goal:** an ESP-IDF project that builds, boots, and enforces the layering rule.

- Project scaffold targeting esp32c5, reusing the working display setup
- Directory layout: `kal/`, `kernel/`, `io/`, `modules/`, `tools/`
- `rv9_kal.h` + FreeRTOS backend (thin wrappers, no cleverness)
- **Layering check in the build**: fail if anything above the KAL includes
  `freertos/*.h`. Write this on day one; it is the load-bearing discipline.
- Boot banner over serial

**Done when:** it boots, prints, and the layering check demonstrably fails a
deliberate violation.

*Rough size: a weekend.*

---

## Phase 1 — Modules  ✅ done

**Goal:** code as a runtime object.

- Module header format (see design §5), CRC-32 verification
- `tools/mkmodule` — host-side tool to wrap a built binary into a module
- Flash partition for module storage
- Module directory: scan flash at boot, index everything valid
- Load, link/unlink, link counting, sharing
- **Decided:** position-independent by construction (`-mcmodel=medany`, one
  blob, no `.data`/`.bss`, enforced by linker assertions), copied into
  executable RAM. See design §5.
- `mdir`-equivalent: list modules over serial

**Done when:** a module is built by the host tool, written to flash, found at
boot, verified, loaded, and its entry point called.

*Rough size: several weekends. The relocation decision is the long pole.*

---

## Phase 2 — Processes  ✅ done

**Goal:** more than one thing running, scheduled the way OS-9 did it.

- Process descriptor, state machine, process table
- `fork` from a module, `exit`, `wait`
- `chain` (replace the running module, keeping the pid) **deferred to phase 4**,
  where the shell is the thing that actually wants it
- Priority scheduling **with aging** — starvation-free is a design requirement
- Per-process static data and stack from the module header hints
- Signals or an equivalent minimal async notification
- `procs`-equivalent: list processes

**Done when:** two processes forked from modules run concurrently, at different
priorities, and the low-priority one still makes progress.

*Rough size: a few weekends. Straightforward while FreeRTOS does the hard part.*

---

## Phase 3 — I/O manager and SCF  ✅ done

**Goal:** the abstraction the whole system hangs from — and a real console.

- Path table (per-process), path descriptors, the generic call surface:
  `open close read write seek getstat setstat`
- File manager and driver interfaces as module types
- Device descriptor format and binding
- **SCF** — sequential character file manager
- `uart` driver → `/uart0`
- `lcdcon` driver → `/term`: a text console on the ST7789, scrolling, cursor
- Standard paths (stdin/stdout/stderr equivalents) inherited across fork

**Done when:** a process opens `/term`, writes to it, and text appears on the
panel — through the full manager/driver/descriptor stack, with no shortcuts.

*Rough size: the biggest phase so far. This is the heart of the system.*

---

## Phase 4 — Shell and utilities  ✅ done

**Goal:** it stops being a demo and becomes something you can sit in front of.

- Shell as a loadable module: parse, fork, wait, redirect paths
- Utilities as separate modules: `mdir`, `procs`, `free`, `echo`, `chaintest`
- I/O redirection between paths (`cmd > /dev`), via `dup2` and
  inherit-by-reference
- `chain()`, deferred here from phase 2 and now done: a process continues as
  a different module, keeping its pid, priority and open paths
- `sysinfo` in the module ABI, so utilities can ask about modules, processes
  and memory without being part of the kernel
- `load`/`unlink` as commands, and pipes, did **not** land. Pipes want a PIPE
  file manager, which is better company for RBF in phase 5 than bolted on
  here.

**Done when:** you type a command at a prompt on the LCD and a separate module
runs, with output redirected.

At this point RV-9 is a real, if small, operating system. **A reasonable place
to stop and enjoy it for a while before committing to the rest.**

*Rough size: a few weekends, and the most fun of them.*

---

## Phase 5 — Storage  ✅ RAM disk done, SD card pending

**Goal:** persistence.

- **RBF** — done. Identification sector, allocation bitmap, directory,
  file descriptors with segment lists. A directory is a file.
- block driver interface — done: `geometry`, `read_blocks`, `write_blocks`,
  separate from the character `read`/`write`
- `ramdisk` driver → `/r0` — done, 64 KB
- `dir`, `filetest`, `del` utilities, argument passing to modules — done
- **`sdspi` driver → `/sd0` — not written.** Waiting on a microSD card to
  test against. The file manager above it is already proven, so this is a
  driver-shaped hole rather than an unknown.
- Bus arbitration with the LCD is the interesting part when it comes: both
  devices share SPI, and mediating that is the I/O manager's business rather
  than each driver improvising.
- Not done: a native filesystem beyond RBF, and loading modules from a
  volume rather than the flash partition.
- **Pipes did not land.** A PIPE file manager is straightforward, but shell
  pipe syntax needs two processes with one blocking on the other's output,
  and that deserves its own attention rather than being tacked on here.

**Done when:** a module is loaded from a volume and run. Not yet — files
work, but the loader still reads only the flash partition.

---

## Phase 6 — Network  ✅ loopback verified, association untested

**Goal:** the network as a path, and the proof the I/O design was right.

- Bring up WiFi under ESP-IDF, still on FreeRTOS
- lwIP as it ships
- **NFM** — network file manager, exposing endpoints as paths
- `/n0` descriptor
- Something end-to-end and satisfying: fetch a URL from the shell and display it

**Done when:** `/n0` is opened, written, read, and closed by an ordinary process
with no network-specific system calls.

If the I/O abstraction was designed properly in phase 3, this phase mostly
writes itself. If it was not, this is where you find out — which is precisely
why it comes here and not earlier.

---

## Phase 7 — The native kernel

**Goal:** remove FreeRTOS. The year-of-evenings phase.

- RV-9 scheduler: context switch, run queues, priority aging, tick handling
- Native timers, native allocator
- KAL native backend, satisfying the same interface phases 0-6 proved correct
- `wifi_osi_funcs_t` implemented against RV-9 primitives — the blobs never know
- lwIP `sys_arch` ported — one file
- **PMP-based process isolation** — hardware memory protection, the thing real
  OS-9 on a 6809 could never do
- Keep the FreeRTOS build alive as a reference oracle for behavioural diffs

**Done when:** the same modules, unchanged, run on the native kernel with WiFi
working.

*Rough size: this is the real project. Everything before it is preparation —
useful, working preparation, but preparation.*

---

## Sequencing notes

- **Phases 0-4 never need rewriting.** They sit above the KAL, so phase 7
  replaces the floor beneath them without touching them.
- Phase 6 before phase 7 is deliberate: validate the I/O design under a working
  network stack before you also have to debug your own scheduler.
- The layering check from phase 0 is what makes phase 7 possible at all. If it
  is ever disabled "just this once", the project quietly becomes a rewrite.

## Immediate next step

Phase 5. Storage: RBF, the `sdspi` driver, and `/sd0`.

The SD card shares its SPI bus with the LCD, so bus arbitration is the new
problem — the console and the card will contend. A PIPE file manager belongs
in this phase too, since it is a file manager and wants the same attention
as RBF.
