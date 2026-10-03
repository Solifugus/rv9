#!/usr/bin/env python3
"""
Does the host stub still agree with the firmware it stands in for?

tools/hosttest/stub/rv9/io.h mirrors a handful of codes from rv9/module.h,
because pulling the real header into a build that only wants one driver drags
the whole module ABI with it. That trade is fine. The hazard it carries is not
the copying -- it is that a renumbered code **would not break the build**. The
host test would go on passing while exercising a different code from the one
the firmware uses, which is the quietest kind of wrong.

So the copies are checked. Every RV9_* define in the stub must exist upstream
with the same value; anything else is free to differ, because the stub is a
stub and is allowed to be thin.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

sys.path.insert(0, HERE)
from mkprofile import defines, read  # noqa: E402 -- one parser, not two

STUB = "tools/hosttest/stub/rv9/io.h"
REAL = ["components/rv9_module/include/rv9/module.h",
        "components/rv9_io/include/rv9/io.h"]


def main():
    stub = defines(read(STUB), "RV9_")
    real = {}
    for rel in REAL:
        real.update(defines(read(rel), "RV9_"))

    bad = []
    for name, value in sorted(stub.items()):
        if name not in real:
            bad.append("%s is in the stub and nowhere upstream" % name)
        elif real[name] != value:
            bad.append("%s is %d in the stub and %d upstream"
                       % (name, value, real[name]))

    if bad:
        print("checkstub: %s has drifted:" % STUB, file=sys.stderr)
        for b in bad:
            print("  - %s" % b, file=sys.stderr)
        return 1

    print("host stub agrees with the firmware on all %d mirrored names"
          % len(stub))
    return 0


if __name__ == "__main__":
    sys.exit(main())
