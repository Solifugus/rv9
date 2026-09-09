# RV-9

A small modular operating system for RISC-V, in the spirit of Microware OS-9.

Target: ESP32-C5 (single RISC-V core @ 240MHz, WiFi 6, 4MB flash, ~246KB RAM)
on the Waveshare ESP32-C5-LCD-1.47 board.

- [Design](docs/design.md) — architecture, module format, I/O model, the
  FreeRTOS-to-native strategy
- [Development plan](docs/roadmap.md) — phased, each phase useful on its own

Status: **phases 0-2 complete.** KAL over FreeRTOS (27/27 conformance tests
passing on hardware); modules built, verified, loaded and run from flash;
processes with OS-9-style priority aging, measured starvation-free.

```
. ~/esp/esp-idf/export.sh
idf.py build                          # app
./tools/build_modules.sh              # modules -> build/modules.bin
idf.py -p /dev/ttyACM0 flash
./tools/flash_modules.sh /dev/ttyACM0 # module store
idf.py -p /dev/ttyACM0 monitor
```

## The idea in one paragraph

OS-9 got several things right that small systems today mostly get wrong:
position-independent memory modules that the system can find and share at
runtime, and an I/O model where file managers, drivers and device descriptors
are separate loadable pieces. RV-9 rebuilds those ideas on RISC-V, first as a
personality layer over FreeRTOS, then — through a deliberately narrow kernel
abstraction layer — on its own kernel with PMP-based memory protection.

Not a clone. No binary compatibility. The ideas are worth stealing; the 1980s
encodings are not.
