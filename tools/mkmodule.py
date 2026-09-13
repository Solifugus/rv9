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

# Manifest tags. These numbers are the agreement with
# components/rv9_module/include/rv9/module.h and are fixed once published.
MANDATORY = 0x8000
TAGS = {
    "end": 0x0000, "desc": 0x0001, "stack": 0x0002, "static": 0x0003,
    "heap_max": 0x0004, "class": 0x0005, "period_us": 0x0006,
    "deadline_us": 0x0007, "min_inter_us": 0x0008, "wcet_us": 0x0009,
    "device": 0x000A, "exclusive": 0x000B, "failsafe": 0x000C,
    "capability": 0x000D, "compiler": 0x000E, "runtime": 0x000F,
}

CLASSES = {"unspecified": 0, "proaction": 1, "reaction": 2, "realtime": 3}

# How each tag's value is encoded, so a build.conf says `heap_max=0` and
# gets four bytes rather than the string "0".
U32_TAGS = {"stack", "static", "heap_max", "period_us", "deadline_us",
            "min_inter_us", "wcet_us"}
U8_TAGS = {"class"}
STR_TAGS = {"desc", "device", "exclusive", "failsafe", "capability",
            "compiler", "runtime"}
REPEATABLE = {"device", "exclusive", "capability"}


def encode_manifest(entries):
    """entries: list of (name, value, mandatory). Returns padded bytes."""
    out = bytearray()

    for name, value, mandatory in entries:
        # A number is a tag this tool has not been taught yet, which is the
        # normal case for a producer newer than its tools. Its value goes
        # in as bytes; only the registry above knows how to encode types.
        if name not in TAGS:
            try:
                tag = int(name, 0)
            except ValueError:
                raise ValueError(f"unknown manifest tag {name!r}") from None
            payload = str(value).encode("utf-8")
            out += struct.pack("<HH", tag | (MANDATORY if mandatory else 0),
                               len(payload)) + payload
            out += b"\0" * (-len(out) % 4)
            continue

        if name in U32_TAGS:
            payload = struct.pack("<I", int(str(value), 0))
        elif name in U8_TAGS:
            n = CLASSES.get(str(value).lower())
            if n is None:
                n = int(str(value), 0)
            payload = struct.pack("<B", n)
        elif name in STR_TAGS:
            payload = str(value).encode("utf-8")
            if len(payload) > 255:
                raise ValueError(f"{name} is too long ({len(payload)} bytes)")
        else:
            raise ValueError(f"no encoding for tag {name!r}")

        tag = TAGS[name] | (MANDATORY if mandatory else 0)
        out += struct.pack("<HH", tag, len(payload)) + payload
        out += b"\0" * (-len(out) % 4)      # entries are 4-byte aligned

    if out:
        out += struct.pack("<HH", TAGS["end"], 0)
        out += b"\0" * (-len(out) % 4)

    return bytes(out)


def build(blob, name, mtype, entry_off, static_size, stack_size, revision,
          manifest=b""):
    name_bytes = name.encode("ascii") + b"\0"
    name_bytes += b"\0" * (-len(name_bytes) % 4)      # keep the blob aligned

    name_offset = HDR_LEN
    manifest_offset = HDR_LEN + len(name_bytes) if manifest else 0
    blob_offset = HDR_LEN + len(name_bytes) + len(manifest)
    module_len = blob_offset + len(blob)

    header = struct.pack(
        HDR_FMT,
        MAGIC, HDR_LEN, ABI, module_len,
        name_offset, blob_offset + entry_off,
        static_size, stack_size,
        TYPES[mtype], 0, revision, 0,
        0,          # crc32, filled in below
        manifest_offset,
    )
    assert len(header) == HDR_LEN, len(header)

    image = bytearray(header + name_bytes + manifest + blob)
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
    ap.add_argument("--tag", action="append", default=[], metavar="NAME=VALUE",
                    help="manifest entry; NAME! marks it mandatory, meaning a "
                         "loader that does not understand it must refuse the "
                         "module. Repeatable.")
    args = ap.parse_args()

    entries = []
    for spec in args.tag:
        if "=" not in spec:
            sys.exit(f"--tag wants NAME=VALUE, got {spec!r}")
        name, value = spec.split("=", 1)
        mandatory = name.endswith("!")
        entries.append((name.rstrip("!").strip(), value, mandatory))

    try:
        manifest = encode_manifest(entries)
    except ValueError as e:
        sys.exit(str(e))

    if len(args.name) > 31:
        sys.exit(f"module name too long: {args.name!r} (max 31 chars)")

    with open(args.input, "rb") as f:
        blob = f.read()
    if not blob:
        sys.exit(f"{args.input} is empty")

    image, crc = build(blob, args.name, args.type, args.entry_offset,
                       args.static_size, args.stack_size, args.revision,
                       manifest)

    with open(args.output, "wb") as f:
        f.write(image)

    extra = f", manifest {len(manifest)}" if manifest else ""
    print(f"{args.name}: {len(image)} bytes "
          f"({len(blob)} code+rodata{extra}), crc32 {crc:#010x}")


if __name__ == "__main__":
    main()
