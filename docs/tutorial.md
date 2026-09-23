# RV-9 — A Tutorial

**Getting a modular operating system running on an ESP32-C5, and building
something on it.**

> **Status: being written.** Sections marked *To write* are outlines. What is
> written is written against the board, not from memory: every transcript
> below was copied from a real session, and where the board says something
> different from this document, the board is right and this document is a
> bug.

This is the *learning* document, and it is deliberately terse: it shows each
thing once and then points at the [reference](reference.md), which is where
the complete answer lives. When you want to know *why* something is built the
way it is, go to the [design log](design.md) — long, chronological, and it
records the mistakes as well as the results.

It is written for somebody who has to be productive quickly, including the
author six months from now, so it leans towards the things that are easy to
forget: which two commands flash which half, which pins the board has already
spoken for, and what the module rules are that the compiler will not warn you
about.

---

## 1. What you need

- **An ESP32-C5 board.** Development is on a Waveshare ESP32-C5-LCD-1.47.
  Anything with a C5 will run the kernel, processes, I/O and the shell; the
  display, microSD and pin numbers in this document are that board's.
- **ESP-IDF v6.0.3.** Earlier versions do not have C5 support in the state
  RV-9 expects.
- **A USB-C cable**, which is also the console.

Optionally, and only for later sections: a microSD card, an LED and a
resistor, a sonic ranger.

## 2. Building it and putting it on the board

Two things get flashed, separately, and understanding why is most of the way
to understanding RV-9.

```sh
. ~/esp/esp-idf/export.sh

idf.py build                            # the firmware
./tools/build_modules.sh                # the modules -> build/modules.bin

idf.py -p /dev/ttyACM0 flash            # firmware to 0x10000
./tools/flash_modules.sh /dev/ttyACM0   # modules to their own partition
```

The **firmware** is the kernel, the abstraction layer above it, and the four
managers: modules, processes, I/O, real time. It knows nothing about any
particular command.

The **modules** are everything else — the shell, every command, every device
driver, every file manager, every device descriptor. About seventy of them,
in their own flash partition, found by name at runtime.

So `./tools/flash_modules.sh` on its own is usually all you need. Changing a
command, or adding one, does not touch the firmware. Later on you will fetch
a module over WiFi and run it without flashing anything at all.

```sh
idf.py -p /dev/ttyACM0 monitor          # the console, on USB serial
```

> **If you have two Espressif boards plugged in**, `/dev/ttyACM0` may not be
> the one you mean. `ls /dev/serial/by-id/` names them by MAC address, and
> those paths are stable across reboots.

## 3. The first boot tells you more than "it started"

RV-9 runs its test suites at every boot, on the hardware, and prints the
results. This is not a debug build — it is how the system is shipped, on the
argument that a claim with no test behind it is a claim and not a feature.

```
kal 45   conform 27   mod 17   io 44   pub 38
fault 76   proc 11   sched 15   mem 13   sd 7
```

Ten suites, 294 checks. Among the things that just happened while you were
waiting for a prompt: a control loop was admitted and met every deadline
beside a heavier one; another was refused because it would have made the
first one late; a process was killed and its motor pin was parked by the
system afterwards; a fork bomb stopped at its own memory budget; and a
microSD card was written, read back and checked against what survived the
last reboot.

The one worth reading properly is `sched`:

```
--- a fast loop beside a heavy one, priority derived ---
  pass  admitting the fast loop moved the running heavy loop below it
  (bounds: fast 2500 us, heavy 38000 us)
  (fast loop's worst response 2022 us, status 0)
  pass  the fast loop met every deadline beside the heavy one
--- the same, at one priority: the control ---
  (fast loop's worst response 4294967295 us, status -13)
  pass  at one priority it answers later than when placed by deadline
```

Two identical runs, differing only in whether RV-9 was allowed to derive
priority from deadlines. In the second the fast loop missed and was stopped
(`-13` is DEADLINE). The control run exists because "our scheduler meets
deadlines" is not a measurement unless you also show what happens when it is
not allowed to.

Then:

```
RV-9 shell. Type 'help'.
rv9>
```

## 4. Commands are modules

There is almost no built-in command table. `help` and `exit` are built in
because they act on the shell itself; everything else you type is a module
that gets forked.

```
rv9> mdir
name         type    rev  size link
shell        program 15   6516 1
mdir         program 3     864 1
cat          program 2     720 0
...
```

`mdir` is in that list. It is listing the store it came out of.

Three commands worth running first, because they describe the machine
rather than doing anything to it:

```
rv9> procs
pid   par   name        state   base eff ended
35    33    procs       active  8    8
33    0     shell       active  8    8
32    0     sshd        active  4    4
31    0     rshd        active  4    4
30    0     greet       exited  8    8   status 0
```

`procs` can see itself running, at priority 8, forked by the shell at pid
33. `ended` is where a finished process says how it went — and the
difference between `status 0` and a fault written there is a distinction
RV-9 takes some trouble over; see §12 and reference §13.

```
rv9> free
heap free      53592
  largest block 20480
available      41304
  for programs 33112
  rt reserve   8192
reserved       12288
refused        3
low water      12736  (since boot; the tests spend to the floor)
```

Read that from the bottom. `reserved` is memory ordinary programs may never
have. `rt reserve` is 8 KB that only a real-time loop may take. `for
programs` is what is actually left for you. `refused` counts the allocations
that were turned down to keep those floors intact — three of them, during
the boot tests, on purpose.

> **To write:** `owns` and `pubs`, and what they show before anything is
> running.

## 5. Everything is a path

> **To write:** the path table; `dir /r0`; `cat /tsens` and why a
> temperature is a path; `pin 2 1`; `echo hello > /term`; the fact that
> `/n0/host/port` is a TCP connection you can `cat`. The point to land: a
> program that reads standard input works on a file, a pipe, a socket and an
> SSH session without knowing which it has.

## 6. Small tools that compose

> **To write:** `|` costs two opens and a fork because a pipe is a file
> manager. `<`, `>`, `>>`. The thirteen tools and the habit of refusing
> rather than answering with part of the truth — `sort` will not return your
> lines shortened, and says so. Worked examples building up to a
> three-stage pipeline.

## 7. Scripts, and letting a tool decide

A file of commands is a script, and the shell runs it with the same parser it
gives a terminal:

```
rv9> shell /r0/checkout
```

No prompt and no banner when scripted, because nobody is watching. `#` starts
a comment. `exit n` is how a script says how it went, and the shell returns
that, so scripts compose with each other exactly as commands do.

### Writing one, on the board

There is no editor worth using here, so `echo` and `>>` are how a file gets
written:

```
rv9> echo # count to five > /r0/loop
rv9> echo set N 1 >> /r0/loop
rv9> echo while compare \$N le 5 >> /r0/loop
rv9> echo echo turn \$N >> /r0/loop
rv9> echo set N = calc \$N + 1 >> /r0/loop
rv9> echo end >> /r0/loop
rv9> echo echo finished after \$N turns >> /r0/loop
```

**Note the `\$`.** Without the backslash the shell you are typing at expands
`$N` before `echo` ever sees it, and what lands in the file is `compare  le
5`. That is not a corner case; it is every line of every loop. `\$` is a
dollar and `\\` is a backslash, and nothing else is escapable.

```
rv9> shell /r0/loop
turn 1
turn 2
turn 3
turn 4
turn 5
finished after 6 turns
```

### The whole of the syntax

| | |
|---|---|
| `set N value` | Remember it. Eight variables, names under 12 characters, values under 32. |
| `set N = cmd args` | Run the command and keep its **first word of output**. This is the one that lets a script act on a *value* rather than only on success. |
| `$N` | Substituted anywhere on a line. An undefined name expands to nothing. |
| `\$`, `\\` | A literal dollar, a literal backslash. |
| `vars` | What is currently remembered. |
| `if cmd` … `else` … `end` | The command's success decides. |
| `while cmd` … `end` | Repeats. **Scripts only** — a terminal cannot be read twice. |
| `a && b`, `a \|\| b` | b only if a succeeded, or only if it did not. |
| `exit n` | Leave, with a status the caller can read. |
| `status` | What the last command returned, or its fault. |

Four levels of `if`/`while` nesting. The condition is **one command** — no
pipes, no `&&` — which is enough because the deciding is done by commands.

### The logic is not in the shell

`compare` and `calc` are modules, like everything else:

```
rv9> compare 9 lt 10          # says nothing: 0 is true
rv9> compare 9 gt 10
compare returned 1
rv9> calc 7 x 6
42
```

That split is deliberate. The shell knows *which lines run*; every piece of
actual logic stays a separately loadable, separately replaceable module. A
shell that grows an operator every time somebody needs to compare two things
becomes a language, and there is already a language for that.

Two consequences of it being modules. `compare` spells its operators
`eq ne lt le gt ge` rather than using `<` and `>`, because those are
redirection — `compare $D < 300` would open a file called 300. And `calc`
uses `x` for multiply, because relying on `*` not being special today is how
a glob added later breaks every script ever written.

`compare` returns **2** for a question that made no sense, which is not the
same as "no" — and it refuses to order two things that are not numbers rather
than inventing an answer:

```
rv9> compare abc lt abd
compare: lt needs two numbers; got 'abc' and 'abd'
```

### What `&&` actually tests

Not "did it return zero". The bit counts a **fault** as failure too, so a
program that was stopped for running off its stack does not count as having
succeeded, whatever number happened to be in its status register. That
distinction needs `wait_why`, which is why this could not have been built
before ABI 14 — see [reference §13](reference.md#13-faults-and-errors-two-number-spaces).

### Nesting, all together

```
set N 1
while compare $N le 6
    set R = calc $N % 2
    if compare $R eq 0
        if compare $N gt 4
            echo $N even and big
        else
            echo $N even
        end
    else
        echo $N odd
    end
    set N = calc $N + 1
end
exit 0
```

Indentation is ignored; it is for you.

### Where this stops

No functions, no arrays, no arithmetic syntax, no quoting beyond `\$`, no
globbing. Those are the features that turn a shell into a language, and the
whole shell costs about 2.4 KB of RAM as it stands. A script that needs more
than this is asking for R9.

## 8. Getting it on the network

> **To write:** `wifi <ssid> <password>`, `passwd`, `authkey`, then
> `ssh demo@<board>`. Note that mbedTLS as shipped has no Ed25519, so use
> ECDSA P-256 or RSA. Mention that an SSH session is a device — `/ssh0` —
> and that this is why the shell needed no changes to work over one.

## 9. Your first module

> **To write:** the smallest module that does something, in full: the
> `.text.entry` attribute, `rv9_module_entry`, what arrives in `env`, and
> `build.conf`. Then build, flash the store, run it by name. Then the rules
> and why each exists: no `.data`/`.bss`, no libc, no 64-bit division, no
> function-scope `static const` arrays, `-mcmodel=medany`. Each of those is
> a bug somebody already had.

## 10. Talking to a device from a module

> **To write:** `open`/`read`/`write`/`close` on `/gpio/N`; `getstat` and
> `setstat` as the way to ask a device something rather than tell it
> something; the same code against `/pwm0` and `/adc0`. Finish with reading
> a sensor over I²C at `/i2c0/0x68`, and the register-on-the-path trick that
> makes a read one transaction.

## 11. Publishing a value

> **To write:** `/pub0/NAME`, one writer, a sequence number, and why the
> three numbers become visible together or not at all. `watch` in another
> shell. The initialisation-then-execution split: open the cell *before*
> declaring a period, because the first write through a path is the
> expensive one.

## 12. A real-time component

> **To write:** the manifest as a contract — `class=realtime`, `period_us`,
> `deadline_us`, `wcet_us`, `heap_max=0`, `mandatory`. `rt control`. Then
> the interesting half: ask for something the machine cannot promise and
> read the refusal, which names which loop and by how much. Then
> `failsafes=`, and killing the process to watch RV-9 park the pin with
> nobody left alive to do it.

## 13. Loading a module without reflashing

> **To write:** `fetch host /path > /r0/thing.mod`, then `load`, then run it
> by name. The point: code is a runtime object, CRC-checked and versioned,
> and this is the workflow that makes a board on a bench bearable.

## 14. Where to look next

> **To write:** reference.md for the contract; design.md for why; roadmap.md
> for what is not done; `docs/target/rv9-profile.json` and the `profile`
> command for the machine-readable version, which is what a compiler reads.

---

## Licence

Apache License 2.0 — see [LICENSE](../LICENSE) and [NOTICE](../NOTICE).
