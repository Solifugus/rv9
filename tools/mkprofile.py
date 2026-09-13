#!/usr/bin/env python3
"""
Generate RV-9's target profile: what a compiler targeting RV-9 may rely on.

R9's design names three contracts between the language and the system,
and this is the first of them -- "the RV-9 toolchain supplies a
machine-readable target profile describing its ABI, module format,
supported manifest entries, execution classes, and operations known to be
real-time safe."

Everything here is read out of the sources rather than written down
beside them, because a profile maintained by hand is a profile that is
wrong the first time somebody forgets it. In particular:

  - manifest tags come from tools/mkmodule.py, the producer, and are
    cross-checked against module.h, the consumer. A tag either side is
    missing is an error, not a silent omission.

  - a tag is "enforced" only if some firmware source other than the
    header refers to it. Registered-but-ignored is reported as such.

  - real-time safety is read off the RV9_RT_CODE attribute on the
    function that actually implements each call, file manager operation
    and driver operation -- the same attribute that decides whether the
    code is still there when the flash cache is not.

  - every fault code must have a name in R9's vocabulary. A new fault
    added to RV-9 without one stops this script, which is the point: the
    mapping is a language decision and has to be made by somebody.

The board this runs on has a second profile -- its devices, its limits,
where the radio sits -- which only the board knows. The `profile` command
prints that one.

    tools/mkprofile.py           write docs/target/rv9-profile.json
    tools/mkprofile.py --check   fail if that file is out of date
"""
import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "docs", "target", "rv9-profile.json")

sys.path.insert(0, HERE)
import mkmodule  # noqa: E402  -- the manifest producer is the source of truth

MODULE_H = "components/rv9_module/include/rv9/module.h"
KAL_H = "components/rv9_kal/include/rv9/kal.h"
PROC_H = "components/rv9_proc/include/rv9/proc.h"

# How RV-9's reasons for stopping a component are named in R9 (§15.3).
# None means "not a fault in the language's sense": a process that was
# killed was stopped by an operator, not by its own failure.
R9_FAULT_NAMES = {
    "NONE": None,
    "STACK": "STACK",
    "KILLED": None,
    "DEADLINE": "DEADLINE",
    # Decided 2026-09-13: a component that stops coming back to wait has
    # missed its deadline. RV-9 keeps RUNAWAY in its own diagnostics.
    "RUNAWAY": "DEADLINE",
}

# Calls that forward to a device: bounded only when the device path is.
DEVICE_CALLS = {"read", "write"}


def fail(msg):
    sys.exit("mkprofile: " + msg)


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def strip_comments(text):
    return re.sub(r"/\*.*?\*/", " ", text, flags=re.S)


def defines(text, prefix):
    """Integer #defines starting with prefix: plain, parenthesised, or 1u<<n."""
    out = {}
    pat = re.compile(r"^#define\s+(%s\w*)\s+(.+?)\s*(?:/\*.*)?$" % prefix, re.M)
    for m in pat.finditer(text):
        val = m.group(2).strip()
        shift = re.fullmatch(r"\(\s*1u?\s*<<\s*(\d+)\s*\)", val)
        plain = re.fullmatch(r"\(?\s*(-?(?:0x[0-9A-Fa-f]+|\d+))u?\s*\)?", val)
        if shift:
            out[m.group(1)] = 1 << int(shift.group(1))
        elif plain:
            out[m.group(1)] = int(plain.group(1), 0)
    return out


def enum_values(text, prefix, typename):
    """Members of one named enum: `typedef enum { ... } typename;`.

    Scoped to the one typedef, because prefixes are shared: RV9_MOD_ names
    both the module types and the module errors, and the first version of
    this listed RV9_MOD_OK as a kind of module.
    """
    m = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*%s\s*;" % typename, text, re.S)
    if not m:
        fail("cannot find enum %s" % typename)
    body = strip_comments(m.group(1))
    return {n.group(1): int(n.group(2))
            for n in re.finditer(r"\b(%s\w+)\s*=\s*(\d+)" % prefix, body)}


def short(names, prefix):
    return {k[len(prefix):]: v for k, v in sorted(names.items(),
                                                  key=lambda kv: kv[1])}


def rt_functions(text):
    """Names of functions defined with RV9_RT_CODE in this source."""
    body = strip_comments(text)
    return set(re.findall(r"RV9_RT_CODE\b[^;{()=]*?\b(\w+)\s*\(", body))


def firmware_sources():
    """The system proper. Not main/: a boot test that builds a manifest
    naming a tag is not a consumer of it, and counting one made `desc` look
    enforced when nothing in RV-9 ever reads it."""
    return sorted(glob.glob(os.path.join(ROOT, "components", "*", "src", "*.c")))


# ---- manifest ----

def manifest(module_h):
    tags = defines(module_h, "RV9_MTAG_")
    mandatory = tags.pop("RV9_MTAG_MANDATORY")
    tag_max = tags.pop("RV9_MTAG_MAX")

    sources = {f: strip_comments(open(f, encoding="utf-8").read())
               for f in firmware_sources()}

    out = []
    for name, number in sorted(mkmodule.TAGS.items(), key=lambda kv: kv[1]):
        macro = "RV9_MTAG_" + name.upper()
        if macro not in tags:
            fail("mkmodule.py has tag %r but module.h has no %s" % (name, macro))
        if tags[macro] != number:
            fail("%s is %#x in module.h and %#x in mkmodule.py"
                 % (macro, tags[macro], number))
        if name == "end":
            continue

        if name in mkmodule.U32_TAGS:
            encoding = "u32"
        elif name in mkmodule.U8_TAGS:
            encoding = "u8"
        elif name in mkmodule.STR_TAGS:
            encoding = "string"
        elif name in mkmodule.FS_TAGS:
            encoding = "u32 value, then device path"
        else:
            fail("no encoding known for tag %r" % name)

        entry = {
            "name": name,
            "tag": number,
            "encoding": encoding,
            "repeatable": name in mkmodule.REPEATABLE,
            "enforced": any(re.search(r"\b%s\b" % macro, src)
                            for src in sources.values()),
        }
        names = mkmodule.U8_NAMES.get(name)
        if names:
            entry["values"] = dict(sorted(names.items(), key=lambda kv: kv[1]))
        out.append(entry)

    for macro in tags:
        if macro[len("RV9_MTAG_"):].lower() not in mkmodule.TAGS:
            fail("module.h has %s but mkmodule.py cannot produce it" % macro)

    return {
        "mandatory_bit": mandatory,
        "highest_known_tag": tag_max,
        "unknown_advisory": "skipped",
        "unknown_mandatory": "module refused",
        "layout": "entries of u16 tag, u16 length, value, padded to 4 bytes; "
                  "ends at tag 0 or the end of the module",
        "tags": out,
    }


# ---- the call surface ----

def env_calls(module_h):
    m = re.search(r"typedef struct \{\s*/\* --- ABI 1 --- \*/(.*?)\}\s*rv9_mod_env_t;",
                  module_h, re.S)
    if not m:
        fail("cannot find rv9_mod_env_t in module.h")

    body = re.sub(r"/\*\s*---\s*ABI\s+(\d+)[^*]*\*/", r"@@ABI \1@@", m.group(1))
    body = "@@ABI 1@@" + strip_comments(body)

    module_c = read("components/rv9_module/src/module.c")
    proc_c = read("components/rv9_proc/src/proc.c")
    impl = dict(re.findall(r"env->(\w+)\s*=\s*(\w+)\s*;", module_c))
    impl.update(re.findall(r"\benv\.(\w+)\s*=\s*(\w+)\s*;", proc_c))
    rt = rt_functions(module_c) | rt_functions(proc_c)

    calls, abi = [], 1
    for piece in re.split(r"(@@ABI \d+@@)", body):
        marker = re.fullmatch(r"@@ABI (\d+)@@", piece)
        if marker:
            abi = int(marker.group(1))
            continue
        for decl in piece.split(";"):
            decl = " ".join(decl.split())
            if not decl:
                continue
            fn = re.search(r"\(\s*\*\s*(\w+)\s*\)\s*\(", decl)
            if fn:
                name = fn.group(1)
                f = impl.get(name)
                if f is None or f == "NULL":
                    safety = "no"
                elif f in rt:
                    safety = "device" if name in DEVICE_CALLS else "yes"
                else:
                    safety = "no"
                calls.append({"name": name, "kind": "call", "since_abi": abi,
                              "rt_safe": safety})
            else:
                var = re.search(r"(\w+)\s*$", decl)
                if var:
                    calls.append({"name": var.group(1), "kind": "value",
                                  "since_abi": abi})
    return calls


def io_layers():
    """File managers and drivers, and whether their data paths are resident."""
    managers, drivers = [], []
    files = glob.glob(os.path.join(ROOT, "components", "*", "src", "*.c"))
    for path in sorted(files):
        text = open(path, encoding="utf-8").read()
        rt = rt_functions(text)
        for kind, block in re.findall(
                r"static const rv9_(filemgr|driver)_t \w+ = \{(.*?)\};",
                strip_comments(text), re.S):
            fields = dict(re.findall(r"\.(\w+)\s*=\s*([\w\"]+)", block))
            name = fields.get("name", "").strip('"')
            if not name:
                continue

            def resident(*keys):
                present = [fields[k] for k in keys if k in fields]
                if not present:
                    return None
                return all(f in rt for f in present)

            if kind == "filemgr":
                managers.append({"name": name,
                                 "read_rt": resident("read"),
                                 "write_rt": resident("write")})
            else:
                drivers.append({
                    "name": name,
                    "read_rt": resident("unit_read") if "unit_read" in fields
                               else resident("read"),
                    "write_rt": resident("unit_write") if "unit_write" in fields
                                else resident("write"),
                    "retains": fields.get("retains") == "true",
                    "sessions": "open" in fields,
                })
    managers.sort(key=lambda e: e["name"])
    drivers.sort(key=lambda e: e["name"])
    return managers, drivers


# ---- everything ----

def profile():
    module_h = read(MODULE_H)
    kal_h = read(KAL_H)
    proc_h = read(PROC_H)
    kal_rt = read("components/rv9_kal/src/kal_rt.c")
    proc_c = read("components/rv9_proc/src/proc.c")

    abi = defines(module_h, "RV9_MODULE_")
    faults = short(defines(module_h, "RV9_FAULT_"), "RV9_FAULT_")
    for name in faults:
        if name not in R9_FAULT_NAMES:
            fail("fault %s has no R9 name; decide one in R9_FAULT_NAMES" % name)

    managers, drivers = io_layers()

    def one(text, macro):
        v = defines(text, macro).get(macro)
        if v is None:
            fail("cannot read %s" % macro)
        return v

    head = re.search(r"sizeof\(rv9_pub_t\)\s*==\s*(\d+)", module_h)

    return {
        "profile": "rv9",
        "format": 1,
        "generated_by": "tools/mkprofile.py -- regenerate, do not edit",
        "module": {
            "magic": abi["RV9_MODULE_MAGIC"],
            "abi": abi["RV9_MODULE_ABI"],
            "header_bytes": abi["RV9_MODULE_HDR_LEN"],
            "types": short(enum_values(module_h, "RV9_MOD_", "rv9_mod_type_t"),
                           "RV9_MOD_"),
            "position": "independent: PC-relative code and rodata as one blob, "
                        "no writable data, no external symbols",
        },
        "manifest": manifest(module_h),
        "execution_classes": short(enum_values(module_h, "RV9_MCLASS_",
                                               "rv9_mod_class_t"),
                                   "RV9_MCLASS_"),
        "on_deadline": short(defines(module_h, "RV9_ON_DEADLINE_"),
                             "RV9_ON_DEADLINE_"),
        "faults": [{"name": n, "value": v, "r9_reason": R9_FAULT_NAMES[n]}
                   for n, v in faults.items()],
        "process_errors": short(defines(module_h, "RV9_PE_"), "RV9_PE_"),
        "io_errors": short(defines(module_h, "RV9_IOE_"), "RV9_IOE_"),
        "signals": short(defines(module_h, "RV9_SIG_"), "RV9_SIG_"),
        "sysinfo": short(defines(module_h, "RV9_SYS_"), "RV9_SYS_"),
        "calls": env_calls(module_h),
        "rt_safety": {
            "yes": "bounded; resident in memory that survives flash operations",
            "device": "bounded when the device's file manager and driver "
                      "operations are both marked real-time safe below",
            "no": "not to be called with a deadline pending",
            "basis": "read from the RV9_RT_CODE attribute on the function "
                     "that implements each entry; what that function calls "
                     "is its implementer's responsibility, not checked here",
        },
        "file_managers": managers,
        "drivers": drivers,
        "realtime": {
            "slots": one(kal_rt, "MAX_RT_TASKS"),
            "runaway_ms": one(kal_h, "RV9_RT_RUNAWAY_MS"),
            "watchdog_us": one(kal_rt, "WATCH_US"),
            "utilisation_ceiling_permille": one(proc_c, "RT_UTIL_MAX_PERMILLE"),
            "levels": {
                "urgent": "above every host task, the radio included",
                "routine": "above every ordinary process, below the radio",
            },
            "placement": "response-time analysis at admission over declared "
                         "periods, deadlines and execution bounds; measured "
                         "execution stands in where none is declared",
        },
        "processes": {
            "history": one(proc_c, "PROC_HISTORY"),
            "history_max": one(proc_c, "PROC_HISTORY_MAX"),
            "max_paths": one(proc_h, "RV9_MAX_PATHS"),
            "pids": [1, 65535],
        },
        "publication": {
            "max_name": one(module_h, "RV9_PUB_MAX_NAME"),
            "head_bytes": int(head.group(1)) if head else None,
            "getstat_wait": one(module_h, "RV9_PUB_GS_WAIT"),
            "getstat_info": one(module_h, "RV9_PUB_GS_INFO"),
        },
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="fail if the committed profile is out of date")
    args = ap.parse_args()

    text = json.dumps(profile(), indent=2) + "\n"

    if args.check:
        try:
            with open(OUT, encoding="utf-8") as f:
                current = f.read()
        except FileNotFoundError:
            fail("%s does not exist; run tools/mkprofile.py" % OUT)
        if current != text:
            fail("%s is out of date; run tools/mkprofile.py" % OUT)
        print("target profile is current")
        return

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
