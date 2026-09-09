#!/usr/bin/env python3
"""
Wrap a raw binary blob in an RV-9 module header.

The CRC must match components/rv9_module/src/crc32.c exactly -- both are
standard CRC-32 (zlib polynomial), computed over the whole image with the
crc32 field taken as zero.
"""
import argparse
import struct
import sys
import zlib

MAGIC = 0x4D395652          # "RV9M"
ABI = 1
HDR_LEN = 40
HDR_FMT = "<IHHIIIIIBBBBII"

TYPES = {
    "program": 1, "library": 2, "filemgr": 3, "driver": 4,
    "descriptor": 5, "data": 6, "system": 7,
}


def build(blob, name, mtype, entry_off, static_size, stack_size, revision):
    name_bytes = name.encode("ascii") + b"\0"
    name_bytes += b"\0" * (-len(name_bytes) % 4)      # keep the blob aligned

    name_offset = HDR_LEN
    blob_offset = HDR_LEN + len(name_bytes)
    module_len = blob_offset + len(blob)

    header = struct.pack(
        HDR_FMT,
        MAGIC, HDR_LEN, ABI, module_len,
        name_offset, blob_offset + entry_off,
        static_size, stack_size,
        TYPES[mtype], 0, revision, 0,
        0,          # crc32, filled in below
        0,          # reserved1
    )
    assert len(header) == HDR_LEN, len(header)

    image = bytearray(header + name_bytes + blob)
    crc = zlib.crc32(bytes(image)) & 0xFFFFFFFF
    struct.pack_into("<I", image, 32, crc)            # crc32 field at 0x20
    return bytes(image), crc


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", help="raw binary (objcopy -O binary output)")
    ap.add_argument("output", help="module file to write")
    ap.add_argument("--name", required=True, help="module name, max 31 chars")
    ap.add_argument("--type", default="program", choices=sorted(TYPES))
    ap.add_argument("--entry-offset", type=lambda v: int(v, 0), default=0,
                    help="entry point within the blob (default 0)")
    ap.add_argument("--static-size", type=lambda v: int(v, 0), default=0,
                    help="per-instance static storage the loader must provide")
    ap.add_argument("--stack-size", type=lambda v: int(v, 0), default=0,
                    help="stack hint; 0 lets the loader decide")
    ap.add_argument("--revision", type=int, default=0)
    args = ap.parse_args()

    if len(args.name) > 31:
        sys.exit(f"module name too long: {args.name!r} (max 31 chars)")

    with open(args.input, "rb") as f:
        blob = f.read()
    if not blob:
        sys.exit(f"{args.input} is empty")

    image, crc = build(blob, args.name, args.type, args.entry_offset,
                       args.static_size, args.stack_size, args.revision)

    with open(args.output, "wb") as f:
        f.write(image)

    print(f"{args.name}: {len(image)} bytes "
          f"({len(blob)} code+rodata), crc32 {crc:#010x}")


if __name__ == "__main__":
    main()
