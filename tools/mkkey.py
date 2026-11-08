#!/usr/bin/env python3
"""
Make a key to sign RV-9 modules with, and print the line that tells a
board to trust it.

    tools/mkkey.py                      ~/.config/rv9/sign.key, named after you
    tools/mkkey.py --key FILE --name N

The private key never goes near a board or the repository. What a board
holds is the public half, one line of /f0/trust:

    <name> <key id> <public key>

`trust add` on the board takes that line; tools/mksystem.py --install puts
it there too.
"""
import argparse
import getpass
import os
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkmodule  # noqa: E402

DEFAULT = os.path.expanduser("~/.config/rv9/sign.key")


def trust_line(name, key):
    pub = key.public_key().public_bytes(serialization.Encoding.X962,
                                        serialization.PublicFormat.UncompressedPoint)
    return f"{name} {mkmodule.key_id(key.public_key()).hex()} {pub.hex()}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--key", default=DEFAULT)
    ap.add_argument("--name", default=getpass.getuser())
    a = ap.parse_args()

    if os.path.exists(a.key):
        key = serialization.load_pem_private_key(open(a.key, "rb").read(), None)
        print(f"# {a.key} already exists; its trust line:", file=sys.stderr)
    else:
        key = ec.generate_private_key(ec.SECP256R1())
        os.makedirs(os.path.dirname(a.key), mode=0o700, exist_ok=True)
        fd = os.open(a.key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as f:
            f.write(key.private_bytes(serialization.Encoding.PEM,
                                      serialization.PrivateFormat.PKCS8,
                                      serialization.NoEncryption()))
        print(f"# wrote {a.key}; keep it off the boards and out of the repo",
              file=sys.stderr)
    print(trust_line(a.name, key))


if __name__ == "__main__":
    main()
