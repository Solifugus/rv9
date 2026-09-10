# RV-9

A small modular operating system for RISC-V, in the spirit of Microware OS-9.

Target: ESP32-C5 (single RISC-V core @ 240MHz, WiFi 6, 4MB flash, ~246KB RAM)
on the Waveshare ESP32-C5-LCD-1.47 board.

- [Design](docs/design.md) — architecture, module format, I/O model, the
  FreeRTOS-to-native strategy
- [Development plan](docs/roadmap.md) — phased, each phase useful on its own

Status: **phases 0-4 complete.** KAL over FreeRTOS (27/27 conformance tests
passing on hardware); modules built, verified, loaded and run from flash;
processes with OS-9-style priority aging, measured starvation-free; unified
I/O with SCF, a UART driver, and a text console on the ST7789 panel; and a
shell where every command is a loadable module; and RBF storage with
segment-list files on a RAM disk; the network as a path; and RV-9's own
context switch, scheduler and timer tick running its threads, passing the
same KAL conformance suite as FreeRTOS (phase 7, steps 1-2 of 5).

```
[freertos]   23 passed, 0 failed
[rv9-kernel] 23 passed, 0 failed
```

```
rv9> mdir
name        type    rev  size link
desc_term   descrip 5    132   0
echo        program 1    136   0
mdir        program 1    784   1
shell       program 2    1248  1
...
rv9> echo > /term        # output goes to the panel instead
rv9> filetest
filetest: wrote and verified 600 bytes to /r0/notes.txt
rv9> dir
name                     size
notes.txt                600
rv9> nettest
netecho: listening on /n0/listen/8042
nettest: echoed 'the network is a path' over loopback
rv9> wifi <ssid> <password>
rv9> fetch example.com
opening /n0/example.com/80
HTTP/1.1 200 OK
...
--- 828 bytes
```

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
