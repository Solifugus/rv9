# RV-9 / autonomous language compiler — alignment notes

These notes come from the language side. They are kept verbatim below,
followed by RV-9's reading of where it actually stands against each point.

The governing constraint, which RV-9 should hold to: *this does not mean
changing RV-9 around an unfinished language. It means avoiding ABI and
resource-management decisions that unnecessarily prevent the compiler from
communicating information RV-9 can use.*

---

## The notes, as given

RV9 is going to be a target for a language designed for autonomous physical
systems. The compiler and operating system should be deliberately co-designed
rather than treating one another as generic producer and consumer.

The language currently has three conceptual execution levels:

1. **Proaction** — goals, planning, deliberation, optimization; potentially
   complex/dynamic data structures; relatively loose timing and memory
   requirements.
2. **Reaction** — events, watchers, state transitions, constraints;
   deterministic state-to-state behavior; fault detection and recovery;
   preferably bounded where practical, but not necessarily hard real-time.
3. **Real-time** — physical control and sampling; explicit periods/deadlines;
   bounded execution; bounded/static memory where possible; no dependence on
   unpredictable services during operation.

These may eventually be language semantics rather than three independent
languages.

The important design goal for RV9 is:

> A program should be able to describe what machine resources it requires
> before RV9 agrees to run it.

**1. Preserve and extend the module resource contract.** Treat static size,
stack hint, type, entry and ABI version as the beginning of a resource
contract rather than loader metadata. Future metadata may need to express
required stack, per-instance data, maximum heap, whether heap is permitted at
all, execution class, period, deadline, minimum inter-arrival time, required
devices, exclusive versus shared ownership, failsafe behaviour, and
capabilities. Do not commit to a binary encoding for all of these yet; an
extensible mechanism is preferable to repeatedly breaking the fixed header.

**2. Stack size must be a per-program property.** The 8192-byte default is
vastly larger than actual usage. The compiler should eventually calculate or
conservatively estimate a program's requirement; RV-9 remains responsible for
enforcing it. Stack overflow detection is highly desirable, because a small
declared stack is only useful if exceeding it fails safely and
diagnostically.

**3. Real-time processes should support admission control.** Before starting
an RT program RV-9 should be able to ask whether enough guaranteed memory is
available, whether its stack can be met, whether required devices are free,
whether exclusive devices are already owned, whether its timing can coexist
with admitted work, and whether enough system reserve remains. The compiler
describes the requirement; RV-9 decides whether the machine can promise it.

**4. Separate ordinary memory from protected reserves.** Design toward a
system/failsafe reserve, a real-time reserve, ordinary process memory, and
reclaimable memory. These need not be separate allocators immediately; a
reservation floor may be enough. But ordinary programs must eventually be
unable to consume memory required for kernel operation, interrupt handling,
failsafe operation, or already-admitted RT processes.

**5. Heap policy should differ by execution class.** Proaction: dynamic
allocation allowed, failure survivable. Reaction: preferably bounded.
Real-time: ideally no dynamic allocation after initialization — fixed stack,
fixed statics, preallocated buffers, nothing on the time-critical path. A
module should be able to declare `heap_max = 0` and have that mean something.

**6. Hardware resources should be treatable as owned resources.** Shared read
access, exclusive control access, ownership tied to process lifetime, release
on normal exit, supervisor-controlled safe state on abnormal termination. The
syntax is irrelevant to RV-9; what matters is that the kernel and I/O system
carry enough identity and ownership information to enforce such contracts
later.

**7. Real-time hardware access must be bounded.** RV-9 already distinguishes
bounded RT-safe paths from operations whose latency is not bounded. Preserve
this as a first-class property, and expose it machine-readably rather than
only in documentation, so the compiler can reject an unbounded operation
inside a hard RT section.

**8. Initialization and execution are different phases.** Permit RT programs
to do expensive setup — allocate buffers, open devices, configure
peripherals, arm interrupts — before entering their timing contract, and
expose a clean transition into admitted RT execution.

**9. Failsafe behaviour should live below the dying process.** A hard-killed
or wedged control process cannot be trusted to clean up after itself. RV-9 or
a trusted supervisor should be able to prevent the process running again,
place owned hardware into a declared safe state, release resources, report
upward, and optionally allow a recovery process to take over. Do not rely on
destructors or exit handlers for hard failure.

**10. Timing information should be machine-readable.** Release time, actual
start, completion, execution time, worst observed, missed deadlines, missed
releases, jitter, overruns. Timing failure should be observable as state, not
merely logged text.

**11. Avoid POSIX assumptions.** The OS-9-like model is an advantage. Retain
per-instance statics separate from shared code, modules as first-class
runtime objects, the unified path/device model, small process descriptors,
explicitly sized stacks, no mandatory virtual memory, and no assumption that
every program needs stdin/stdout/filesystem/network.

**12. Consider an extensible module manifest.** Rather than enlarging the
fixed header, a compact optional TLV extension area: stack required, static
required, heap max, execution class, RT period, deadline, minimum interval,
device requirement, resource ownership, failsafe state, capability, compiler
ABI, language runtime version. Unknown entries should be safely ignored or
rejected according to whether they are advisory or mandatory.

**13. Compiler-generated resource certificate.** Compilation should
eventually produce a memory/timing/resource/properties record that RV-9
admission-checks. Do not trust compiler claims blindly where they can be
validated cheaply.

**14. Preserve measurement as a design principle.** Intuitive estimates were
dramatically wrong. Keep `free`, `stacks`, `procs`, memory by process,
largest free block, low-water mark, RT reservations, admission state and
resource ownership observable.

**15. Do not prematurely implement the language.** RV-9 provides general
mechanisms — resource contracts, admission, bounded execution, ownership,
capabilities, failsafe, timing telemetry, memory accounting. The compiler
gives them meaning. Neither side should duplicate the other's job.

> **Central principle.** The compiler describes what a program promises and
> what it requires. RV-9 determines whether the physical machine can honour
> that contract, and then enforces it while the program runs.

---

## Where RV-9 stands today

Measured or checked against the running system, 2026-09-12.

| | status |
|---|---|
| 1. resource contract in the header | partial — four fields exist, no extension mechanism |
| 2. per-program stack | **done**, `stack_size` in `build.conf`; overflow **detected** (guard word, thread killed), not prevented — PMP is phase 7 |
| 3. RT admission | not started — RT work is launched, not admitted |
| 4. protected reserves | **partial** — a 12 KB floor RV-9 will not allocate into, so exhaustion is a refusal it reports rather than an abort inside ESP-IDF; no per-process limit, no RT-specific reserve |
| 5. heap policy by class | see below — the situation is the reverse of what is assumed |
| 6. device ownership | reference counts only; no owner identity, no exclusivity |
| 7. RT-safe marked machine-readably | documented in prose, `RV9_RT_CODE` in source; not readable |
| 8. init separate from execution | **already exactly this** |
| 9. failsafe below the process | release-on-exit works; declared safe states do not exist |
| 10. timing as state | **done**, `RV9_SYS_RT` |
| 11. no POSIX assumptions | **already true** and worth defending |
| 12. extensible manifest | not started — the highest-value item |
| 13. resource certificate | not started; validation loop partly exists |
| 14. measurement | **established practice**: `free`, `stacks`, `procs`, `docs/memory.md` |
| 15. no language semantics in the OS | held so far |

### Three corrections

**On §5, heap: the situation is the opposite of the assumption.** RV-9
modules have *no dynamic allocation at all*. There is no `alloc` in
`rv9_mod_env_t`; a module gets a stack, a zeroed statics area sized by its
header, and nothing else. So `heap_max = 0` is not a restriction to be
added — it is already true of every program on the machine, and enforced by
there being no mechanism to violate.

The work therefore runs the other way: Proaction needs a heap *introduced*,
and it should arrive already bounded and already per-class, rather than as a
general allocator later fenced off. That is a much better position to design
from than retrofitting limits onto an existing malloc.

**On §8, initialization: this is already the shape.** An RT program opens its
devices, allocates and configures, and only then calls `rt_declare(period)`
to enter its timing contract; `rt_wait()` is the periodic boundary
thereafter. `control` does exactly this. What is missing is not the phase
split but the *admission* at the boundary — `rt_declare` currently accepts,
it does not decide.

**On §3, admission: the honest limit is WCET.** RV-9 can check memory, stack,
device availability and reserve cheaply and refuse on any of them. It cannot
verify a worst-case execution time before running the code; nobody can, from
the outside. What it can do — and already does — is *observe* execution time,
lateness and overruns, and treat a declared deadline as a claim to be
policed. So admission should be: refuse on what is checkable, admit on the
timing claim, and report the moment reality disagrees. That division is worth
fixing early because it decides what the compiler must prove versus what it
may merely assert.

### The validation loop that already exists

§13 asks that compiler claims be validated where cheap. One such loop is
already built and worth naming, because it generalises:

Stacks are painted at creation and scanned on demand, so a declared
`stack_size` can be compared against the high-water mark actually reached.
A program claiming 736 bytes and touching 900 is caught by measurement, not
by trust. The same shape — *declare, measure, compare, report* — applies to
execution time (`RV9_SYS_RT` already does it), to heap once there is one, and
to device use.

### What would foreclose options if left alone

In the order that matters:

1. **The fixed module header.** Every new field breaks the ABI. A TLV
   extension area is the single change that stops this recurring, and it is
   cheap now and expensive after several more fields.
2. **Stack overflow detection.** Per-program stacks are already possible and
   already unsafe: nothing detects an overrun, so a too-small declaration
   corrupts the heap silently. Small stacks are only useful if exceeding one
   fails loudly.
3. **A memory floor.** Until ordinary allocation can be refused, admission
   control has nothing to protect and "guaranteed memory" cannot mean
   anything.

Everything else in these notes can wait for the language without cost.
