# RV-9 memory model

Measured on an ESP32-C5 (Waveshare LCD-1.47), ESP-IDF v6.0.3, native
kernel, 2026-09-12. Every number here was taken from the running board
unless it says otherwise. Where something is computed from `sizeof` rather
than observed, it says so.

The reason for measuring rather than estimating: the interesting figures
turned out to be an order of magnitude away from the intuitive ones in
both directions. Stacks are eight times larger than anything uses; WiFi is
half the machine.

## The whole budget

| | bytes |
|---|---|
| heap available when RV-9 starts | 198,112 |
| module directory | 4,204 |
| devices (all of them) | 86,700 |
| system daemons (`rshd`, `sshd`) | 19,112 |
| shell, console paths, boot demos | ~35,000 |
| **free, idle, everything running** | **~53,000** |

ESP-IDF has already taken its share before that first figure: the chip has
320 KB of SRAM and RV-9 sees 198 KB of it.

## Fixed OS RAM

Costs paid once, at boot, and never returned.

| item | bytes | note |
|---|---|---|
| kernel globals + scheduler structures | ~3,000 | `s_threads[]` is static, not heap |
| module directory | 4,204 | 64 bytes per module, 51 modules |
| device tables | 76 per device | `rv9_dev_t`, plus driver state below |
| **WiFi + lwIP** | **52,376** | attaching `/n0`. Half of everything. |
| filesystem structures (`/f0`, RBF) | 4,976 | mount, bitmap, directory cache |
| RAM disk (`/r0`) | 17,168 | 32 sectors × 512, a build choice |
| console (`/term`) | 8,048 | grid, attributes, 3,200-byte row strip |
| serial console (`/uart0`) | 1,228 | |
| SSH device (`/ssh0`, `/sshcfg`) | 1,456 | the session itself is dynamic |
| window (`/w0`) | 244 | its buffers are dynamic |
| pins, PWM, ADC, temperature | ~320 | four devices |
| RT scheduler structures | static | `s_rt[4]`, in `.bss` |
| interrupt structures | ESP-IDF's | not visible to RV-9 |

**The two levers are enormous and both are choices, not requirements.**
Dropping WiFi returns 52 KB. Dropping or shrinking the RAM disk returns up
to 17 KB. A vehicle build with neither starts from about 122 KB free
rather than 53 KB — more than double.

## Per-process RAM

| item | bytes | note |
|---|---|---|
| process descriptor | 192 | `sizeof(rv9_proc_t)` |
| CPU context | 56 | 14 words, saved on the thread's own stack |
| **stack** | **8,192 default** | the whole story; see below |
| statics (heap/data) | declared per module | 8 to 3,840 across the current set |
| module image | 200–3,000 | copied into executable RAM to run |
| path table | ~44 | 8 slots, `RV9_MAX_PATHS` |
| open file (per path) | 56 | `rv9_path_t` |
| — readable SCF path, extra | 520 | line buffer, `LINE_MAX` 512 |
| — network path, extra | ~24 | plus lwIP's own socket |
| RT metadata | 0 | a slot in the static `s_rt[4]` |
| device ownership | 0 | a reference count on the device |
| pipes, message queues, failsafe | — | not implemented |

**Measured total for a real daemon: 9,556 bytes.** Of which 8,192 is stack.

### Stacks are the whole cost, and they are eight times too big

Stacks are painted at creation and can be scanned, so this is measured
rather than guessed — `stacks` reports it:

```
pid  name             size   used   spare
8    stacks           2048    440    1608
7    shell            8192   1040    7152
6    sshd             8192   1260    6932
5    rshd             8192   1112    7080

given 26624 bytes, used 3852, spare 22772
```

**86% of the stack allocation is never touched.** And `sshd`'s 1,260 bytes
includes the entire SSH handshake — key exchange, signature, channel
setup — because that runs in the opening process's context.

A module may now declare `stack_size` in its `build.conf`; the default
remains 8,192 only because nothing had ever measured what was needed. A
trivial program runs in 256 bytes, but that is not a recommendation:
there is **no stack overflow detection**, so "it did not crash" is not
evidence. Size against the measured high-water mark with real margin —
2,048 is comfortable for everything here.

For a language whose programs are processes, this is the number that
decides concurrency. At 8 KB, 53 KB of free heap holds six. At 2 KB it
holds twenty-four.

## Dynamic / shared RAM

Taken while in use and returned afterwards.

| item | bytes | when |
|---|---|---|
| SSH session | ~10,500 | while connected; one at a time |
| window buffers (`/w0`) | ~11,900 | while open: document 4,096, band 5,120, coverage 640, points 2,048 |
| filesystem buffers | in the mount | RBF holds its structures at mount |
| network packets | lwIP's pools | inside the 52 KB above |
| shared libraries | — | `RV9_MOD_LIBRARY` exists; nothing links against one yet |
| language runtime | — | not yet |

Both large items are allocated on open and freed on close, which is why
the window costs nothing when nobody is drawing.

## Reserved

**There is no reserve. This is the gap that matters most.**

Nothing sets memory aside for interrupts, real-time work, or failsafe, and
nothing refuses an allocation to protect them. Any process may allocate
until the machine dies, and the machine dies badly: running the window,
an SSH session and a control loop together exhausted the heap and ESP-IDF
aborted inside the WiFi PHY —

```
ESP_ERROR_CHECK failed: ESP_ERR_NO_MEM at phy_track_pll_init
abort() was called
Rebooting...
```

— which is a reboot, in a layer RV-9 does not own, triggered by an
unrelated component's allocation failing. On a vehicle that is the whole
system stopping because a display wanted a buffer.

What is missing, in the order it would be worth building:

- **A floor.** A reserve below which ordinary allocation fails and says so,
  leaving enough for the kernel, interrupts and an orderly shutdown.
- **Failure that is survivable.** `rv9_alloc` returning NULL is handled in
  most places; ESP-IDF's `ESP_ERROR_CHECK` aborting is not, and RV-9 cannot
  catch it. Keeping well clear of the edge is the only defence available.
- **A per-process limit**, so one program cannot take the machine down.

## What this means for a language runtime

- **CPU is not the constraint.** A 50 Hz control loop executes in 1–2 µs on
  a 240 MHz core — 0.01% of its budget — while the panel redraws thirteen
  times a second beside it without disturbing it.
- **Memory is, and it is mostly stacks and WiFi.** Both are choices.
- **Concurrency density is a stack-size decision**, and the measurement to
  size it by now exists.
- **There is no safety net.** A runtime that over-allocates does not get an
  error; it gets a reboot from underneath. Until there is a reserve, the
  runtime should hold its own ceiling and stay well inside it.

## Reproducing these numbers

- `free` — heap free, low water, executable free
- `stacks` — per-process stack given and used
- `procs`, `mdir` — process and module inventory
- Device costs came from a temporary probe in `rv9_io_attach_from_modules`
  logging heap between attachments; not kept, easily re-added.
