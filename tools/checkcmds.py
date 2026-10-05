#!/usr/bin/env python3
"""
Does docs/commands.md still name every command this machine has?

A command is a module, so the module directory *is* the list of commands --
which means the list cannot be maintained by hand without going stale the
first time somebody adds one. `mdir` on the board is the truth; this is the
same question asked of the source tree.

The bar is the one checkdocs.py uses, and for the same reason: the name in
backticks, somewhere on the page. Whether the prose about a command is any
good cannot be checked mechanically. Whether the command is mentioned at all
can be, and that is the failure worth catching -- somebody adds a module and
nothing anywhere tells a person it exists.

Device descriptors are not commands and are skipped: `desc_w0` is how /w0
comes to exist, not something you type.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

COMMANDS = os.path.join(ROOT, "docs", "commands.md")
MODULES = os.path.join(ROOT, "modules")


def main():
    if not os.path.exists(COMMANDS):
        print("checkcmds: %s is missing" % COMMANDS, file=sys.stderr)
        return 1

    with open(COMMANDS) as f:
        text = f.read()

    # Command names are not C identifiers -- `st-watch-mm` has hyphens in it,
    # and checkdocs' identifier scanner would read that as three words. So the
    # whole name is looked for inside a backtick span, bounded so that `pub`
    # is not satisfied by `pubs`.
    spans = re.findall(r"`([^`\n]+)`", text)

    def named(n):
        pat = re.compile(r"(?<![A-Za-z0-9_-])" + re.escape(n) +
                         r"(?![A-Za-z0-9_-])")
        return any(pat.search(sp) for sp in spans)

    names = sorted(d for d in os.listdir(MODULES)
                   if os.path.isdir(os.path.join(MODULES, d))
                   and not d.startswith("desc_"))

    missing = [n for n in names if not named(n)]

    if missing:
        print("checkcmds: docs/commands.md does not name %d command(s) this "
              "machine has:" % len(missing), file=sys.stderr)
        line = "   "
        for n in missing:
            if len(line) + len(n) + 2 > 74:
                print(line, file=sys.stderr)
                line = "   "
            line += " " + n
        if line.strip():
            print(line, file=sys.stderr)
        print("\nName it in backticks in docs/commands.md -- in §9 if it is a "
              "demonstration\nrather than something to type.", file=sys.stderr)
        return 1

    print("commands page names all %d modules in the store" % len(names))
    return 0


if __name__ == "__main__":
    sys.exit(main())
