#!/usr/bin/env python3
"""
Read an RV-9 module's header and manifest back out.

The manifest is what a program says it needs before RV-9 agrees to run it,
and a format nobody can read is a format nobody will trust. This is the
other half of tools/mkmodule.py: point it at a .mod file, or at the whole
store image, and it prints what the loader will see.

    tools/modinfo.py build/modules/control.mod
    tools/modinfo.py build/modules.bin        # every module in the store
"""
import os
import struct
import sys
import zlib

MAGIC = 0x4D395652
HDR_LEN = 40
HDR_FMT = "<IHHIIIIIBBBBII"

TYPES = {1: "program", 2: "library", 3: "filemgr", 4: "driver",
         5: "descriptor", 6: "data", 7: "system"}

MANDATORY = 0x8000
TAGS = {0x0000: "end", 0x0001: "desc", 0x0002: "stack", 0x0003: "static",
        0x0004: "heap_max", 0x0005: "class", 0x0006: "period_us",
        0x0007: "deadline_us", 0x0008: "min_inter_us", 0x0009: "wcet_us",
        0x000A: "device", 0x000B: "exclusive", 0x000C: "failsafe",
        0x000D: "capability", 0x000E: "compiler", 0x000F: "runtime",
        0x0010: "on_deadline", 0x0011: "publishes", 0x0012: "watches",
        0x0013: "mem_max", 0x0014: "placement", 0x0015: "heap_in",
        0x0016: "ext_max", 0x0017: "uses", 0x0018: "isa", 0x0019: "module",
        0x001A: "file"}

CLASSES = {0: "unspecified", 1: "proaction", 2: "reaction", 3: "realtime"}
ON_DEADLINE = {0: "report", 1: "fault"}
PLACEMENT = {0: "derived", 1: "urgent", 2: "routine"}
HEAP_IN = {0: "internal", 1: "external"}
U8_NAMES = {"class": CLASSES, "on_deadline": ON_DEADLINE,
            "placement": PLACEMENT, "heap_in": HEAP_IN}

U32 = {"stack", "static", "heap_max", "period_us", "deadline_us",
       "min_inter_us", "wcet_us", "mem_max", "ext_max"}
U8 = {"class", "on_deadline", "placement", "heap_in"}
FS = {"failsafe"}
ISA_BITS = {1 << 0: "pie", 1 << 1: "fpu"}


def show_value(name, raw):
    if name in FS and len(raw) >= 5:
        # u32 value then the device path, unterminated.
        value = struct.unpack("<I", raw[:4])[0]
        path = raw[4:].decode("utf-8", "replace")
        return f"{path} = {value}"
    if name == "isa" and len(raw) == 4:
        n = struct.unpack("<I", raw)[0]
        return " ".join(v for b, v in sorted(ISA_BITS.items()) if n & b) or "0"
    if name in U32 and len(raw) == 4:
        n = struct.unpack("<I", raw)[0]
        if name == "heap_max" and n == 0:
            return "0 (no heap at all)"
        return str(n)
    if name in U8 and len(raw) == 1:
        return U8_NAMES[name].get(raw[0], str(raw[0]))
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.hex()


def show_manifest(image, off, module_len):
    """The same walk the loader does, with the same bounds checks."""
    if off == 0:
        print("  manifest       none")
        return

    if off < HDR_LEN or off >= module_len:
        print(f"  manifest       BAD OFFSET {off}")
        return

    print("  manifest")
    while off + 4 <= module_len:
        tag, length = struct.unpack_from("<HH", image, off)
        number = tag & 0x7FFF
        if number == 0:
            return

        value_off = off + 4
        if value_off + length > module_len:
            print("    (runs off the end of the module)")
            return

        raw = image[value_off:value_off + length]
        name = TAGS.get(number, f"tag{number:#06x}")
        mark = "!" if tag & MANDATORY else " "
        print(f"    {mark}{name:<14} {show_value(name, raw)}")

        off = value_off + length
        off += -off % 4


TRUST = [os.path.expanduser("~/.config/rv9/trust")]
ATTR_SIGNED = 0x01


def trusted_keys():
    """name -> public point, from a trust file like the board's /f0/trust,
    and the key tools/mkkey.py made, when there is one."""
    keys = {}
    for path in TRUST + [os.environ.get("RV9_TRUST", "")]:
        if path and os.path.exists(path):
            for line in open(path):
                f = line.split()
                if len(f) == 3 and len(f[1]) == 16:
                    keys[f[1]] = (f[0], bytes.fromhex(f[2]))
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import mkkey
        from cryptography.hazmat.primitives import serialization
        if os.path.exists(mkkey.DEFAULT):
            k = serialization.load_pem_private_key(open(mkkey.DEFAULT, "rb").read(), None)
            name, kid, pub = mkkey.trust_line("yours", k).split()
            keys.setdefault(kid, (f"{name}, {mkkey.DEFAULT}", bytes.fromhex(pub)))
    except ImportError:
        pass
    return keys


def show_signature(image, attr, module_len):
    if not attr & ATTR_SIGNED:
        print("  signature      none")
        return
    block = image[module_len - 80:module_len]
    if struct.unpack_from("<I", block, 0)[0] != 0x53395652:
        print("  signature      BAD BLOCK")
        return
    kid = block[4:12].hex()
    known = trusted_keys().get(kid)
    if known is None:
        print(f"  signature      key {kid}, not one known here")
        return
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
    msg = bytearray(image[:module_len - 80])
    struct.pack_into("<I", msg, 32, 0)
    pub = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), known[1])
    sig = encode_dss_signature(int.from_bytes(block[16:48], "big"),
                               int.from_bytes(block[48:80], "big"))
    try:
        pub.verify(sig, bytes(msg), ec.ECDSA(hashes.SHA256()))
        verdict = "ok"
    except InvalidSignature:
        verdict = "DOES NOT MATCH"
    print(f"  signature      key {kid} ({known[0]}): {verdict}")


def show(image, base=0):
    (magic, header_len, abi, module_len, name_off, entry_off,
     static_size, stack_size, mtype, attr, revision, _r0,
     crc, manifest_off) = struct.unpack_from(HDR_FMT, image, 0)

    if magic != MAGIC:
        sys.exit(f"not a module: magic {magic:#010x}")

    name_end = image.index(b"\0", name_off)
    name = image[name_off:name_end].decode("ascii")

    check = bytearray(image[:module_len])
    struct.pack_into("<I", check, 32, 0)
    computed = zlib.crc32(bytes(check)) & 0xFFFFFFFF

    print(f"{name}")
    print(f"  type           {TYPES.get(mtype, mtype)}")
    print(f"  revision       {revision}")
    print(f"  format abi     {abi}")
    print(f"  length         {module_len}")
    print(f"  static         {static_size}")
    print(f"  stack          {stack_size or 'loader decides'}")
    print(f"  entry          +{entry_off}")
    print(f"  crc32          {crc:#010x} "
          f"({'ok' if computed == crc else 'MISMATCH'})")
    show_manifest(image, manifest_off, module_len)
    show_signature(image, attr, module_len)
    print()

    return module_len


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())

    with open(sys.argv[1], "rb") as f:
        data = f.read()

    # A store image is modules end to end, 4-byte aligned.
    off = 0
    while off + HDR_LEN <= len(data):
        if struct.unpack_from("<I", data, off)[0] != MAGIC:
            break
        length = show(data[off:])
        off += length
        off += -off % 4

    if off == 0:
        sys.exit("no module found")


if __name__ == "__main__":
    main()
