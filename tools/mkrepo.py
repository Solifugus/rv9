#!/usr/bin/env python3
"""
Make a release of RV-9 modules: a directory any web server can serve, which
a board puts on its module path as /peer/<name>/cmd/ (design section 75).

    OUT/catalog              one line per module:
                             name revision size type signer sha256
    OUT/cmd/<name>.mod       the modules, as built
    OUT/files/<name>         the files they read (their `file` tags), for
                             `get` to fetch beside them

The same layout httpd serves from a board, so a board and a PC look alike
to whoever fetches. The files are taken from --files and must hash to what
each module's tag names; a module whose tag names a hash that no file there
has makes this fail rather than publish a release `get` would refuse.

    tools/mkrepo.py rel                       every program built, signed
    tools/mkrepo.py rel net greet             groups and names, as --store
    tools/mkrepo.py rel --files out --serve 8099

Modules are taken from build/modules (tools/build_modules.sh), which signs
them with ~/.config/rv9/sign.key when it exists. An unsigned module is
published with a warning: a board refuses one from a peer.

--serve PORT serves the release from this machine until interrupted, and
prints the line a board's /f0/peers wants. Read-only, like httpd.
"""
import argparse
import functools
import hashlib
import http.server
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mksystem  # noqa: E402  (modules(), select(), lan_address())

ROOT = mksystem.ROOT
HDR_FMT = "<IHHIIIIIBBBBII"
HDR_LEN = 40
MAGIC = 0x4D395652
TYPES = {1: "program", 5: "descriptor"}
ATTR_SIGNED = 0x01
SIG_MAGIC = 0x53395652
SIG_LEN = 80
TAG_FILE = 0x001A


def fail(msg):
    sys.exit(f"mkrepo: {msg}")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


@functools.lru_cache(maxsize=None)
def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for piece in iter(lambda: f.read(1 << 20), b""):
            h.update(piece)
    return h.hexdigest()


def read_module(path):
    """(name, revision, type, signer key id or '-', [file tag values])."""
    image = open(path, "rb").read()
    (magic, _hl, _abi, module_len, name_off, _entry, _st, _sk, mtype, attr,
     revision, _r0, _crc, man_off) = struct.unpack_from(HDR_FMT, image, 0)
    if magic != MAGIC or module_len != len(image):
        fail(f"{path}: not a whole module")
    name = image[name_off:image.index(b"\0", name_off)].decode("ascii")

    signer = "-"
    if attr & ATTR_SIGNED and module_len >= HDR_LEN + SIG_LEN:
        magic_s, = struct.unpack_from("<I", image, module_len - SIG_LEN)
        if magic_s == SIG_MAGIC:
            signer = image[module_len - SIG_LEN + 4:module_len - SIG_LEN + 12].hex()

    files = []
    off = man_off
    while man_off and off + 4 <= module_len:
        tag, length = struct.unpack_from("<HH", image, off)
        if tag & 0x7FFF == 0:
            break
        if tag & 0x7FFF == TAG_FILE:
            files.append(image[off + 4:off + 4 + length].decode("utf-8"))
        off += 4 + length
        off += -off % 4
    return image, name, revision, TYPES.get(mtype, "other"), signer, files


def find_file(name, dirs):
    for d in dirs:
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    return None


def make(out, chosen, mod_dir, file_dirs):
    lines, files, unsigned, missing = [], {}, [], []
    for n in sorted(chosen):
        src = os.path.join(mod_dir, n + ".mod")
        if not os.path.exists(src):
            continue                     # a library or a descriptor: none
        image, name, rev, mtype, signer, tags = read_module(src)
        if mtype != "program":
            continue
        if signer == "-":
            unsigned.append(name)
        lines.append(f"{name} {rev} {len(image)} {mtype} {signer} {sha256(image)}")
        for t in tags:
            path, _, digest = t.partition("=")
            if not path.startswith("/sd0/"):
                continue                  # not served: httpd serves the card
            base = path[len("/sd0/"):]
            have = find_file(base, file_dirs)
            if have is None:
                missing.append(f"{base} (for {name})")
            elif digest and file_sha256(have) != digest:
                fail(f"{have} is not the build {name} names: {digest[:16]}..., "
                     f"and this is {file_sha256(have)[:16]}...")
            else:
                files[base] = have

    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(os.path.join(out, "cmd"))
    os.makedirs(os.path.join(out, "files"))
    for line in lines:
        name = line.split()[0]
        shutil.copyfile(os.path.join(mod_dir, name + ".mod"),
                        os.path.join(out, "cmd", name + ".mod"))
    for base, src in sorted(files.items()):
        shutil.copyfile(src, os.path.join(out, "files", base))
    with open(os.path.join(out, "catalog"), "w") as f:
        f.write("".join(line + "\n" for line in lines))

    size = sum(os.path.getsize(p) for p in files.values())
    print(f"{out}: {len(lines)} modules, {len(files)} files ({size // 1024:,} KB)")
    if unsigned:
        print(f"  unsigned, which a board refuses from a peer: {' '.join(unsigned)}")
    if missing:
        print(f"  not in {" ".join(file_dirs)}, so `get` "
              f"cannot fetch them: {', '.join(missing)}")


class Quiet(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write(f"  {self.address_string()} {fmt % args}\n")


def serve(out, port):
    host = mksystem.lan_address()
    handler = functools.partial(Quiet, directory=out)
    with http.server.ThreadingHTTPServer(("", port), handler) as httpd:
        print(f"serving {out} at {host}:{port}; for a board's /f0/peers:")
        print(f"  <name> {host}:{port}")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n\n", 1)[1])
    ap.add_argument("out", help="the directory to write; replaced")
    ap.add_argument("select", nargs="*", default=["all"],
                    help="groups and module names (default: all)")
    ap.add_argument("--modules", default=os.path.join(ROOT, "build", "modules"),
                    help="where the built modules are (default build/modules)")
    ap.add_argument("--files", action="append",
                    help="where the files they read are; repeatable "
                         "(default out/)")
    ap.add_argument("--serve", type=int, metavar="PORT",
                    help="then serve it over HTTP on this port")
    a = ap.parse_args()

    # A name built here but with no build.conf -- a module made by hand with
    # tools/mkmodule.py into --modules -- is taken as named.
    mods, _ = mksystem.modules()
    loose = {w for w in a.select if w not in mods
             and os.path.exists(os.path.join(a.modules, w + ".mod"))}
    words = " ".join(w for w in a.select if w not in loose)
    chosen = mksystem.select(words, mods, "select") if words else set()
    chosen = {n for n in chosen | loose if not n.startswith("desc_")}
    make(a.out, chosen, a.modules, a.files or [os.path.join(ROOT, "out")])
    if a.serve:
        serve(a.out, a.serve)


if __name__ == "__main__":
    main()
