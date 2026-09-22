# RV-9 — Reference

**The contract: what RV-9 promises a program, what it requires of one, and
what it enforces.**

> **Status: being written.** Sections marked *To write* are outlines.
>
> Where this document lists something the firmware defines — a system call, a
> manifest tag, a fault code — the list is meant to be **complete**, and
> `tools/hosttest/run.sh` is meant to fail if it stops being. See §16.

Three documents, three jobs. This one is for looking things up. The
[tutorial](tutorial.md) is for learning by doing. The
[design log](design.md) is for why, and records the wrong turns.

The machine-readable version of most of what follows is
[`docs/target/rv9-profile.json`](target/rv9-profile.json), generated from the
sources. A compiler targeting RV-9 should read that rather than this; this
document exists to explain what the fields mean.

**Module ABI 14.** Anything below marked *since ABI n* is absent from an
older firmware.

---

## 1. The seven nouns

Everything in RV-9 is one of these, and the separations between them are the
architecture rather than an implementation detail.

| | what it is |
|---|---|
| **module** | A position-independent lump of code with a header, a CRC and a manifest, found by name in a store. Programs, drivers, file managers and device descriptors are all modules. |
| **process** | A running module: a stack, its own statics, a path table, a memory budget, a priority, and a parent. |
| **path** | An open thing, named as a string. `open("/gpio/2")`. The number you get back is an index into your own path table, and children inherit it. |
| **file manager** | The *discipline* a path obeys — what read and write mean. SCF makes characters, RBF makes blocks, PIO makes single values, IFM makes transactions. |
| **driver** | The hardware underneath, with no opinion about discipline. |
| **device descriptor** | A module that says *this file manager, that driver, these options, called this name* — which is how `/i2c0` exists without anybody compiling it in. |
| **class** | What kind of citizen a process is: `REALTIME` is admitted or refused, and gets reserves nothing else may touch. |

The consequence worth stating once: because a file manager, a driver and a
descriptor are three separate loadable modules, adding a device is adding a
descriptor, and adding a *kind* of device is adding a file manager. Neither
needs the firmware rebuilt.

## 2. Paths

> **To write:** the grammar — `/dev` and `/dev/unit` and `/dev/a/b/c`; which
> managers take what; the path table's size (`max_paths`, 12) and that slots
> 0, 1 and 2 are stdin, stdout and stderr by convention rather than by rule;
> `dup2` and inheritance across `fork`; that `open` returns the lowest free
> slot, which is a fact you can get wrong (see design §47's neighbours).
> Table of every path as shipped.

## 3. The disciplines

> **To write:** one subsection each for SCF, RBF, PIO, IFM, NFM, PFM and
> PIPE. For each: what read and write mean, whether seek exists, what
> getstat/setstat codes apply, whether read and write are real-time safe
> (the profile's `read_rt`/`write_rt`), and what happens at end of data.

## 4. Devices as shipped

> **To write:** all fifteen drivers and their descriptors, with the
> descriptor options each takes (`opt[0..]`) and its default — for example
> `/i2c0` takes SDA, SCL and kHz, defaulting to 8, 9 and 100. Note which
> pins are reserved by the board and refused (`/gpio` refuses the USB
> console, display and card pins).

## 5. The module format

> **To write:** the 40-byte header, the magic, the ABI check (the loader
> refuses a module declaring a *higher* ABI than the firmware, never a
> lower), the CRC, the type field, and `.text.entry`. Then the rules a
> module must obey and the reason for each: no `.data`/`.bss`, no libc, no
> 64-bit division, no function-scope `static const` arrays, `-mcmodel=medany`
> — each of which was discovered by breaking it.

## 6. The manifest

A module's TLV manifest is what it says it needs. RV-9 reads it **before the
module runs**, which is what makes refusal possible: a program that asks for
more than the machine can promise never starts, rather than failing halfway.

Tags come from `build.conf`. **Enforced** means RV-9 acts on it; advisory
means it is recorded and passed on but nothing checks it.

| tag | enforced | what it says |
|---|---|---|
| `desc` | advisory | One line, for `mdir` and for humans. |
| `stack` | **yes** | Stack bytes. Allocated at fork; a fork that cannot have it is refused. |
| `static` | advisory | Bytes of private storage, allocated and zeroed at fork. Reaching `env->statics`. |
| `heap_max` | **yes** | Ceiling on what the process may allocate. Zero means *none*, which is the declaration a control loop should make. |
| `class` | advisory | `UNSPECIFIED`, `PROACTION`, `REACTION` or `REALTIME`. |
| `period_us` | **yes** | How often a real-time component is released. |
| `deadline_us` | **yes** | How long after release it must have answered. |
| `min_inter_us` | **yes** | For an event-released component: the shortest gap between events it will tolerate. |
| `wcet_us` | **yes** | Declared worst-case execution time. This is the number admission does arithmetic with. |
| `device` | **yes** | A device the module needs, shared. |
| `exclusive` | **yes** | A device the module needs *alone*. A second owner is refused before it runs. |
| `failsafe` | **yes** | A device and a constant. RV-9 writes it when the process ends, however it ends — including when the process is the thing that failed. |
| `capability` | advisory | Something the module claims to offer. |
| `compiler` | advisory | What produced it. |
| `runtime` | advisory | What it expects to run under. |
| `on_deadline` | **yes** | `REPORT` or `FAULT`. Whether a missed deadline stops the component. |
| `publishes` | **yes** | A cell this module owns. Reserved at fork, so a second copy is refused and nothing else can write into it. |
| `watches` | **yes** | A cell this module reads. Admitted because the declaration of its writer exists on the machine, whether or not the writer is running. |
| `mem_max` | **yes** | The process's whole memory budget, itself and everything it starts. |
| `placement` | **yes** | `DERIVED`, `URGENT` or `ROUTINE`. A pin constrains the analysis rather than overriding it: if the pin would make any loop late, the program is refused. |
| `mandatory` | — | Not a tag but a bitmask over them. See below. |

### Mandatory tags, and unknown ones

A tag RV-9 does not recognise is **ignored** if advisory and **refuses the
module** if marked mandatory. That is the whole forward-compatibility story,
and it runs in the useful direction: a module built for a newer RV-9 that
needs something this one has never heard of does not start, instead of
starting and quietly not getting it.

So `mandatory="heap_max"` in a `build.conf` means *refuse me rather than run
me without this*, and a control loop should say it.

> **To write:** the TLV layout itself, the mandatory bit's position, and
> `highest_known_tag`.

## 7. The environment

Thirty-four calls, arriving as a struct of function pointers. No libc, no
globals, no kernel calls: everything a module can do is here.

> **To write:** all thirty-four, grouped — lifecycle, paths, processes,
> information, real time — each with its signature, what it returns, its
> error codes, its real-time safety, and the ABI it arrived in. The grouping
> to use:
>
> - **itself**: `abi_version`, `statics`, `statics_size`, `pid`, `arg`
> - **time and yielding**: `time_ms`, `time_us`, `sleep_ms`, `yield`
> - **paths**: `open`, `close`, `read`, `write`, `seek`, `dup2`, `getstat`,
>   `setstat`, `remove`
> - **processes**: `fork`, `fork_arg`, `fork_rt`, `wait`, `wait_why`,
>   `chain`, `signal`, `kill`, `signals_take`
> - **modules**: `load`
> - **information**: `sysinfo`, `print`
> - **real time**: `rt_declare`, `rt_declare_event`, `rt_wait`, `rt_stats`
>
> Two things to be explicit about. `wait_why` (ABI 14) is the one that
> separates what a program returned from what RV-9 decided about it, and
> without it the two share a number space — see §13. And `rt_wait` returns
> the number of releases *coalesced*, which is how a component learns it was
> too slow without being told by a fault.

## 8. sysinfo

> **To write:** thirteen codes, each with its struct, and the convention that
> a caller passes the size it knows about so a record may grow without
> breaking an older module. `MEM`, `MODULES`, `PROCS`, `RT`, `STACK`,
> `ADMIT`, `CLAIM`, `DEVICES`, `LIMITS`, `BUDGETS`, `STACK_PEAKS`, `LOG`,
> `CLOCK`.

## 9. Device settings: getstat and setstat

> **To write:** the codes, by discipline. Generic PIO
> (`SS_DIRECTION`, `SS_PULL`, `SS_FREQUENCY`, `GS_RANGE`, `SS_EDGE`,
> `GS_EVENT`, `GS_PULSE_US`, `GS_PULSES`, `GS_PERIOD_US`), IFM
> (`SS_REG`, `GS_PRESENT`), console (`GS_SIZE`, `SS_CURSOR`, `SS_COLOUR`,
> `SS_ATTR`, `SS_CLEAR`, `SS_CURSOR_ON`, `GS_ONSCREEN`), publication and
> RBF.
>
> **Known gap:** these are not in `rv9-profile.json`, which means a compiler
> reading the profile cannot see them. They should be. Raised here rather
> than quietly omitted.

## 10. Publication

> **To write:** one writer, declared; `rv9_pub_t`'s 16-byte head; the
> sequence number and what `torn` means; `stamp_us` being when the reading
> was *taken*; the blocking `getstat` wait so a watcher re-evaluates on
> change rather than polling; that closing a cell does not erase it, because
> the last thing a stopped loop measured is what an investigation wants.

## 11. The real-time class

> **To write:** admission by response-time analysis rather than utilisation;
> priority derived from deadlines; the utilisation ceiling; the 2 ms runaway
> watchdog and what it does; `on_deadline`; the slot count; what a refusal
> message contains and how to read it.

## 12. Memory

> **To write:** the classes and their floors — ordinary work refused 8 KB
> before a real-time loop would be, the failsafe path lower still; per-process
> budgets and how a charge is attributed to ancestors; what `free` reports
> and what each line means. Cross-reference [memory.md](memory.md), which is
> the measured account.

## 13. Faults and errors: two number spaces

This is the one place where reading the reference carelessly will produce a
bug, so it is stated at length.

A module returns an `int`. RV-9's own verdicts are also `int`s, and they
overlap. `RV9_PE_FAULT` is 6; a tool returning `-6` to mean "that address did
not answer" was for some time announced by the shell as having been *stopped
by the scheduler*. It had run perfectly.

**A status and a fault are two different questions.** Ask both:

```c
int status = 0, fault = RV9_FAULT_NONE;
env->wait_why(pid, &status, &fault, RV9_WAIT_FOREVER);

if (fault != RV9_FAULT_NONE) {
    /* RV-9 stopped it. `status` is not the program's answer. */
} else {
    /* It returned under its own power. `status` means whatever it says. */
}
```

`wait()` still exists and still hands back only a status. It is not wrong,
but anything deciding *what happened* should use `wait_why`.

> **To write:** the five fault codes, the fourteen process errors and the
> twelve I/O errors, each with what actually causes it. And the advice that a
> module should return **positive** codes for its own conditions, which keeps
> it out of the way of everything negative.

## 14. Limits

> **To write:** a table straight from the profile — path slots per process,
> pid range, process history depth, real-time slots, publication name
> length, the watchdog and runaway intervals, the utilisation ceiling.
> Generated rather than typed, so it cannot drift.

## 15. The boot suites

> **To write:** what each of the ten suites proves, and why a system ships
> with its tests running at every boot. Reading a suite's output is the
> fastest way to find out whether a board is healthy, and `sched`'s control
> run is the one to read first.

## 16. Keeping this document honest

> **To write, and to build:** `tools/checkdocs.py`, wired into
> `tools/hosttest/run.sh` beside `mkprofile.py --check`. It should fail when
> the firmware defines a call, a manifest tag, a fault code or an error code
> that this document does not mention.
>
> The argument is the same one the generated profile already makes: the
> *presence* of a thing can be checked mechanically even though the prose
> about it cannot. Documentation that the build refuses to let you forget is
> a different kind of object from documentation you mean to get back to.

---

## Licence

Apache License 2.0 — see [LICENSE](../LICENSE) and [NOTICE](../NOTICE).
