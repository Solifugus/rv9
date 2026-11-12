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

**At the prompt**, the line edits: ↑ and ↓ walk back through the last thirty
or so commands of this session (↓ past the newest gives back what you were
typing), ← and → move within the line, Home/End or Ctrl-A/Ctrl-E jump to
either end, Delete and Backspace remove, Ctrl-U clears the line and Ctrl-C
abandons it. The same over SSH, on the serial console and on the panel —
the cursor is moved with backspaces, never escape codes. Scripts read lines
as they always did.

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
| `budgets` | What each process is charged (`own`), what it and its children hold (`held`), and its ceiling — then the same held and ceiling for external memory (`ext`, `-` for none declared). Watch how close `held` is to `budget` for a shell running background work. |
| `free` | Memory. *Free* and *available* are different questions and both are printed; the second is the one that decides whether the next process starts. |
| `uptime` | `up 2h 14m 8s`. |
| `log` | What the machine has been saying — an 8 KB ring in RAM. `log \| match error` is the usual form. |
| `owns` | Who has which device. One line per owner, so a device shared by five processes is five lines. `reserved` marks a claim taken from a manifest at fork. |
| `pubs` | What is being published, by whom, how often, how long ago, and how many snapshots were abandoned (`torn`). |
| `date` | Acquired from the network, and **unknown until it answers** — this board has no clock that survives power. It says so rather than printing 1970. |
| `netstat` | The link: state, SSID, address. |
| `get <command>` | A command from another machine, kept on this one: the module the path finds on a peer, copied to the first place on the path that is this machine's (`/sd0/cmd/`), and the files it reads from `files/` beside it. Kept only if it verifies, is signed by a key in `/f0/trust` and can run here; each file only if it hashes to what the module's `file` tag names. A file here of another build is left alone and said. |
| `on <peer> <command> [arg]` | A command run on another machine, as if here: its output and errors come back as it writes them, its status is `on`'s, and input is sent on from a pipe or a file (`dir /f0 \| on p4 sort`). The peer runs it only if this machine is in its `/f0/callers`, and what it runs is its own, by its own path. You seldom type it: a command the path finds on a peer that this machine cannot run (`listen` on a C5, for the FPU) is run there by itself. Killing it stops the command there. |
| `peers` | The other boards: each one heard on the network (its address, how long since it was heard, the start of its id, and its httpd's and execd's ports, if any), then the lines of `/f0/peers`, which win. A name heard with an id other than the one first heard with it is shown `refused` and not used; `/f0/known` keeps the first ids. This board's name is `/f0/name`, else its chip and the end of its MAC. |
| `ident` | This machine's id, 32 hex digits, which a peer lists it by in its `/f0/callers` to let it run commands there: `echo c5 <id> >> /f0/callers` on the peer. Names the SSH host key; the key never leaves. |
| `have` | Does this machine have it? `have /i2c0` by name, `have svgwin` by driver, `have scf` by file manager. `have` alone lists what a program would usually ask about. |
| `can <command>` | Could this machine run it, and if not, why not. Every need its manifest declares, met or not: the devices and instructions it cannot run without, devices it only uses for some of what it does, the modules it starts, the files it reads, external memory for its arena. Ends `yes`, `no: it would be refused`, `no: it would start, and lack what it needs`, or for a peer's command this machine would refuse, `no: not here; asked for, it runs on p4 (on p4)`; the status is 0 for yes. Finds the command as running it would, in the store or on the module path. |
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
| `load <file.mod>` | Add a program to the running system. The store stops being something you reflash and becomes something you add to. It stays in RAM until restart; a module in `/sd0/cmd/` or `/f0/cmd/` needs no `load` at all — typing its name finds it, and its memory is given back when it ends (reference §5, the module path). |

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
| `i2c scan` / `i2c <addr> <reg> [count]` / `i2c <addr> <reg> = <byte>...` | The two-wire bus. Addresses in hex as a datasheet writes them or decimal as a script does. A register number above `0xff` is sent as two bytes, high first, which is what sixteen-bit register maps expect. The `=` form writes, because a bus you can only read is a bus you cannot bring a chip up on. |
| `range <pin> [count] [listen]` | Distance from a three-pin sonic ranger, or any pulse width. `listen` measures pulses without triggering. |
| `rec <file> [seconds]` | Record both microphones to a raw file: 16 kHz, two 16-bit channels, MIC1 then MIC2. Records into a 1 MB external arena (16 s) and writes afterwards, so a flash stall cannot drop samples — about 90 KB/s to `/f0`, more to `/sd0`. Says how loud each microphone was, and says plainly when every sample was zero. P4 only. On a PC: `aplay -f S16_LE -r 16000 -c 2`. |
| `play [-1] [-v dB] <file>` / `play beep [s]` | Through the speaker. `-1` for a one-channel file, such as `arecord -c 1` makes; `-v` sets the volume first, −95 to +32 dB (the default is −10). `beep` is 440 Hz, and with `rec` running beside it, is how to test the microphones without talking. |
| `play tone <Hz> [ms]` / `play notes <notes>` | A pitch (20 to 7,900 Hz, default half a second), or a tune struck like a piano: note names with `#` or `b` and an octave (4 is middle C's), `/1 /2 /4 /8 /16` for how long and `.` for dotted, `R` a rest, `T<bpm>` the tempo (default 120). Octave and length carry on from the note before: `play notes C4/8 E G C5/2`. |
| `listen [seconds]` / `listen -c [seconds]` / `listen -f <file>` | Speech to text, with Whistle's engine in the module itself. Reads the 17 MB model from `/sd0/whistle.cact` (five seconds), beeps, records MIC2 -- the more sensitive capsule -- for four seconds by default and at most sixteen, and prints what it heard and how long it took. `-c` goes on listening, beep after beep, until it is killed; `-f` takes a one-channel 16 kHz recording instead, up to sixteen seconds of it. Its 27.5 MB is given back when it ends. Within three seconds of a `say` there is not room for it yet: `/tts0` is still holding its voice. P4 only: it needs the vector unit and the FPU. |
| `say [-v dB] [-o <file>] [-t] [words]` | Speak, through the speaker: the words, or with none, standard input to its end (`date \| say`, `say < /sd0/note.txt`). The first sentence is heard about a second after asking while the rest are still being worked out, and all of it is made faster than it plays. 3 dB louder than the voice comes, which slt has room for, without touching the speaker volume `play` shares; `-v` sets that volume first, as `play`'s does; `-o` puts the speech in a file instead, 16 kHz mono 16-bit, which `play -1` plays; `-t` says afterwards how long the speech was, how long until the first sound and how long making it took. The voice is CMU Flite's slt, `/sd0/slt.flitevox`, through `/tts0`; the first use after a restart reads it, about two seconds. A typed line holds about 250 characters; a longer text goes in by `<` or a pipe. P4 only. |
| `snap [-q 1..100 \| -r] <file>` | Take a picture with the camera and save it: an 800x640 JPEG, quality 80 unless `-q` says, or with `-r` the raw RGB565 pixels (1 MB). Says how big it is and how long starting the camera, taking and compressing took — about 70 ms, 40 ms and 5 ms. P4 only. |
| `view [seconds]` / `view -1` / `view <file>` | What the camera sees, on the screen: live until the screen is touched (or for that many seconds; 30 with no touch screen), one picture with `-1`, or a picture `snap -r` saved. Scaled to fit — 750x600 on the P4's panel — and about 8.6 frames a second live. P4 only. |
| `mystify [seconds] [trail]` | The old screen saver, in SVG on `/w0`: two shapes bouncing round the window, each trailing fading copies of itself (seven by default) as their colours drift round the wheel. Until the screen is touched, or for that many seconds. About 8 frames a second with the full trail on the P4's panel, 10 with five; it says what it managed when it stops. |
| `detect [faces\|people] [-v] [-c] [-f <file>] [count]` | Find the people, or with `faces` the faces, in front of the camera: one line per find, `person 0.88 179,251 317,616` or `face 0.61 347,113 639,491 eyes 433,268 548,259 nose 494,298 mouth 449,397 533,394` — the score, the box in the camera's 800x640 pixels, and for a face five points — or `nobody` / `no faces`, then how long it took. `-f` reads a picture `snap -r` saved instead. The models are files on the card: `/sd0/pico.rv9m` for people, `/sd0/face1.rv9m` and `/sd0/face2.rv9m` for faces, Espressif's detectors converted by `tools/mkpico.py`; nothing of them is in the firmware. People about 90 ms a picture with the P4's vector unit, faces about 22 ms; `-c` forces plain C, `-v` adds a hash of the outputs and, for people, where the time went. Same answer either way, byte for byte, and the same as ESP-DL's. The face detector wants a face seen more or less from the front: looking up at one from below, it finds nothing — Espressif's too. |
| `detect who [count]` / `detect enroll <name> [count]` / `detect known` / `detect forget <name>` | Whose faces they are. `enroll` takes five pictures (or `count`) half a second apart — turn your head a little — and keeps one averaged signature per name in `/sd0/faces.db` (1 KB each, 64 at most; enrolling a name again replaces it). `who` prints `face matthew 0.83 <box>` for a face whose signature's cosine with somebody's is above 0.5, ESP-DL's line, and `face unknown <best> <box>` otherwise. The signature model is `/sd0/feat.rv9m` (Espressif's human_face_feat mfn, 1.2 MB); about 130 ms a face on top of finding it. Nothing leaves the board. `-f` works here too. |
| `detect [faces\|people\|who] -p [count]` | The same, quietly, until stopped (or `count` pictures), publishing each picture's answer to `/pub0/SEEN` for other programs to act on: how many, the most prominent one's box and score, and with `who` its likeness and name — only that one is recognised, so a picture takes about 0.3 s, not 0.3 s a face. `detect who -p &` runs it in the background; `watch SEEN` shows the words. Reference §10 has the layout. |
| `welcome [-u] [minutes]` | Say hello to the people the camera knows: watches `/pub0/SEEN` and, when a known face is the main one in two pictures running, says "Hello, Matthew." through `say`, once, and not again for that person for ten minutes (or `minutes`). `-u` greets a face nobody knows too, after three pictures, at most once a minute. Run `detect who -p &` first. It never looks at a picture itself. |
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
| `wtap > /w0` | A whisker button you can actually press. Reports taps, moves, and the worst finger-to-pixels time. |
| `wslop > /w0` | Five marks, eight taps each: how far a finger lands from where it aimed — bias, scatter and drift, in pixels and millimetres. Also draws a red bar on the top rows and a cyan one on the bottom, so the first frame shows whether the picture sits where the window says. **Measured 2026-10-24: scatter 12–26 px worst, drift zero.** The "picture ~45 rows low" from that day was wslop's own: its marks and bars were laid out 64 rows tall (a whisker button with no size), not the panel. Bias wants measuring again. |
| `wscroll [rows] > /w0` | A list of ten thousand rows to drag, built a screenful at a time with whisker's `wsk_slice`. Times every frame — the repaint, and finger-to-pixels — and a tap names the row it hit. `wscroll 112` makes the pane 112 rows. **Measured 2026-11-11: 37.5 ms a frame for 112 rows (finger to pixels 42), 229 for the whole screen below the header.** |
| `wnest > /w0` | A window that is one tree, driven by whisker's toolkit: a toolbar row, a list beside a column of buttons, a status row — stacks nested three deep. Press a button and it holds; release on it and it prints the click; drag the list and it scrolls. The program only prints what `wsk_feed` tells it and repaints the rows it is given. |
| `wkeys > /w0` | Type with a finger: a field and an on-screen keyboard, built on whisker's toolkit. Letters, and a **123** key for digits and symbols (two layers, because a whole keyboard overruns the 4 KB document limit). Left, Right and Del edit at the caret; tap the field to move the caret; **Enter** prints the field; **Clear** empties it. |

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
| `wifi <ssid> [password]` | Associate, and remember it. With no password it asks for one and does not show it as typed; `""` for an open network. `wifi try` does not save; `wifi forget`. Credentials are typed by whoever owns the network and are in no source file. |
| `scan` | Visible access points. Worth having here specifically: the C5 has a 5 GHz radio and most ESP32s do not. |
| `netstat` | State, SSID, address. |
| `fetch <host[:port]> [path]` | An HTTP GET, with no sockets, no DNS call and no connect — it builds a path name, opens it, writes a request and reads the answer. Headers to stderr, body to stdout, so `fetch host /x.mod > /r0/x.mod` puts exactly the bytes on the volume. If the volume fills, it says how much it kept and fails, rather than counting what it was sent. |
| `passwd <password>` / `passwd show` | The login password, and the host key fingerprint. Goes to `/sshcfg`, which is the SSH driver bound so that opening it does not wait for a login. |
| `trust` / `trust add [line]` / `trust remove <name or id>` | The keys whose signed modules this machine runs from a file (`/f0/trust`). `add` takes the line `tools/mkkey.py` prints, as an argument or pasted. With no key, a module from the card, `/f0` or `load` runs signed or not, with a warning; with one, it must be signed by a key here. The store is trusted either way. |
| `authkey` / `authkey list` / `authkey clear` | Trust a public key, read from stdin because a key line is longer than an argument should be. No Ed25519 in mbedTLS as shipped — use ECDSA P-256 or RSA. |
| `sshd` | A shell over SSH. |
| `httpd` | This machine's modules, served to its peers over HTTP/1.0: `GET /catalog` (one line per module: name, revision, size, type, signer, SHA-256), `/cmd/<name>.mod` (from `/m0`) and `/files/<name>` (`/sd0/<name>`, only when a module here declares that it reads it, so `/sd0/faces.db` is never served). Read-only, one connection at a time. Started by init when `/f0/serve` exists, on the port written in it, else 8080: `echo 8080 > /f0/serve`. |
| `execd` | Commands run here for the peers in `/f0/callers` (one a line: a name and the id `ident` prints there), on port 2301: each caller proves who it is by signing a challenge with its key, and its command runs with three pipes for its standard paths, sent back to `on` in frames. Started by init when `/f0/callers` exists; `del /f0/callers` and nobody may. Who connected is proved, but the stream is not encrypted. |
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
RV-9 has to park them), `arena` and `arenain` (a 64 KB external and a 4 KB
internal arena, proved aligned and zeroed; `arena spawn` starts copies holding
theirs until its external budget refuses the third).

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
