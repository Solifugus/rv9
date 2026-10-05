# RV-9 — A Tutorial

**Getting a modular operating system running on an ESP32-C5, and building
something on it.**

> **Status: all fourteen sections written.** Every transcript below was copied
> from a real session on the board, not written from memory — and three of
> them were *wrong* when written from memory and corrected by running them.
> Where the board says something different from this document, the board is
> right and this document is a bug.

This is the *learning* document, and it is deliberately terse: it shows each
thing once and then points at the [reference](reference.md), which is where
the complete answer lives. For looking a command up rather than learning it,
go to [commands.md](commands.md). When you want to know *why* something is built the
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

Two more worth typing before anything is running, because what they show
*empty* is the point:

```
rv9> owns | first 4
resource                  owner  refs  held as
/pipe/s9                  34     2     shared
/n0/listen/22             sys    1     shared
/ssh0                     33     1     shared
```

A count of open paths would answer *is it busy*. This answers **who has it**,
which is the question worth asking when something physical is happening and
only one program should be causing it. One line per owner, so a device shared
by five processes is five lines.

```
rv9> pubs
name                 means  seq   bytes   cap  age_ms  by   rdrs  torn  note
```

Nothing yet. After `rt control` has run there will be a row with a sequence
number climbing, and it will still be there when the loop has stopped. Note
the `by` column in §11: it is `-` there because `pub` published and exited —
**the cell outlived it**, which is the whole point of a cell.

## 5. Everything is a path

A file is a path. So is a pin, a temperature sensor, a window, a pipe and a
TCP connection. Try them in order and watch the same verbs work on all of
them:

```
rv9> echo hello > /r0/note
rv9> cat /r0/note
hello
rv9> dir /r0
name                     size
note                     6
```

Now something that is not storage at all:

```
rv9> pin 2 1
/gpio/2 := 1, reads 1
rv9> pin 2
/gpio/2 = 1
```

`pin` is a thin conversion between text and the single value the device
carries. A control loop skips the conversion and writes the value itself.

```
rv9> echo hello > /term
```

That puts text on the panel, and `echo` has no idea the panel exists — it
writes to standard output, and the shell pointed standard output somewhere
before forking it.

**And a connection is a path too.** `/n0/host/port` is a TCP session you open,
read and write like a file. `fetch` is built from exactly that and contains no
sockets.

### Where the analogy stops, and why that is the lesson

A sensor is a path, but it is not a *stream*:

```
rv9> cat /tsens
/tsens: cannot open          # a PIO device wants a unit: /tsens/0
rv9> cat /tsens/0            # opens fine -- and never returns
```

That is not a bug. `/tsens` is a **PIO** device: the unit of transfer is one
value, and a temperature has no end. `cat` reads until end of data, and there
is none. Use the tool built for it:

```
rv9> temp
32.20 C
```

So "everything is a path" does not mean everything is a file. The path is the
*address*; what `read` and `write` mean is the **file manager's** business, and
there are seven of them. [reference.md §3](reference.md) names them.

### The point, which is the whole design

A program that reads standard input works on **a file, a pipe, a socket and an
SSH session** without knowing which it has. It never asks. That is why `sshd`
is almost nothing — serving a shell over the network is *open the connection,
point stdin and stdout at it, fork the shell, put them back* — and why the same
`ed` edits in a 236-column SSH window and on the panel at thirty by eight.

Twelve paths per process. Slots 0, 1 and 2 are standard input, output and error
**by convention rather than by rule**, and a child inherits those three and only
those three.

## 6. Small tools that compose

Seven filters, and no more. Each reads standard input, so each composes:

```
rv9> mdir | count lines
114
rv9> procs | match active | field 3
field
match
procs
```

That second line answers "what is running right now" out of three tools that
each do one thing — and it finds itself in the answer, because `field` and
`match` are processes too.

Build one up a stage at a time and watch it narrow:

```
rv9> mdir | sort | last 2
worker       program 1    224   0
wstat        program 1    4788  0
```

### What the pipe actually costs

`|` is not free. A pipe is a file manager — `/pipe/NAME` is a real device — so
each stage costs **two opens and a fork**. On a board with 39 KB for programs
that is a real price, which is why `<` exists:

```
rv9> count lines < /r0/note     one process
1
```

`cat /r0/note | count lines` gives the same answer and spends a second process
for nothing. Use `<` when you have a file, `|` when you have a program.

### They refuse rather than lie

This is the habit worth absorbing. `sort` holds 128 lines, and more than that
is **refused** — it will not return some of your lines in order, and it will
not return all of them shortened. Either would be a wrong answer that looks
like a right one.

The same instinct runs through the machine: a document too big for `/w0` is
dropped and logged rather than half-drawn, and a real-time loop that cannot be
admitted is refused rather than run badly.

`first` stops early on purpose, which closes its input and tells the stage
upstream that nobody is listening:

```
rv9> mdir | first 3
```

> **One trap, and it is real.** A few producers do not yet stop when their
> reader goes away — `log` is one. So `log | sort` on more than 128 lines
> **hangs**: `sort` refuses and exits, and `log` goes on writing into a pipe
> nobody is reading. `log | last 20` is fine, because `last` consumes
> everything. It is in the roadmap as an open item; until it is closed, prefer
> a filter that reads to the end.

[commands.md §4](commands.md) lists all seven.

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

```
rv9> wifi <ssid> <password>
rv9> netstat
state   up
ssid    <ssid>
address 192.168.1.121
```

Credentials are typed by whoever owns the network. They are in no source file,
no descriptor and no flash image put there by anybody else.

`scan` lists what is visible, and is worth having on this board specifically:
the C5 has a 5 GHz radio and most of the family does not, so which band a
network is on is a question you cannot ask elsewhere.

Then a way in:

```
rv9> passwd <password>
rv9> passwd show
```

`passwd` goes to `/sshcfg` rather than `/ssh0`, and that is not arbitrary:
`/ssh0` blocks until somebody logs in, so a password that could only be set
through it would be a password you could never set the first time.

For keys, `authkey` reads one from standard input — a key line runs to a couple
of hundred characters, which is longer than an argument should be. **mbedTLS as
shipped has no Ed25519**, so use ECDSA P-256 or RSA.

```
rv9> authkey list
rv9> authkey clear       password login still works
```

Now from your workstation:

```
$ ssh demo@192.168.1.121
```

**And nothing in the shell changed to make that work.** An SSH session is a
device — `/ssh0` — so `sshd` is *open the connection, point stdin and stdout at
it, fork the shell, put them back*. `rshd` on port 2300 is the same file with
one string different, which is the argument for having put the protocol below
the path.

One thing to know: a fetch is a path too.

```
rv9> fetch example.com /index.html | first 5
```

No sockets, no DNS call, no connect — it builds a path name, opens it, writes a
request and reads the answer. Headers go to stderr and the body to stdout, so
`fetch host /x.mod > /r0/x.mod` puts exactly the bytes of the file on the
volume while the status still reaches you. That is §13.

## 9. Your first module

A command is a module, so writing a command is writing one. The whole of the
smallest useful module:

```c
#include "modlib.h"

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL) return 1;
    m_say(env, RV9_STDOUT, "hello from a module\n");
    return 0;
}
```

`.text.entry` puts it first in the blob, because the loader enters at offset
zero. `env` is **everything a module can do** — thirty-four entries, no libc,
no globals, no kernel calls. `modlib.h` is header-only and each module links
its own copy.

Beside it, `build.conf`:

```
static_size=0
stack_size=2048
revision=1
mtype=program
desc="says hello"
```

Then:

```
rv9> # on the host
$ ./tools/build_modules.sh
$ ./tools/flash_modules.sh /dev/serial/by-id/usb-Espressif_...
rv9> mine
hello from a module
```

### The rules, and why each exists

Each of these is a bug somebody already had:

| rule | why |
|---|---|
| no `.data`, no `.bss` | One copy of the code serves every process. The linker **refuses to link** a module with writable statics, which is the good case. |
| no libc | There is none to link against. No `memcpy`, no `malloc`, no `printf`. |
| no 64-bit division | `uint64_t / n` calls `__udivdi3` in libgcc, which is not there. |
| no `static const` array of **pointers** | The array holds absolute addresses. A `switch` returning string literals compiles to exactly this. Plain `static const` *data* is fine. |
| `-mcmodel=medany` | Every reference becomes PC-relative, so the blob works wherever the loader puts it. |

The last one is not taken on trust: `build_modules.sh` links every module
**twice at different addresses and compares the bytes**. Identical means
position independent; different means it is holding an address, and the build
stops.

## 10. Talking to a device from a module

The same four verbs, from code:

```c
int p = env->open("/gpio/3", RV9_MODE_RW);
if (p < 0) return 2;

uint32_t v = 1;
env->write(p, &v, sizeof(v));
env->read(p, &v, sizeof(v));
env->close(p);
```

**`getstat` and `setstat` are how you ask a device something rather than tell
it something.** Direction, pull, edge, range — none of them are data, so none
of them go through `write`:

```c
uint32_t out = 1;
env->setstat(p, RV9_PIO_SS_DIRECTION, &out);

uint32_t range = 0;
env->getstat(p, RV9_PIO_GS_RANGE, &range);   /* the largest a write may carry */
```

The same code works against `/pwm0/3` and `/adc0/1`, because those are **PIO**
devices too — one value in, one value out. Only the device name changes.

### A sensor on a shared bus

I²C is a different discipline — **IFM**, transactions — because reading six
bytes of acceleration has to be *one* transaction or the three axes come from
three different instants:

```c
int p = env->open("/i2c0/0x68", RV9_MODE_RW);
uint32_t reg = 0x3B;
env->setstat(p, RV9_IFM_SS_REG, &reg);   /* the register a read is preceded by */
env->read(p, buf, 6);                    /* write-then-read, one transaction */
```

Setting the register on the **path** rather than the device is what makes two
programs able to read different registers of the same chip without disturbing
each other.

## 11. Publishing a value

One process computes something; another has to see it. A cell is where it goes.

```
rv9> pub SPEED 1200 40 7
/pub0/SPEED <- 1200 40 7
rv9> pubs | match SPEED
SPEED      -      1     12      64   1196    -    0     0
```

In another shell:

```
rv9> watch SPEED
```

`watch` blocks against the **sequence number**, not an edge, so a publication
landing between two waits is still seen, and several arriving together coalesce
into one wakeup. No polling loop and no sleep guessed at.

Three things about a cell that are worth knowing before you use one:

- **One writer, declared.** A component names the cell in its manifest
  (`publishes="/pub0/SPEED"`), and it is reserved at fork — a second copy is
  refused before it starts.
- **The values arrive together or not at all.** Three numbers published as one
  set are read as one set; a reader never sees a new first number beside an old
  third.
- **The cell outlives the publisher.** A loop that has stopped leaves its last
  value and the moment it was observed, which is exactly what somebody arriving
  after the failure needs.

And one thing about *writing* one from a loop, which costs a day if you miss
it: **open the cell before declaring a period.** The first write through a path
is measurably the most expensive — `control` spent 30 µs of a 50 µs budget
warming up a path on its first activation.

## 12. A real-time component

The manifest is a contract, and this is what one looks like:

```
class=realtime
period_us=1000
deadline_us=1000
wcet_us=50
heap_max=0
mandatory="heap_max"
```

`heap_max=0` says *this program never allocates*, and `mandatory` says *refuse
me rather than run me without that promise being kept*. A control loop that
allocates on its time-critical path is not a control loop.

```
rv9> rt control
rt: started control as pid 58
control: 2000 activations at 1000 us
  worst jitter   64 us
  worst execute  54 us
  overruns       0
```

**`rt` means ask, not run.** RV-9 checks the declaration against itself and
against everything already admitted, and a refusal is specific:

```
rv9> rt heavyloop &
rv9> rt fastloop &
rv9> rt control
E admit 'control': wants 50 permille, 680 already promised, ceiling 700
control: the CPU is already promised
```

That is the utilisation ceiling. The more interesting refusal is
`RV9_PE_UNSCHEDULABLE` — the CPU was sufficient and the *ordering* was not —
and it names which loop would miss and by how much, which is frequently not the
one you were trying to start.

```
rv9> rt
real-time promised  68.0% of 70.0%
slots               2 of 4

slot  period  deadline  runs     bound  worst  misses
0     100000  100000    routine  38000  16395  0
1     5000    3000      urgent   2500   2036   0
```

`routine` and `urgent` were not chosen by anybody — priority is **derived**
from the deadlines of everything admitted.

### Then kill it and watch RV-9 tidy up

```
failsafes="/gpio/2=0"
```

A program that declares that has `/gpio/2` written to 0 when it ends —
**however** it ends, including when the thing that failed is the program
itself. `hold crash` runs off its stack with the pin high, and `pin 2`
afterwards reads 0. Nobody was alive to do it; RV-9 did.

## 13. Loading a module without reflashing

```
rv9> fetch 192.168.1.12:8099 /thing.mod > /r0/thing.mod
rv9> load /r0/thing.mod
rv9> thing
```

That is the whole workflow, and it is why a board on a bench is bearable. The
module store stops being something you reflash and becomes something you add
to.

The module is **CRC-checked and versioned** on the way in: a bad transfer is
refused, and when two modules share a name the higher revision wins — so
upgrading something is loading a newer copy, not deleting the old one first
from a machine that may be using it.

`downloaded` exists to prove exactly this. It is built by the host, kept *out*
of the flash image on purpose (`.nostore`), served over HTTP and fetched. If it
runs, a program reached the machine without anybody reflashing it.

## 14. Where to look next

| | |
|---|---|
| [commands.md](commands.md) | Everything you can type. Checked against the store, so it cannot go stale. |
| [reference.md](reference.md) | The contract: the environment, the manifest, the devices, the faults, the limits. Eighteen sections, and every name the firmware defines is in it or the build fails. |
| [design.md](design.md) | **Why**, and the wrong turns. Much the longest, and the one worth reading in order. |
| [roadmap.md](roadmap.md) | What is not done, and what is open. |
| [memory.md](memory.md) | Where the RAM actually goes. |
| `docs/target/rv9-profile.json` | The machine-readable half, generated from the sources. A compiler targeting RV-9 reads this, not the prose. |
| `profile` | The same question asked of *this board* — its devices, its limits, what each is built from. |

---

## Licence

Apache License 2.0 — see [LICENSE](../LICENSE) and [NOTICE](../NOTICE).
