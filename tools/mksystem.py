#!/usr/bin/env python3
"""
Build an RV-9 system: one board, the devices it has, the commands it needs.

A system file (systems/*.sys) says which chip, which drivers go into the
firmware, whether it carries the self-tests, and which groups of modules go
into its store. From that this builds, in build-<name>/:

  - the firmware, from sdkconfig.defaults (and the chip's own) with the
    system's choices on top, in system.defaults
  - the module store, modules.bin, for that firmware's drivers
  - a size report: firmware and store against their partitions, static
    internal RAM, and what is left for the card

What the store leaves out is still built, in build/modules, for the card.

    tools/mksystem.py systems/c3.sys                build it and report
    tools/mksystem.py systems/c3.sys --report       report on the last build
    tools/mksystem.py systems/c3.sys --flash PORT   build, then write both

Needs the IDF environment (. $HOME/esp/esp-idf/export.sh). A system file is
shell-shaped, like build.conf: key="value" lines and # comments.
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

KEYS = {"target", "drivers", "selftest", "store"}

# What a group's programs open, for the warning when a store carries a group
# whose devices the firmware lacks. Any one of the drivers is enough.
GROUP_NEEDS = {
    "net": {"net"},
    "pins": {"gpio", "pwm", "adc", "tsens", "i2c"},
    "screen": {"svgwin", "lcdcon"},
    "sound": {"audio"},
    "vision": {"camera"},
}

# Drivers that are built anywhere but attach only on the P4 (the #if in each).
P4_ONLY = {"audio", "camera", "tts"}


def fail(msg):
    sys.exit(f"mksystem: {msg}")


def read_system(path):
    sysdef = {}
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = re.fullmatch(r'(\w+)="([^"]*)"', line)
            if not m:
                fail(f"{path}:{n}: expected key=\"value\"")
            if m[1] not in KEYS:
                fail(f"{path}:{n}: no such key '{m[1]}'")
            sysdef[m[1]] = m[2]
    for k in KEYS:
        if k not in sysdef:
            fail(f"{path}: '{k}' is missing")
    if sysdef["selftest"] not in ("yes", "no"):
        fail(f"{path}: selftest is yes or no")
    return sysdef


def known_drivers():
    """Every CONFIG_RV9_DRV_* any component offers, read from the Kconfigs."""
    names = set()
    for k in glob.glob(os.path.join(ROOT, "components", "*", "Kconfig")):
        names |= {m.lower() for m in
                  re.findall(r"^\s*config RV9_DRV_(\w+)", open(k).read(), re.M)}
    return names


def write_defaults(build, sysdef, drivers):
    lines = ["# Written by tools/mksystem.py from the system file. Do not edit."]
    for d in sorted(known_drivers()):
        if d in drivers:
            lines.append(f"CONFIG_RV9_DRV_{d.upper()}=y")
        else:
            lines.append(f"# CONFIG_RV9_DRV_{d.upper()} is not set")
    if sysdef["selftest"] == "yes":
        lines.append("CONFIG_RV9_SELFTEST=y")
    else:
        lines.append("# CONFIG_RV9_SELFTEST is not set")
    text = "\n".join(lines) + "\n"

    path = os.path.join(build, "system.defaults")
    old = open(path).read() if os.path.exists(path) else None
    if old != text:
        os.makedirs(build, exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
        # Defaults only fill in what an sdkconfig does not already say, so a
        # changed system starts its sdkconfig again. sdkconfig.old keeps it.
        cfg = os.path.join(build, "sdkconfig")
        if os.path.exists(cfg):
            os.replace(cfg, cfg + ".old")
    return path


def cached_target(build):
    cache = os.path.join(build, "CMakeCache.txt")
    if not os.path.exists(cache):
        return None
    m = re.search(r"^IDF_TARGET:STRING=(\S+)", open(cache).read(), re.M)
    return m[1] if m else None


def run(cmd):
    print("+ " + " ".join(cmd), flush=True)
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode:
        fail(f"{cmd[0]} failed ({r.returncode})")


def sdkconfig(build):
    cfg = {}
    for line in open(os.path.join(build, "sdkconfig")):
        m = re.match(r"(CONFIG_\w+)=(.*)", line)
        if m:
            cfg[m[1]] = m[2].strip('"')
    return cfg


def check_config(cfg, sysdef, drivers):
    """The sdkconfig is what was asked for, or Kconfig refused something."""
    got = {d for d in known_drivers() if cfg.get(f"CONFIG_RV9_DRV_{d.upper()}") == "y"}
    if got != drivers:
        fail("the firmware's drivers are not the system's: "
             f"missing {sorted(drivers - got)}, extra {sorted(got - drivers)}")
    if (cfg.get("CONFIG_RV9_SELFTEST") == "y") != (sysdef["selftest"] == "yes"):
        fail("the firmware's self-tests are not the system's")


def partitions(cfg):
    csv = os.path.join(ROOT, cfg.get("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME",
                                     "partitions.csv"))
    parts = {}
    for line in open(csv):
        f = [x.strip() for x in line.split("#")[0].split(",")]
        if len(f) >= 5 and f[0]:
            parts[f[0]] = int(f[4], 0)
    return parts


def static_ram(build):
    """Internal RAM the firmware takes before it runs: DIRAM/DRAM/IRAM."""
    maps = glob.glob(os.path.join(build, "*.map"))
    if not maps:
        return None
    r = subprocess.run([sys.executable, "-m", "esp_idf_size", "--format", "json2",
                        maps[0]], capture_output=True, text=True)
    if r.returncode:
        return None
    used = total = 0
    for region in json.loads(r.stdout).get("layout", []):
        name = region["name"].lower()
        if any(w in name for w in ("flash", "psram", "rtc", "lp ")):
            continue
        used += region.get("used", 0)
        total += region.get("total", 0)
    return used, total


def store_report(build):
    mods = os.path.join(ROOT, "build", "modules")
    log = os.path.join(build, "store.txt")
    count, out = 0, []
    if os.path.exists(log):
        text = open(log).read()
        m = re.search(r"\((\d+) bytes, (\d+) modules\)", text)
        count = int(m[2]) if m else 0
        m = re.search(r"built, not in the store: (.*)", text)
        out = m[1].split() if m else []
    card = sum(os.path.getsize(os.path.join(mods, n + ".mod")) for n in out
               if os.path.exists(os.path.join(mods, n + ".mod")))
    return count, out, card


def kb(n):
    return f"{(n + 1023) // 1024:,} KB"


def report(name, build, sysdef, drivers):
    cfg = sdkconfig(build)
    parts = partitions(cfg)
    app = os.path.join(build, "rv9.bin")
    store = os.path.join(build, "modules.bin")
    if not os.path.exists(app) or not os.path.exists(store):
        fail(f"no build in {os.path.relpath(build, ROOT)}; run without --report")
    fw, st = os.path.getsize(app), os.path.getsize(store)
    count, out, card = store_report(build)
    ram = static_ram(build)

    def line(what, n, part):
        size = parts.get(part)
        if size:
            return f"  {what:<10}{kb(n):>10} of {kb(size):>9} {100 * n // size:3d}%"
        return f"  {what:<10}{kb(n):>10}"

    print(f"\nsystem {name}, {sysdef['target']}")
    print(line("firmware", fw, "factory"))
    print(line("store", st, "modules") + f"   {count} modules")
    if ram is not None:
        print(f"  {'RAM':<10}{kb(ram[0]):>10} of {kb(ram[1]):>9} {100 * ram[0] // max(ram[1], 1):3d}%"
              "   internal, taken before anything runs")
    print(f"  {'for card':<10}{kb(card):>10}  {len(out)} modules built and left out")
    print(f"  drivers   {' '.join(sorted(drivers)) or '(the core only)'}")

    warned = False
    for g, needs in GROUP_NEEDS.items():
        if (g in sysdef["store"].split() or sysdef["store"] == "all") and not needs & drivers:
            print(f"  warning: the store has '{g}' and the firmware none of "
                  f"{' '.join(sorted(needs))}")
            warned = True
    if sysdef["target"] != "esp32p4" and P4_ONLY & drivers:
        print(f"  warning: {' '.join(sorted(P4_ONLY & drivers))} attach only on "
              "the P4; here they are built and do nothing")
        warned = True
    for what, n, part in (("firmware", fw, "factory"), ("store", st, "modules")):
        if part in parts and n > parts[part]:
            fail(f"the {what} does not fit its partition")
    return warned


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("system", help="a systems/*.sys file")
    ap.add_argument("--report", action="store_true", help="report only")
    ap.add_argument("--flash", metavar="PORT", help="write firmware and store")
    a = ap.parse_args()

    sysdef = read_system(a.system)
    name = os.path.splitext(os.path.basename(a.system))[0]
    build = os.path.join(ROOT, f"build-{name}")

    drivers = set(sysdef["drivers"].split())
    unknown = drivers - known_drivers()
    if unknown:
        fail(f"no such driver: {' '.join(sorted(unknown))}")
    if "ssh" in drivers and "net" not in drivers:
        fail("ssh needs net")

    if not a.report:
        was = cached_target(build)
        if was and was != sysdef["target"]:
            fail(f"build-{name} was built for {was}; remove it to build "
                 f"{sysdef['target']} there")
        defaults = write_defaults(build, sysdef, drivers)
        run(["idf.py", "-B", build,
             "-D", f"IDF_TARGET={sysdef['target']}",
             "-D", f"SDKCONFIG={os.path.join(build, 'sdkconfig')}",
             "-D", f"SDKCONFIG_DEFAULTS={os.path.join(ROOT, 'sdkconfig.defaults')};{defaults}",
             "build"])
        check_config(sdkconfig(build), sysdef, drivers)

        cmd = [os.path.join(ROOT, "tools", "build_modules.sh"),
               "--store", sysdef["store"],
               "--sdkconfig", os.path.join(build, "sdkconfig"),
               "--out", os.path.join(build, "modules.bin")]
        print("+ " + " ".join(cmd), flush=True)
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        with open(os.path.join(build, "store.txt"), "w") as f:
            f.write(r.stdout + r.stderr)
        if r.returncode:
            sys.stderr.write(r.stdout[-2000:] + r.stderr)
            fail("the store did not build")

    report(name, build, sysdef, drivers)

    if a.flash:
        run(["idf.py", "-B", build, "-p", a.flash, "flash"])
        run([os.path.join(ROOT, "tools", "flash_modules.sh"), a.flash,
             os.path.join(build, "sdkconfig"), os.path.join(build, "modules.bin")])


if __name__ == "__main__":
    main()
