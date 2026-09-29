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


# C spellings that appear inside signatures and are nobody's API name. Only
# one of them collides with something the firmware defines -- the manifest
# tag `static` -- which is why the filter applies to the insides of a
# signature and never to a span that is nothing but a name: `static` on its
# own, as §6 writes it, is somebody naming the tag.
NOISE = {
    "void", "int", "char", "const", "unsigned", "signed", "long", "short",
    "static", "struct", "union", "enum", "return", "sizeof", "bool", "size_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int8_t", "int16_t", "int32_t", "int64_t",
}


def mentioned(text):
    """
    Every identifier the document puts in backticks.

    Originally this matched only a span that was *entirely* an identifier,
    which turned out to punish the better documentation: writing the whole
    signature -- `int close(int path)` -- documents `close` far more usefully
    than the bare name, and the check called it missing. Thirteen calls
    failed that way the first time §7 was written properly.

    So identifiers are taken from *within* each span, less the C spellings
    above. It is still a test for absence and still a low bar; it is now a bar
    that good prose can clear.

    A span that is *only* an identifier is taken as written, blocklist and
    all. Otherwise the manifest tag `static` could never be documented: the
    one way to name it is the one way the filter throws away.
    """
    names = set()
    for span in re.findall(r"`([^`\n]+)`", text):
        lone = re.fullmatch(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*", span)
        if lone:
            names.add(lone.group(1))
            continue
        for ident in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", span):
            if ident not in NOISE:
                names.add(ident)
    return names


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


# Where §14's rows live in the profile. The document names the profile's own
# key so that a reader and a code generator are talking about one quantity;
# this says which quantity, and limits(), below, insists they agree.
LIMITS = {
    "max_paths":                     ("processes", "max_paths"),
    "history":                       ("processes", "history"),
    "budget_default":                ("processes", "budget_default"),
    "budget_ancestors":              ("processes", "budget_ancestors"),
    "slots":                         ("realtime", "slots"),
    "utilisation_ceiling_permille":  ("realtime", "utilisation_ceiling_permille"),
    "watchdog_us":                   ("realtime", "watchdog_us"),
    "runaway_ms":                    ("realtime", "runaway_ms"),
    "rt_reserve":                    ("memory", "rt_reserve"),
    "max_name":                      ("publication", "max_name"),
    "head_bytes":                    ("publication", "head_bytes"),
    "abi":                           ("module", "abi"),
    "header_bytes":                  ("module", "header_bytes"),
}


def limits(text, p):
    """
    Does §14 still say what the firmware does?

    The section promises a table of numbers, and a table of numbers in a
    document is worthless unless something fails when it goes stale -- the
    failure mode is not a wrong number, it is a number that was right in
    March. So each row is read back and compared to the profile, which was
    itself checked against the sources on the way here.

    A row whose value is a range (`pids`, 1-65535) is skipped: it is prose
    about two numbers rather than one number, and the two are named in §7
    where being wrong about them would matter.
    """
    problems = []

    body = re.search(r"^## 14\..*?(?=^## )", text, re.S | re.M)
    if not body:
        problems.append("no section 14 to check")
        return problems

    seen = set()
    for m in re.finditer(r"^\| `([a-z_]+)` \| ([0-9]+) \|", body.group(0), re.M):
        key, shown = m.group(1), int(m.group(2))
        where = LIMITS.get(key)
        if where is None:
            problems.append("section 14 has a row %r the profile has no "
                            "place for" % key)
            continue
        seen.add(key)
        real = p[where[0]][where[1]]
        if shown != real:
            problems.append("section 14 says %s is %d; the profile says %d"
                            % (key, shown, real))

    for key in sorted(set(LIMITS) - seen):
        problems.append("section 14 does not give %s" % key)

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

    bad = structure(text) + limits(text, p)
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
