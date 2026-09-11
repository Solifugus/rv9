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
- **`/f0` — persistent storage on a flash partition, done.** RBF over
  `flashdisk`: same file manager as `/r0`, different driver. Programs
  written there are loaded at boot by `autoload()` and are commands again
  without a cable.
- **`sdspi` driver → `/sd0` — not written.** Waiting on a microSD card to
  test against. The file manager above it is already proven, so this is a
  driver-shaped hole rather than an unknown.
- Bus arbitration with the LCD is the interesting part when it comes: both
  devices share SPI, and mediating that is the I/O manager's business rather
  than each driver improvising.
- **Loading modules from a volume — done.** `load <path>` reads a module
  through the I/O manager, verifies it and adds it to the directory, so a
  program can be added without reflashing. Combined with redirection into a
  file and the network being a path, a program can arrive from anywhere:
  verified by downloading one over WiFi and running it.
- Not done: a native filesystem beyond RBF.
- **Pipes did not land.** A PIPE file manager is straightforward, but shell
  pipe syntax needs two processes with one blocking on the other's output,
  and that deserves its own attention rather than being tacked on here.

**Done when:** a module is loaded from a volume and run. Not yet — files
work, but the loader still reads only the flash partition.

---

## Phase 6 — Network  ✅ done

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

## Phase 7 — The native kernel  ◐ steps 1-2 of 5 done

**Goal:** remove FreeRTOS. The year-of-evenings phase.

- **step 1 ✅** context switch (RV32I assembly), run queues, priority with
  aging, semaphores, and tests passing on hardware. Runs as a guest inside
  one host task; threads switch cooperatively.
- **step 2 ✅** the kernel's own tick, from a timer interrupt that fires
  even with the host scheduler suspended. Time, sleeps, aging and CPU
  accounting all run on it. The kernel has no code in interrupt context at
  all -- the handler increments the counter directly, because a handler in
  flash faults when the cache is off. Preemption is taken at points a
  thread offers; asynchronous preemption needs the trap vector and comes
  with step 3.
- **step 3** own the CPU from reset; the host scheduler goes away, and the
  hardware stack guard becomes ours to program.
  **Its acceptance test already passes**: the KAL contract is now a table of
  operations with two implementations behind it, and RV-9's kernel
  satisfies all 23 checks.
  Also done, and both were prerequisites: the kernel's own allocator
  (first fit, coalescing both ways) and real wait queues, so a blocked
  thread leaves the run queue instead of spinning.
  **The whole system now runs on RV-9's kernel** (`CONFIG_RV9_KERNEL_NATIVE`):
  processes, I/O, storage, networking and the shell. FreeRTOS holds one
  task for the kernel to live in and runs the ESP-IDF drivers.
  What remains for step 3 is genuinely just boot and traps: a startup path
  that does not hand the machine to FreeRTOS, our own `mtvec`, and the
  asynchronous preemption that trap entry makes possible.
- **step 4** `wifi_osi_funcs_t`, so the radio blobs run on RV-9
- **step 5** PMP isolation
- run queues, priority aging, tick handling
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

## Phase 8 — Real-time  ◐ periodic processes done

Driven by a real use: RV-9 as a target for autonomous systems, where the
control layer is late if it is late.

- **Periodic real-time processes — done.** `rt <module> [period_us]`, or
  `rv9_proc_fork_rt()`. Preemptive, above everything, released by a
  hardware timer. Measured: 27 µs worst jitter at 1 kHz, 7 µs at 5 kHz,
  no overruns, with WiFi up and the shell running.
- **Timing is reported to the application** via `env->rt_stats()`, because
  a control loop that cannot see its own jitter cannot report that it has
  stopped being trustworthy.
- Not done: aperiodic (event-triggered) real-time processes, which is what
  a reactive layer wants — an interrupt or a message releasing a process
  with the same latency guarantee as a period does.
- Not done: bounded-latency I/O for real-time processes. Writing to a
  device from a control loop currently takes the same locks as everyone
  else.
- **Priority inheritance on `rv9_lock_t` — done.** In both schedulers: the
  host mutex lends to the task so the kernel runs, and the lock boosts the
  holding RV-9 thread so the kernel runs *it*. Measured at 504 ms → 397 ms
  for a real-time waiter behind a low-priority holder and a medium hog.
- The three-actor demonstration is disabled. Chasing it found the thing
  worth knowing: inheritance from a real-time waiter elevates the *whole*
  cooperative kernel to real-time priority for the duration of the hold.
  Keep critical sections shared with real-time work short. See design §12.

## Phase 9 — Working on it remotely  ◐ plaintext done

- **`rshd` — a shell over TCP, done.** `nc <ip> 2300`. Roughly forty lines,
  because a connection is a path and a child inherits its parent's paths.
  Local and remote shells run simultaneously as ordinary processes.
- **Not secure, and not pretending to be.** Anyone who can reach the port
  gets a shell.
- **SSH — wanted, not started.** Key exchange (X25519), a host key
  (Ed25519), an AEAD cipher (ChaCha20-Poly1305), transport framing,
  userauth and channels. mbedTLS in ESP-IDF supplies the primitives. This
  is a project of its own; the value of doing `rshd` first is that the
  plumbing underneath is now proven, so SSH is purely a security problem.
- Not done: more than one concurrent remote session, a pty/job control,
  or anything resembling line editing over the network.

## Phase 10 — Hardware  ◐ pins, PWM and ADC done

- **`/gpio`, `/pwm0`, `/adc0` — done**, under a new `pio` file manager: a
  third driver shape after character and block, carrying values rather than
  bytes. `pin`, `pwm`, `adc` utilities convert text at the shell.
- USB pins (13, 14) are reserved: a driver that can disconnect the operator
  should refuse to.
- **`/tsens` — the die temperature, done.** 45-46 C with radio and
  backlight on; the input a thermal-throttling decision needs.
- Not done: LCD backlight brightness as a descriptor option. It has been
  on at full since phase 3 and is a real contributor to board temperature.
- Not done: I2C and SPI as devices, which is what most sensors want.
- Not done: interrupt-driven inputs — a pin change releasing a process.
  That is the same mechanism aperiodic real-time needs, and doing both at
  once is the sensible way round.
- Not done: **bounded-latency I/O for real-time processes.** A control loop
  writing `/pwm0` takes the same locks as the shell, and design §12 now
  explains exactly why that matters: the hold time bounds how long the
  whole system runs at real-time priority.

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
