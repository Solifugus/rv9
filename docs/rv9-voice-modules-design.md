# RV-9 Voice and Tool Modules

**Status:** Proposed design, v0.1  
**Date:** October 25, 2026  
**Initial target:** ESP32-P4 running RV-9  
**Review baseline:** `Solifugus/rv9`, commit `d3a5281e69974f741c54ade05842b057480a650d`

## 1. Purpose

Add optional, local speech recognition, tool interpretation, and speech synthesis to RV-9. Applications should be able to use each independently; Conatus can combine them with its experience corpus, planning, and validation. Model inference must coexist with the display and admitted control loops without exhausting critical memory or monopolizing ordinary execution.

The initial engines are candidates, not permanent API dependencies:

| Capability | Candidate | Contract |
| --- | --- | --- |
| Speech-to-text (STT) | Whistle | Audio utterance → transcript |
| Tool interpretation | Needle | Text and available tool definitions → proposed structured call |
| Text-to-speech (TTS) | Engine selected after licensing and P4 evaluation; sanoTTS is a candidate | Text → PCM audio |

Keep Conatus's reasoning outside these generic modules. A simple appliance can use tool interpretation directly; Conatus can use, replace, or bypass it.

## 2. Findings from the source review

This proposal is based on source inspection, not compilation or hardware measurements. Existing behavior and proposed additions are distinguished below.

| Existing mechanism | Design implication |
| --- | --- |
| Runtime modules, manifests, paths, pipes, and device descriptors | Reuse these for commands, resource declarations, and composition. |
| Module environment has no general allocation API | Supply a budgeted arena rather than importing unrestricted malloc. |
| Module build is freestanding, with no libc linkage | Engine dependencies need explicit adaptation. |
| Linker rejects writable globals; build checks position independence at two addresses | Preserve those guarantees while porting engine state and tables. |
| Loader allocates executable memory for module images | Keep model weights in separate data files. |
| Internal and external memory are distinguished in the KAL | Preserve that distinction for inference. |
| `rv9_alloc_large()` can fall back to internal memory | Add strict external-memory allocation for model workspaces. |
| P4 configuration enables `CONFIG_FREERTOS_UNICORE=y` | Initial implementation must not assume two-core inference. |
| Context switch saves integer callee-saved registers | Audit floating-point and SIMD state before optimized engines yield. |
| P4 notes record PSRAM contention with display scanout | Benchmark with the display active, not just headless. |
| PMP process isolation remains future work | Resource ownership is not yet a hardware security boundary. |

The reviewed P4 configuration describes 32MB PSRAM and 32MB flash. Its partition table assigns 2MiB to the module store and 16MiB to the filesystem volume. Verify actual hardware and available memory at startup rather than assuming the entire capacity is free.

## 3. Proposed component boundaries

All names and paths in this section are proposals.

| Component | Responsibility |
| --- | --- |
| Audio driver and binary stream file manager | I²S/codec setup, PCM capture/playback, bounded buffering, device lifecycle |
| `stt` | Transcribe one bounded utterance or audio file |
| `tools` | Produce a tool proposal using a caller-provided tool catalog |
| `tts` | Synthesize text to an audio stream or file |
| Optional `voice` application | Coordinate listening, interpretation, execution response, and speech |
| Conatus | Context, planning, policy, experience, and outcome validation |

Use `/audio0/in` and `/audio0/out` as candidate device paths. Do not send binary audio through terminal-style character transformations. Configure format and operation using the existing `getstat`/`setstat` conventions.

Start with independent executable modules. Add persistent service processes only where measured model-loading cost justifies keeping an engine resident. Do not introduce a general model broker or plugin framework in the first version.

## 4. Interfaces and request lifecycle

Audio format must explicitly identify sample rate, channel count, sample representation, and byte order. The STT adapter initially accepts the format expected by its engine; conversions belong at a clear boundary. TTS output advertises its actual rate rather than assuming that it matches microphone input.

For an initial file-based CLI, use one request per invocation and EOF as the boundary. If persistent services are added, define a small versioned, length-delimited protocol with request IDs. Never rely on a single read returning a complete record.

Each request must have:

- A maximum input length or audio duration.
- A bounded output size or token limit.
- A cancellation path and explicit terminal status.
- Clear ownership of buffers and model references.
- Errors distinguishing unsupported format/model, insufficient memory, cancellation, and inference failure.

STT returns text and optional language information. Do not invent confidence values if the engine does not supply a calibrated measure. Tool interpretation returns an identifier and typed arguments, or a no-match/clarification result. It must not emit shell commands for automatic execution. TTS emits bounded PCM chunks with backpressure.

The application validates the proposed tool against its supplied catalog, checks argument types and ranges, and applies its own authorization before execution. Conatus can then record the proposal, decision, and observed outcome.

## 5. Model storage and memory

Keep weights separate from executable modules. Use SD-backed files for initial experiments because Whistle's approximately 14.5MB reduced model would nearly fill the current 16MiB volume by itself.

A compact model manifest should record model/engine identifier, format version, byte size, content hash, quantization, and declared workspace requirements. Validate lengths and dimensions before allocation. A checksum detects corruption; it does not establish publisher authenticity.

Allocate one bounded external-memory arena at process admission. Expose its address and size through a versioned extension to the module environment. Charge it to the process and ancestor budgets, and reclaim it after execution has stopped on exit, cancellation, or kill. A model may subdivide its arena without calling the system allocator repeatedly.

Keep separate accounting for internal memory, PSRAM workspace, and persistent storage. Add strict external allocation with no internal fallback. Audio DMA buffers must satisfy the driver's placement, alignment, and cache-coherency requirements; do not assume arbitrary PSRAM buffers are suitable.

Initially load only what fits the measured budget. Sequential listening, interpretation, and speaking allows workspace reuse. Do not promise all three models can remain resident until weights, activations, decoder caches, display buffers, and OS overhead have been measured together.

Avoid repeated small filesystem reads inside matrix operations. Measure model load time and use contiguous reads or a bounded cache. Memory mapping and shared weight caches can follow if justified.

## 6. Scheduling and processor state

Inference runs as ordinary work. Real-time motion loops retain their existing admission and failsafe mechanisms.

Insert yield and cancellation checkpoints inside bounded computational tiles. A checkpoint only between whole layers may be too coarse. Measure maximum uninterrupted execution, cancellation latency, and control-loop response under load. Never perform substantial inference in an interrupt or while holding an I/O or scheduler lock.

Before enabling hardware floating point or P4 SIMD, audit compiler conventions, context switching, interrupt behavior, and extension registers. Test interleaved computations across repeated yields. Use a portable implementation for correctness comparison, not as an assumed performance solution.

Keep the first version single-core. A later dedicated inference worker may justify enabling core 1, but requires explicit synchronization, memory ownership, cache handling, and lifecycle rules. It must not bypass RV-9 budgeting or become an unmanaged thread.

## 7. Engine packaging

Retain the module build's position-independence and reentrancy checks. Move writable globals into instance state, replace absolute pointer tables where needed, and provide only the math and utility routines required by the engine. Add target-specific build selection without changing the portable C3/C5 baseline for unrelated modules.

A firmware-linked engine behind a thin command module is acceptable for the first benchmark. Label it as a prototype arrangement. The intended result is optional engine modules with a small platform adapter for optimized kernels, allocation, time/yield, and I/O.

Do not add an inference-specific syscall for every model operation. Prefer existing paths and process facilities, extending the ABI only for missing generic resources such as external arenas. Update generated target-profile metadata and ABI tests with each extension.

## 8. Implementation sequence

| Phase | Work | Exit criterion |
| --- | --- | --- |
| 0 — Freeze inputs | Pin engine revisions and model hashes; confirm board codec/microphone, storage, licenses, available memory, and build dependencies. | Reproducible host reference and a measured resource budget. |
| 1 — Audio foundation | Implement binary audio paths, format/status controls, bounded capture/playback, and cleanup. | Known PCM plays correctly; captured audio is intelligible; close/kill stops DMA safely. |
| 2 — Execution foundation | Add strict PSRAM arena admission/accounting, cancellation checkpoints, target build support, and register-state tests. | Allocation refusal is clean; exit/kill returns memory; interleaved arithmetic remains correct. |
| 3 — TTS | Port the selected engine; synthesize to a file first, then stream to playback. Include text normalization/phonemization dependencies. | Understandable speech, bounded buffering, measured time to first audio and sustained generation rate. |
| 4 — STT | Port Whistle; transcribe fixed recordings against a host baseline, then add push-to-talk. | Repeatable transcripts and measured latency/accuracy on real microphone recordings. |
| 5 — Tool interpretation | Port Needle with a small fixed catalog and strict output validation. Display proposals before enabling execution. | Correct calls on representative requests; unknown, malformed, and ambiguous requests handled explicitly. |
| 6 — Conatus integration | Add the coordinating application, permitted execution, spoken results, and experience records. | A complete local interaction works repeatedly with bounded resources. |
| 7 — Optimization | Profile P4 kernels, model loading, PSRAM traffic, residency, and possibly core 1. | Improvements preserve correctness and coexistence tests; complexity is supported by measurements. |

Begin with alternating listening and speaking. Wake words, acoustic echo cancellation, speaking interruption, and continuous streaming STT are later features. The first useful demonstration can be a spoken request for a sensor value, a validated read-only tool call, and a spoken answer.

## 9. Acceptance and regression tests

Run targeted tests separately from routine boot tests; large model evaluation should not make every boot lengthy.

- **Correctness:** prerecorded input and host reference outputs; engine/version recorded with results.
- **Memory:** peak internal/PSRAM usage, largest free blocks, repeated request cycles, load failure, cancellation, and kill recovery.
- **Scheduling:** existing deadline tests under inference load; longest non-yielding interval and cancellation latency.
- **Audio:** underrun/overrun reporting, partial reads/writes, EOF handling, and device ownership cleanup.
- **Display coexistence:** inference with active panel scanout and representative storage/network traffic.
- **Tool behavior:** unsupported tools, invalid arguments, no-match cases, ambiguous instructions, and denied operations.
- **Robustness:** missing/truncated models, invalid tensor dimensions, oversized input, exhausted PSRAM, and interrupted requests.
- **Performance:** STT processing time per audio second; TTS first-audio latency and generation rate; tool-call latency; model load time.

Set numerical targets after baseline measurements. Do not infer real-time Whistle performance from CPU frequency alone, or claim security isolation from process accounting alone.

## 10. Licensing and unresolved decisions

RV-9 is Apache-2.0. The reviewed Whistle port describes Apache-2.0 code and model provenance. sanoTTS's repository contains GPLv3 licensing, so it is a candidate pending an explicit packaging/licensing decision; modularity alone should not be assumed to resolve licensing obligations. Check each voice/model artifact and text frontend separately. Alternative licensing or a different TTS engine may be preferable.

Remaining implementation decisions are the exact P4 audio hardware, available storage, arena sizing and ABI representation, execution-state preservation for optimized kernels, TTS engine selection, and whether the first production version needs resident services.

## 11. Source references

RV-9 links below are pinned to the reviewed commit:

- [Module ABI](https://github.com/Solifugus/rv9/blob/d3a5281e69974f741c54ade05842b057480a650d/components/rv9_module/include/rv9/module.h)
- [Module loader](https://github.com/Solifugus/rv9/blob/d3a5281e69974f741c54ade05842b057480a650d/components/rv9_module/src/module.c)
- [Process accounting](https://github.com/Solifugus/rv9/blob/d3a5281e69974f741c54ade05842b057480a650d/components/rv9_proc/src/proc.c)
- [Memory allocation](https://github.com/Solifugus/rv9/blob/d3a5281e69974f741c54ade05842b057480a650d/components/rv9_kal/src/kal_mem.c)
- [Context switching](https://github.com/Solifugus/rv9/blob/d3a5281e69974f741c54ade05842b057480a650d/components/rv9_kernel/src/switch.S)
- [I/O interfaces](https://github.com/Solifugus/rv9/blob/d3a5281e69974f741c54ade05842b057480a650d/components/rv9_io/include/rv9/io.h)
- [Module build](https://github.com/Solifugus/rv9/blob/d3a5281e69974f741c54ade05842b057480a650d/tools/build_modules.sh) and [linker](https://github.com/Solifugus/rv9/blob/d3a5281e69974f741c54ade05842b057480a650d/modules/module.ld)
- [P4 configuration](https://github.com/Solifugus/rv9/blob/d3a5281e69974f741c54ade05842b057480a650d/sdkconfig.defaults.esp32p4) and [partitions](https://github.com/Solifugus/rv9/blob/d3a5281e69974f741c54ade05842b057480a650d/partitions-p4.csv)
- [Whistle/Needle ESP32 implementation](https://github.com/andrisgauracs/whistle-esp32)
- [sanoTTS](https://github.com/Ampixa/sanoTTS) and [license](https://github.com/Ampixa/sanoTTS/blob/master/LICENSE)
- [Babytalk P4 integration reference](https://github.com/tlack/babytalk)
- [Espressif optimized neural-network kernels](https://github.com/espressif/esp-nn)

External project links are evaluation leads from the discussion, not dependencies already incorporated into RV-9. Pin and revalidate them during Phase 0.

## 12. Phase 0 measurements (2026-10-26)

Whistle STT was measured on the 7B as a plain ESP-IDF app, outside RV-9, using
`~/development/esp32/whistle-p4-bench` (its README has the method and how to
re-run it). The clip is 2.6 s of synthetic English speech, at depth 4.

| | one core | two cores |
| --- | --- | --- |
| Portable C | 20.9 s (8.0× real time) | 11.7 s (4.5×) |
| P4 PIE int8 kernel | **6.2 s (2.4×)** | **4.5 s (1.7×)** |

For comparison, the upstream S3 demo takes 6.3 s on two cores for a 2 s clip
(3.2×). The P4 PIE kernel is one function, a 128-element int8 dot product,
and gives an identical transcript.

Display coexistence, one core with the PIE kernel: a still frame being scanned
out costs +3.4%. Full-screen redraws on the other core cost +13% at 9.4 fps
and +129% at 61 fps. The PSRAM is shared, so heavy drawing is the risk, not
the scan-out itself.

What this changes in the plan:

- **Phase 0's go/no-go answer is go.** Push-to-talk STT is practical on one
  core: about 2.4× the length of the utterance, before any further tuning.
- **Optimized kernels are needed from Phase 4 on, not deferred to Phase 7.**
  The portable path is 3.3× slower. Section 7's "small platform adapter for
  optimized kernels" should carry the PIE dot product from the start.
- **PIE state must be considered alongside FP state (§6).** RV-9's context
  switch saves fcsr and fs0-fs11 since `f1732ba`, but not the PIE q
  registers or the XACC accumulator. One dot product runs for well under a
  microsecond and contains no switch point, so a kernel that never yields
  mid-call is safe on RV-9's cooperative threads. That condition should be
  written into the adapter's contract.
- **Weights go in PSRAM or on SD, not mapped flash (§5).** The P4 caches
  flash with 24-bit addresses: past 16 MB it needs an experimental bootloader
  option. The model loads from flash to PSRAM in 2.1 s, leaving 13.9 MB of
  PSRAM free.
- **Core 1 is worth 1.4×**, not 2×. That supports §6's "keep the first
  version single-core".

A real voice, on 2026-10-27: a 3 s laptop-mic recording of "Hello, this is
me" came back exactly, punctuation included, in 5.9 s on one core and 4.3 s
on two. That is one clip, not an accuracy figure.

Not yet measured: accuracy across speakers and noise, TTS, and the tool
interpreter.
