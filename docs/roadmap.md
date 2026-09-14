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

**Goal:** more than one thing running, with priority scheduling and aging.

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

Since revisited:

- **Console control — done**, see design §16. Cursor addressing, sixteen
  colours, bold/underline/reverse, clear, cursor visibility and "how big are
  you", all as generic setstat codes rather than escape sequences in the
  stream.
- **The driver decides; SCF covers for it.** `/term` implements the codes
  natively — it is a framebuffer with a font renderer, not a terminal, and
  has no parser. A driver that declines gets the ECMA-48 sequence written
  for it instead. `screen` and `screen /term` are the same binary.
- **Per-cell colour on the panel**, with the attribute plane walked
  columns-outer so the anti-aliasing ramp is rebuilt once per run of equal
  colour and not once per scanline. Fixed a latent bug on the way: colours
  were stored pre-byte-swapped, which every blend would have got wrong the
  moment a second colour existed.
- **Raw input — done**, `RV9_SS_RAW`, see design §18. A read hands over
  keystrokes rather than a line: unechoed, unfiltered, escape sequences
  intact. Without it an arrow key never reaches a program, because the
  line discipline drops the escape. It lives on the path rather than the
  device, so one program cannot leave another's shell strange.
- **Size over a wire — since fixed by SSH**, phase 9. `pty-req` carries the
  client's real dimensions, so the 80×24 guess now applies only to a
  session with no pty, which is the honest answer there. A resize
  *signal* is still missing: the driver learns about `window-change` and
  has no way to tell a running program.

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
- **`ed` — a full-screen editor, done**, see design §18. Arrows, home/end,
  page up/down, ^S save, ^K cut, ^X quit. Three kilobytes. The same binary
  runs at 236 columns over SSH and 30x8 on the panel, asking the path how
  big it is on every redraw — so a resize reflows at the next keystroke and
  no resize signal is needed.
- **The shell waited thirty seconds for a command and then printed a prompt
  anyway — fixed.** It had not stopped the command, it had put two
  processes on one terminal. Nothing noticed until a program existed that a
  person sits inside for longer than that.
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

**Interrupt-safe primitives — done**, see design §20.
`rv9_sem_give_from_isr` and `rv9_queue_send_from_isr` work under the native
kernel now. An interrupt raises the count and leaves a note; the kernel
does the waking in thread context, so the wait queues never have to be
interrupt-safe. Latency is a scheduling round rather than an interrupt,
which is right for a semaphore and nowhere near enough for real-time work
— that still goes through `kal_rt.c` and measures in microseconds.

Both also carry `RV9_MUST_CHECK`: a caller that ignores a refusal fails to
build. That was the actual bug behind a fourteen-second boot.


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
- **PMP-based process isolation** — hardware memory protection, the thing the
  8-bit systems of that era could never do
- Keep the FreeRTOS build alive as a reference oracle for behavioural diffs

**Done when:** the same modules, unchanged, run on the native kernel with WiFi
working.

*Rough size: this is the real project. Everything before it is preparation —
useful, working preparation, but preparation.*

---

## Phase 8 — Real-time  ◐ periodic and event-driven processes done

Driven by a real use: RV-9 as a target for autonomous systems, where the
control layer is late if it is late.

- **Periodic real-time processes — done.** `rt <module> [period_us]`, or
  `rv9_proc_fork_rt()`. Preemptive, above everything, released by a
  hardware timer. Measured: 27 µs worst jitter at 1 kHz, 7 µs at 5 kHz,
  no overruns, with WiFi up and the shell running.
- **Timing is reported to the application** via `env->rt_stats()`, because
  a control loop that cannot see its own jitter cannot report that it has
  stopped being trustworthy.
- **Aperiodic (event-triggered) real-time processes — done.**
  `env->rt_declare_event(event_id, min_interval_us)`, released by an
  interrupt instead of a timer, and waiting in the *same* `rt_wait()` — a
  control loop should be able to change what wakes it without being
  rewritten. Measured pin-to-process: 7 µs idle, 15 µs on the first run
  after boot while the radio writes NVS, 13 µs under `noise`, 41–51 µs
  alongside a second real-time process. No events coalesced, none lost.
- **Lateness is measured against the world, not against us.** The interrupt
  handler stamps the clock before waking anyone, and keeps the *oldest*
  unserviced stamp when several arrive — stamping the newest would subtract
  the part of the delay that was our fault.
- Not done: an edge arriving during a flash erase is not measured, because
  every event source available without external wiring is software and
  stalls in the same window. Needs a signal generator or a peripheral output
  routed to an armed pin.
- Not done: a scheduling policy *among* real-time processes. They share one
  host priority and round-robin. The declared periods and inter-arrival
  bounds are already recorded, which is what rate-monotonic or EDF needs.
- **Bounded-latency I/O — done.** A path lookup was a lock plus two list
  walks; it is now a load from task-local storage, filled at `open`. Open
  and close stay unbounded on purpose: a control loop opens what it needs
  before its first period.
- **The real-time path is resident (`RV9_RT_CODE`) — done, and it was the
  whole story.** Every write to flash switches off the cache that makes
  flash readable, so code in flash is *gone* while the radio stores its
  calibration data. Measured before: 199 ms lost in one piece, first run
  after every boot. After: worst write 13 µs, worst wakeup 9 µs late, no
  periods missed, over 20,000 activations. Reaches `/gpio`; PWM and the
  ADC are not resident and are not claimed to be.
- **Missed periods and lateness are measured against the clock**, not by
  draining the release semaphore (which held eight) and not as the
  remainder after whole periods (which cannot exceed one). A 199 ms stall
  used to report "seven overruns, 199 µs late".
- **Not bounded, and now measured: being scheduled while another process
  writes flash.** Under `noise`, the write itself held at 13 µs and the
  worst wakeup was 82 ms — one flash erase. Flash suspend/resume is the
  known lever and is deliberately not enabled; see design §14.
- **`rv9_kal_self_thread()` — a real-time process is no longer told it is
  the shell.** `rv9k_self()` answers with whatever RV-9 thread a host task
  preempted, which handed real-time processes another process's pid, path
  table and priority-inheritance identity. One crash in eight runs, in a
  place unrelated to the cause. See design §14.
- **Priority inheritance on `rv9_lock_t` — done.** In both schedulers: the
  host mutex lends to the task so the kernel runs, and the lock boosts the
  holding RV-9 thread so the kernel runs *it*. Measured at 504 ms → 397 ms
  for a real-time waiter behind a low-priority holder and a medium hog.
- The three-actor demonstration is disabled. Chasing it found the thing
  worth knowing: inheritance from a real-time waiter elevates the *whole*
  cooperative kernel to real-time priority for the duration of the hold.
  Keep critical sections shared with real-time work short. See design §12.

## Phase 9 — Working on it remotely  ✅ done

- **`rshd` — a shell over TCP, done.** `nc <ip> 2300`. Roughly forty lines,
  because a connection is a path and a child inherits its parent's paths.
  Local and remote shells run simultaneously as ordinary processes.
- **Not secure, and not pretending to be.** Anyone who can reach the port
  gets a shell. Kept alongside SSH because it is two lines of code and
  useful on a bench; SSH is what you would leave running.
- **SSH — done**, see design §17. `ssh <anything>@<ip>` with the password
  from `passwd`. curve25519-sha256, an ecdsa-sha2-nistp256 host key the
  board generates for itself and keeps in NVS, aes256-gcm@openssh.com. All
  through PSA (mbedTLS 4 dropped the legacy API), all hardware-accelerated,
  about 23 KB of flash.
- **It is a character device, not a daemon.** `/ssh0` is SCF over an `ssh`
  driver, so the session gets line discipline, echo and cursor addressing
  the way the serial console does — which is what a client in raw mode
  needs. `sshd` is `rshd` with one string changed.
- **The transport is a path** (`/n0/listen/22`), so there is no socket code
  in the SSH implementation at all.
- **Window size, fixed.** `pty-req` and `window-change` carry the client's
  real dimensions, so `RV9_CON_GS_SIZE` over ssh answers with the truth
  instead of the 80x24 guess §16 had to make.
- **Public key authentication — done.** `/f0/authkeys` in the ordinary
  authorized_keys format; `authkey` to add one, `list`, `clear`. The key
  must be authorized *and* the client must prove it holds the private half
  — either check alone lets anybody in. `ecdsa-sha2-nistp256` (verified in
  hardware) and `rsa-sha2-256`/`512`. **Not Ed25519**: mbedTLS as ESP-IDF
  ships it has no EdDSA at all, which matters because ssh-keygen defaults
  to it.
- Not done: rekeying, more than one session at a time, job control.
- **Power save must stay on.** `WIFI_PS_NONE` makes this board lose its
  access point -- reason 200, beacon timeout -- and turning it off is what
  caused the "short sessions fail" hunt below. With ESP-IDF's default,
  eleven sessions in twelve succeed. Latency is the price and it is worth
  paying.
- **Short sessions failing — closed, and self-inflicted.** Turning power
  save off is what broke it. With ESP-IDF's default restored, twenty
  scripted connect-run-disconnect sessions in twenty succeed. The hours
  spent on SSH found six real bugs and none of them was the cause; the
  cause was a "improvement" to the radio made an hour earlier and never
  measured against.
- **No post-quantum key exchange**, which OpenSSH 10 warns about and is
  right to. `mlkem768x25519-sha256` needs an ML-KEM implementation and
  there is none in this mbedTLS — a project, not a setting.
- **A resize signal is still missing.** The driver knows when the window
  changed and has no way to tell a running program. `RV9_SIG_WINCH` is the
  shape; an `RV9_CON_SS_SIZE` setstat is worth having now too, since
  something finally exists that could call it honestly.

## Phase 10 — Hardware  ◐ pins, PWM, ADC and the window done

- **`/gpio`, `/pwm0`, `/adc0` — done**, under a new `pio` file manager: a
  third driver shape after character and block, carrying values rather than
  bytes. `pin`, `pwm`, `adc` utilities convert text at the shell.
- USB pins (13, 14) are reserved: a driver that can disconnect the operator
  should refuse to.
- **`/tsens` — the die temperature, done.** 45-46 C with radio and
  backlight on; the input a thermal-throttling decision needs.
- **Backlight brightness — done**, as a `setstat` on `/term` rather than a
  descriptor option: the useful time to dim a display is while running.
  Not persisted, deliberately.
- **`/w0` — a graphics window, done**, see design §19. Write SVG to it and
  the picture appears: `pic > /w0`, `cat /f0/picture.svg > /w0`.
  Presentation attributes, no CSS. About 59 ms a picture.
- **The panel now belongs to neither device.** `/term` and `/w0` both want
  the one ST7789, so it moved into `panel.c` — brought up by whoever asks
  first, handed out as geometry and a blit under a lock.
- **`cat` — done**, finally. A file reaching a device needs no new verb
  when redirection already works.
- **`gauge` — a live telemetry display, done**, see design §21. Redraws
  continuously at 13 fps and traces a real-time task's execution time
  against its budget, with worst lateness and overruns beside it. The
  panel's blit yields instead of spinning, handing ~14 ms a frame back to
  whatever else is running.
- **`RV9_SYS_RT`** — real-time statistics through `sysinfo`, so a display
  can watch a control loop without being one. `rt_stats` only ever
  described the caller.
- Not done: dirty-band updates. At the few-per-second a human can act on,
  the frame rate is a non-issue; the argument is duty cycle, and it only
  bites where a quarter of the CPU matters.
- **`<path>` — done**, `M L H V C S Q T A Z`, absolute and relative, with
  subpaths, holes and `fill-rule`. Arcs by bisection rather than
  trigonometry. `chart > /w0` draws an area chart, a cubic series, bars and
  a donut.
- **Host tests — `tools/hosttest/run.sh`.** Builds the renderer natively and
  asserts against it, and renders to a PPM you can look at. The panel is the
  one part of the system no test can reach, and a picture can be
  geometrically perfect and still look wrong.
- **`<text>` — done.** The console's font, moved into a shared `font.h`.
  `font-size` and `text-anchor` inherit, so one group styles a whole axis
  of labels. Downscaled with 3x3 sampling, because an anti-aliased face
  reduced by nearest neighbour looks like gravel.
- Not done in the window: font families and weights, rotated text, arc
  x-axis-rotation, opacity, gradients, documents over 4 KB.
- Not done: I2C and SPI as devices, which is what most sensors want.
- **Interrupt-driven inputs — done.** `setstat(path, RV9_PIO_SS_EDGE, ...)`
  arms a pin; `getstat(path, RV9_PIO_GS_EVENT, ...)` says which event it
  signals on. Generic PIO codes, not gpio-specific: any device that can
  interrupt answers the same two, which is how a UART or a card will plug
  into this without a new mechanism.
- **A pin may be held by more than one process — fixed.** Opening a unit
  used to reset the pin every time, silently undoing the previous opener's
  direction, pull and arming. The reset happens on the first open only.
- **`gpio_set_level` is now genuinely resident.** Phase 8 claimed it was, on
  the strength of an ESP-IDF `noflash` mapping that is gated behind
  `CONFIG_GPIO_CTRL_FUNC_IN_IRAM` — off by default. Marking our own code
  IRAM and then calling into flash on the last instruction achieves nothing.
  The option is set; `nm` on the image is the check.
- **Bounded-latency I/O — done**, see phase 8. `/gpio` is reachable from a
  control loop with the flash cache off; `/pwm0` and `/adc0` are not, and
  making them so means driving LEDC and the ADC from registers rather than
  through ESP-IDF's drivers.

## Sequencing notes

- **Phases 0-4 never need rewriting.** They sit above the KAL, so phase 7
  replaces the floor beneath them without touching them.
- Phase 6 before phase 7 is deliberate: validate the I/O design under a working
  network stack before you also have to debug your own scheduler.
- The layering check from phase 0 is what makes phase 7 possible at all. If it
  is ever disabled "just this once", the project quietly becomes a rewrite.

## The language this is a target for

**docs/alignment.md** — notes from the compiler side, and RV-9's honest
reading of where it stands against each. The governing constraint is that
RV-9 should not grow language semantics; it should avoid decisions that stop
the compiler telling it things it could use.

Three items foreclose options if left alone, in order: the **fixed module
header** (every new field breaks the ABI; a TLV extension area stops it
recurring), **stack overflow detection** (per-program stacks already work and
are already unsafe), and a **memory floor** (until allocation can be refused,
admission control has nothing to protect). Everything else can wait.

## Memory

Measured, not estimated: **docs/memory.md**. WiFi is 52 KB and the RAM
disk 17 KB. **Stacks are right-sized**: every module declares one, sized
from its measured peak (design §37), and `stacks` reports each module's
peak since boot.

**Memory has a floor, a real-time reserve and per-process budgets**
(design §23, §35). Ordinary work is refused before a control loop is, and
a failsafe can still be applied with the reserve spent.

What remains is headroom inside an SSH session, which is still the
tightest place on the board.

## Immediate next step

**Phase 5. Storage: the `sdspi` driver and `/sd0`**, when the card arrives.

The card shares its SPI bus with the panel, so bus arbitration is the new
problem and the console will contend with it. `panel.c` already owns that
bus and serialises transfers, which is most of the answer — the driver
below it becomes a second client rather than a second owner. A PIPE file
manager belongs in this phase too, being a file manager wanting the same
attention as RBF.

### Waiting on a normal network

- ~~**Short SSH sessions lose their output on a poor link**~~ (phase 9).
  Found without one: the server sent a DISCONNECT straight after closing
  the channel, and OpenSSH exits on it before writing output that arrived
  in the same read. Fixed, with sessions also stopped being refused
  between connections (design §40). Worth re-checking on a poor link.

### Ready whenever

- **Documents larger than 4 KB for `/w0`.** The source is re-read once per
  band, so it need not be in RAM at all: re-read it from a file and the
  limit becomes storage. Wants the card, and is what maps need.
- **`RV9_CON_SS_SIZE` and `RV9_SIG_WINCH`** — a session telling a path its
  real size, and a program being told the window changed. Both want the
  SSH work finished first.
- **`/w0`:** font weights, rotated text, arc x-axis-rotation, opacity,
  gradients.
- **Phase 7 steps 3-5:** the WiFi blobs on RV-9 primitives, lwIP's
  `sys_arch`, and PMP process isolation.
