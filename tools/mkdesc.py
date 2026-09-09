#!/usr/bin/env python3
"""
Pack an RV-9 device descriptor into the 64-byte on-media form.

Must match rv9_devdesc_t in components/rv9_io/include/rv9/io.h.
"""
import argparse
import struct
import sys

FMT = "<16s16s16sIIII"   # name, filemgr, driver, opt[4]


def field(value, name):
    b = value.encode("ascii")
    if len(b) > 15:
        sys.exit(f"{name} too long: {value!r} (max 15 chars + NUL)")
    return b


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("output")
    ap.add_argument("--dev-name", required=True, help='e.g. "/term"')
    ap.add_argument("--filemgr", required=True, help='e.g. "scf"')
    ap.add_argument("--driver", required=True, help='e.g. "lcdcon"')
    ap.add_argument("--opt", type=lambda v: int(v, 0), nargs=4,
                    default=[0, 0, 0, 0],
                    help="four 32-bit options (SCF: echo autolf - -)")
    args = ap.parse_args()

    blob = struct.pack(FMT,
                       field(args.dev_name, "dev-name"),
                       field(args.filemgr, "filemgr"),
                       field(args.driver, "driver"),
                       *args.opt)
    assert len(blob) == 64, len(blob)

    with open(args.output, "wb") as f:
        f.write(blob)

    print(f"descriptor {args.dev_name}: {args.filemgr} over {args.driver}")


if __name__ == "__main__":
    main()
