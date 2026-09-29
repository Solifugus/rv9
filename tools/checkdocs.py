#!/usr/bin/env python3
"""
Does the reference still mention everything the firmware defines?

The argument is the one the generated profile already makes. Whether the
prose about a thing is any good cannot be checked mechanically; whether the
thing is mentioned at all can be. Documentation the build will not let you
forget is a different kind of object from documentation you mean to get back
to -- and the failure this catches is the cheap one: somebody adds a system
call and nothing anywhere says it exists.

The chain of custody matters and is worth stating:

    the sources        --  mkprofile.py --check  -->  rv9-profile.json
    rv9-profile.json   --  this script           -->  docs/reference.md

So the reference is checked against the profile, and the profile is checked
against the code. Neither link is allowed to be stale, which means a call
added to module.h reaches this check without anybody remembering to tell it.

WHAT COUNTS AS MENTIONED

The name inside backticks, somewhere in the document: `wait_why`, or
`RV9_FAULT_STACK`. Not a bare substring -- "open" and "read" occur in
ordinary English on nearly every page, so a substring test would pass
vacuously for exactly the entries most worth documenting.

That is a low bar on purpose. It is a test for *absence*, not for quality,
and a low bar that is actually enforced beats a high one that is not.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

PROFILE = os.path.join(ROOT, "docs", "target", "rv9-profile.json")
REFERENCE = os.path.join(ROOT, "docs", "reference.md")


def mentioned(text):
    """Every identifier the document puts in backticks."""
    return set(re.findall(r"`([A-Za-z_][A-Za-z0-9_]*)`", text))


def structure(text):
    """
    Is the document still shaped like a document?

    Added after a duplicated "## 4. Devices as shipped" sat in the reference
    for six days: an edit inserted a written section *before* the skeleton it
    was meant to replace, and nothing noticed. The name check could not --
    every name was mentioned, twice, which it reads as fine.

    So this is a different question from completeness, and a cheap one:
    numbered headings should appear once each and count from 1 without gaps.
    It would not catch a section that is merely wrong, and is not meant to.
    """
    problems = []
    seen = {}

    for line in text.splitlines():
        m = re.match(r"^## (\d+)\. (.+)$", line)
        if not m:
            continue
        n = int(m.group(1))
        if n in seen:
            problems.append("section %d appears twice: %r and %r"
                            % (n, seen[n], m.group(2)))
        else:
            seen[n] = m.group(2)

    if seen:
        want = list(range(1, max(seen) + 1))
        missing = [n for n in want if n not in seen]
        if missing:
            problems.append("no section " +
                            ", ".join(str(n) for n in missing))

    return problems


def main():
    for path in (PROFILE, REFERENCE):
        if not os.path.exists(path):
            print("checkdocs: %s is missing" % path, file=sys.stderr)
            return 1

    with open(PROFILE) as f:
        p = json.load(f)
    with open(REFERENCE) as f:
        text = f.read()

    bad = structure(text)
    if bad:
        print("checkdocs: docs/reference.md is malformed:", file=sys.stderr)
        for b in bad:
            print("  - %s" % b, file=sys.stderr)
        return 1

    seen = mentioned(text)

    # What must appear, and under which name. A fault or an error is written
    # the way a program writes it, because that is what somebody greps for.
    wanted = []

    for c in p["calls"]:
        wanted.append(("system call", c["name"], c["name"]))

    for t in p["manifest"]["tags"]:
        wanted.append(("manifest tag", t["name"], t["name"]))

    for f in p["faults"]:
        wanted.append(("fault", "RV9_FAULT_" + f["name"], "RV9_FAULT_" + f["name"]))

    for name in p["process_errors"]:
        wanted.append(("process error", "RV9_PE_" + name, "RV9_PE_" + name))

    for name in p["io_errors"]:
        wanted.append(("I/O error", "RV9_IO_ERR_" + name, "RV9_IO_ERR_" + name))

    for name in p["sysinfo"]:
        wanted.append(("sysinfo code", "RV9_SYS_" + name, "RV9_SYS_" + name))

    for fm in p["file_managers"]:
        wanted.append(("file manager", fm["name"], fm["name"]))

    for d in p["drivers"]:
        name = d["name"] if isinstance(d, dict) else d
        wanted.append(("driver", name, name))

    missing = {}
    for kind, shown, token in wanted:
        if token not in seen:
            missing.setdefault(kind, []).append(shown)

    if not missing:
        n = len(wanted)
        print("reference documents all %d names the firmware defines" % n)
        return 0

    total = sum(len(v) for v in missing.values())
    print("checkdocs: docs/reference.md does not mention %d name(s) the "
          "firmware defines:" % total, file=sys.stderr)
    for kind in sorted(missing):
        names = sorted(missing[kind])
        print("\n  %s (%d):" % (kind, len(names)), file=sys.stderr)
        line = "   "
        for nm in names:
            if len(line) + len(nm) + 2 > 74:
                print(line, file=sys.stderr)
                line = "   "
            line += " " + nm
        if line.strip():
            print(line, file=sys.stderr)

    print("\nWrite it in backticks in docs/reference.md, or -- if the name "
          "should not exist --\nremove it from the sources and regenerate "
          "the profile.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
