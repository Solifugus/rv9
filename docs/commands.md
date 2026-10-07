# RV-9 — The Commands

**Everything you can type, and the one thing worth knowing about each.**

This is the page for the shell. [reference.md](reference.md) is the contract a
*program* relies on; [tutorial.md](tutorial.md) teaches the machine by using
it. This one is for looking something up while you are at the prompt.

**A command is a module.** There is no built-in command table worth the name —
the shell reads a line, finds a module of that name in the store, and forks it.
`mdir` lists them all, including itself. Adding a command means adding a
module, which is why there are eighty-six of them and no `builtins` section.

Every command here is checked against the store by `tools/checkcmds.py`, which
runs in `tools/hosttest/run.sh`. A module that exists and is not on this page
fails the build.

---

## 1. The shell

Seven things, and the whole of the syntax is in
[tutorial.md §7](tutorial.md).

| | |
|---|---|
| `a \| b` | A pipe. Costs two opens and a fork, because a pipe is a file manager. |
| `< file` | Standard input from a file. Cheaper than `cat f \| x` by one process. |
| `> file`, `>> file` | Truncate, or append. |
| `cmd &` | In the background. The shell prints `[pid] name`. |
| `a && b`, `a \|\| b` | Run `b` only if `a` succeeded, or only if it did not. |
| `# …` | A comment, to end of line. |
| `shell <file>` | Run a file of commands. No prompt and no banner, because nobody is watching. |

Scripts add `set`, `$N`, `if`/`while`/`end`, `exit` and `status`. `\$` is a
literal dollar and **the backslash is not optional** — without it the shell you
are typing at expands `$N` before the line reaches the file.

**On exit status.** A command's own codes are **positive**; everything RV-9
hands back is negative. So `i2c` returning `7` is `i2c` talking, and `-6` is
the process manager saying it stopped the thing. `status` tells you which.

---

## 2. Looking at the machine

The habit this machine rewards: ask it, do not guess. All of these read
`sysinfo` and print a table, so all of them compose with the filters in §4.

| | |
|---|---|
| `mdir` | The module directory — name, type, revision, size, link count. It is itself a module in the directory it prints. |
| `procs` | The process table. It always finds itself in the list. |
| `stacks` | What each process was **given** and what it has **ever used**. The used figure is measured, not estimated — stacks are painted and the paint is looked at. |
| `budgets` | What each process is charged (`own`), what it and its children hold (`held`), and its ceiling. Watch how close `held` is to `budget` for a shell running background work. |
| `free` | Memory. *Free* and *available* are different questions and both are printed; the second is the one that decides whether the next process starts. |
| `uptime` | `up 2h 14m 8s`. |
| `log` | What the machine has been saying — an 8 KB ring in RAM. `log \| match error` is the usual form. |
| `owns` | Who has which device. One line per owner, so a device shared by five processes is five lines. `reserved` marks a claim taken from a manifest at fork. |
| `pubs` | What is being published, by whom, how often, how long ago, and how many snapshots were abandoned (`torn`). |
| `date` | Acquired from the network, and **unknown until it answers** — this board has no clock that survives power. It says so rather than printing 1970. |
| `netstat` | The link: state, SSID, address. |
| `have` | Does this machine have it? `have /i2c0` by name, `have svgwin` by driver, `have scf` by file manager. `have` alone lists what a program would usually ask about. |
| `profile` | What *this board* offers a program, as JSON. The other half — what every RV-9 offers — is `docs/target/rv9-profile.json`. |

---

## 3. Files and volumes

The namespace is flat: a path is a device and a name. There is no `makedir`
and there cannot be, which is recorded in the roadmap rather than left to be
discovered.

| | |
|---|---|
| `dir [/dev]` | List a volume. Falls out of a directory being a file — opening a block device with no filename gives you its records. |
| `df /sd0` | How much room. Counted from the bitmap when asked, so a large card takes a moment. |
| `info <path>` | What one path is. Works on things `dir` will never list, because a device is a path too. |
| `cat <file>` | To standard output. `cat picture.svg > /w0` is the point — a file reaching a device needs no new verb. |
| `copy <from> <to>` | Between volumes as readily as within one. |
| `move <from> <to>` | Copy then remove — RBF has no rename, and across volumes there would be nothing to rename. The original goes only after the copy has closed. |
| `del <file>` | Remove it. |
| `dump <file>` | Hex and printable characters, sixteen bytes a line. On a board this earns its place: a module header, a sector, a packet that arrived wrong. |
| `format /sd0 yes` | Empty a volume. **Two words, because there is no undo** — without `yes` it says what it would destroy and refuses. |
| `ed <file>` | A full-screen editor. Arrows, `^S` save, `^X` quit, `^K` cut. The same binary edits in a 236-column SSH window and on the panel at thirty by eight, because it asks the path how big it is. |
| `load <file.mod>` | Add a program to the running system. The store stops being something you reflash and becomes something you add to. |

---

## 4. Filters

Seven, and no more. Each reads standard input, so each composes; each
**refuses rather than answering with part of the truth**.

| | |
|---|---|
| `match <text>` | Lines containing it. Plain text, not a pattern language. |
| `count [lines\|words\|bytes]` | How much came through. Name one to get the number alone. |
| `first [n]` | The leading lines, default ten — and then it **stops**, which closes its input and tells the stage upstream that nobody is listening. |
| `last [n]` | The trailing lines. It keeps a ring of exactly as many as asked. |
| `field <n>` | One column, counted from **one**, because nobody counting columns on a screen starts at zero. |
| `sort` | 128 lines. More is **refused, never silently partial**. |
| `unique` | Drops a line repeating the one before it — adjacent only, so usually after `sort`. |

The canonical shape:

```
rv9> mdir | field 2 | sort | unique
```

Three more that are not filters but live with them:

| | |
|---|---|
| `echo <words>` | Writes the rest of the line. It has no idea whether stdout is the console, the panel, a pipe or a socket. |
| `calc A + B` | Arithmetic. `+ - x / %`. Writes the answer and nothing else, so `set N = calc $N + 1` works. |
| `compare A lt B` | `eq ne lt le gt ge`. Returns 0 when true, so it is what `if` and `while` are built on. |

---

## 5. Devices

A device is a path, so most of these are a thin conversion between text and
what the device carries. A control loop skips the conversion and writes the
value.

| | |
|---|---|
| `pin <n> [0\|1]` | Read or set a GPIO. **A pin keeps its level after this exits** — deliberately, and the opposite of `pwm`. |
| `pwm <pin> <duty> [hz]` | Drive a PWM output. Duty is raw, 0 to the range the device reports. **Stops when the command exits**, because an actuator left running because a program finished is a bad way to find out. At the default 50 Hz and 14 bits a hobby servo wants roughly 820 (1 ms) to 1640 (2 ms). |
| `adc <channel> [samples]` | Raw conversions, not millivolts — calibration belongs to the board and the sensor, not the converter. |
| `temp [n]` | Die temperature. With a count it reports the range, which is the part that says whether something is heating up or merely warm. |
| `i2c scan` / `i2c <addr> <reg> [count]` | The two-wire bus. Addresses in hex as a datasheet writes them or decimal as a script does. |
| `range <pin> [count] [listen]` | Distance from a three-pin sonic ranger, or any pulse width. `listen` measures pulses without triggering. |
| `backlight [0..100]` | The panel. The largest continuous draw on this board, and turning it down costs nothing else — the console works at any brightness, including none. Not remembered across a reset. |

---

## 6. The panel

`/term` is a text console on the glass; `/w0` is a window you write SVG to.
Both share one panel, and **whoever painted last is what you see**.

| | |
|---|---|
| `screen [/term]` | Proves a program can address a screen it knows nothing about. The same code draws the same picture on the panel and in a terminal, and never finds out which. Also reports the physical size where the device knows it. |
| `pic [> /w0]` | A picture. With no redirection it prints the SVG instead, which is the whole demonstration — the program does not know what a panel is. |
| `chart [> /w0]` | Gridlines, a filled area, cubic curves, a donut with a real hole, and labels. |
| `gauge [n]` | A number that moves, redrawn continuously, reporting frames per second. What a small panel on a machine is actually for. |
| `wstat [secs] > /w0` | Die temperature, heap and uptime, repainting **only the rows whose reading changed** — a tick on which nothing changed writes nothing at all. |
| `wfont > /w0` | The same text at seven sizes, to find the legibility floor by looking. **Measured 2026-10-04: font 16 is the comfortable floor; font 8 is readable but needs squinting.** |
| `flick > /w0` | Times a tap acknowledgement: a button drawn, flashed, and drawn again. |
| `wflick > /w0` | The same three draws, generated from a widget tree instead of written as string literals. |
| `wcompose > /w0` | Two independent widget stacks in one document over one background, with one of them repainted alone. |

| `taps` | Every touch on the glass, one line each: kind, finger, position, time. `taps 20` for twenty seconds. |

**`taps` is how you find out whether touch is wired up right.** A driver that
reports nothing and one that reports mirrored coordinates look identical from
the outside. Press the top-left corner: the numbers should be small.

**Writing a picture to the panel wants a bigger stack than you expect.** The
renderer runs on the *writer's* stack, so a program that draws pays for the
rasteriser out of its own — see `modules/pic/build.conf` and
`modules/wflick/build.conf`, which both say so and both have the measurement.

---

## 7. Network

| | |
|---|---|
| `wifi <ssid> <password>` | Associate. Credentials are typed by whoever owns the network and are in no source file. |
| `scan` | Visible access points. Worth having here specifically: the C5 has a 5 GHz radio and most ESP32s do not. |
| `netstat` | State, SSID, address. |
| `fetch <host[:port]> [path]` | An HTTP GET, with no sockets, no DNS call and no connect — it builds a path name, opens it, writes a request and reads the answer. Headers to stderr, body to stdout, so `fetch host /x.mod > /r0/x.mod` puts exactly the bytes on the volume. |
| `passwd <password>` / `passwd show` | The login password, and the host key fingerprint. Goes to `/sshcfg`, which is the SSH driver bound so that opening it does not wait for a login. |
| `authkey` / `authkey list` / `authkey clear` | Trust a public key, read from stdin because a key line is longer than an argument should be. No Ed25519 in mbedTLS as shipped — use ECDSA P-256 or RSA. |
| `sshd` | A shell over SSH. |
| `rshd` | The same over a plain socket, on port 2300. `sshd` is this with one string changed, which is the argument for where the protocol was put. |

---

## 8. Processes and real time

| | |
|---|---|
| `kill [-f] <pid>` | Ask first, then insist. Asking is what lets a program close its paths and put its terminal back; `-f` does none of that. |
| `sleep 2` / `sleep 250ms` | Seconds by default, because that is what somebody typing it means. Exists for scripts, which cannot otherwise wait for anything. |
| `rt` | What the machine has already promised: slots, utilisation, and the declared/measured/unaccounted split. |
| `rt <module> [period]` | **Ask** for a module to be admitted to the real-time class. Admitted, not started — and a refusal names which loop would miss and by how much. |
| `pub <NAME> <values…>` | Publish a set of values once. The smallest thing that can be on the writing end of a cell. |
| `watch <NAME>` | Print a published value every time it changes. Blocks against a **sequence number**, not an edge, so a publication landing between two waits is still seen. |
| `shell [file]` | The command interpreter itself — an ordinary module with no privileges. |

---

## 9. Demonstrations, and things that break on purpose

These are not everyday commands. Each exists to prove one claim on real
hardware, because a mechanism nobody has watched fail is a claim rather than a
feature.

**The loader and the ABI:** `hello` (checks everything the module ABI
promises), `greet` (writes through the full I/O stack by two routes),
`tiny` (how little stack a process can have), `chaintest` (becomes another
module without becoming another process), `downloaded` (a program that was
never flashed — fetched, `load`ed and run).

**Storage and the network:** `filetest` (proves RBF works by verifying rather
than assuming), `nettest` and `netecho` (NFM over loopback, and neither module
contains the word *socket*).

**Failure, caught:** `smash` (runs off its stack on purpose, to exercise the
guard), `deaf` (ignores every signal, so `kill` has something to insist on),
`forkbomb` (starts processes until refused, then cleans up — stopped by its own
budget, not by the machine running out), `runaway` (a real-time loop that stops
waiting), `hold` (holds declared exclusive devices and dies holding them, so
RV-9 has to park them).

**Scheduling, under load:** `worker` (CPU-bound, for proving starvation happens
and then proving it stops), `noise` (holds the locks a real-time process might
want — flash writes and forks — to be run *underneath* a latency measurement),
`fastloop` and `heavyloop` (the pair one priority cannot serve and two can),
`lateloop` (misses a deadline on purpose; `spin` never finishes at all).

**Measurement:** `control` (a PI loop with honest jitter reporting),
`sonar` (real-time ranging, publishing `/pub0/DISTANCE`), `edgegen` (toggles a
pin so something else has edges to react to), `evlat` (interrupt to real-time
process), `iolat` (how long a control loop waits to move a pin).

**Declared meaning:** `st-watch-mm` agrees with `sonar` about millimetres and
is admitted; `st-watch-m` wants metres and is **refused at fork**. They exist
as a pair so the refusal is demonstrated against a passing control rather than
asserted on its own.

---

## Licence

Apache License 2.0 — see [LICENSE](../LICENSE) and [NOTICE](../NOTICE).
