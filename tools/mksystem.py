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
    internal RAM, and the card
  - the card, in card/: the commands `card` names that the store does not
    have, as cmd/<name>.mod (the module path, design section 67), and the
    files that what the system carries reads (each build.conf's `files`),
    taken from --files

The card is RBF, which a PC cannot write, so --install puts it there over
WiFi: it serves card/ over HTTP from this machine and, on the board's
console, fetches each file that is missing or the wrong size. Run again, it
fetches nothing.

    tools/mksystem.py systems/c3.sys                  build it and report
    tools/mksystem.py systems/c3.sys --report         report on the last build
    tools/mksystem.py systems/c3.sys --flash PORT     build, then write both
    tools/mksystem.py systems/p4.sys --install PORT   build, then fill the card

Needs the IDF environment (. $HOME/esp/esp-idf/export.sh). A system file is
shell-shaped, like build.conf: key="value" lines and # comments.
"""
import argparse
import functools
import glob
import http.server
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

KEYS = {"target", "drivers", "selftest", "store"}
OPTIONAL = {"card": ""}

# Drivers that are built anywhere but attach only on the P4 (the #if in each).
P4_ONLY = {"audio", "camera", "tts"}

# What each chip has of what a module may declare it needs (isa=).
TARGET_ISA = {"esp32p4": {"fpu", "pie"}}

# The drivers every firmware has, whose devices nothing declares.
CORE_DRIVERS = {"uart", "flashdisk", "pubmem", "pipemem"}


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
            if m[1] not in KEYS and m[1] not in OPTIONAL:
                fail(f"{path}:{n}: no such key '{m[1]}'")
            sysdef[m[1]] = m[2]
    for k in KEYS:
        if k not in sysdef:
            fail(f"{path}: '{k}' is missing")
    for k, v in OPTIONAL.items():
        sysdef.setdefault(k, v)
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


def modules():
    """Every module with a build.conf: (groups, files), and its needs."""
    mods, needs = {}, {}
    for conf in glob.glob(os.path.join(ROOT, "modules", "*", "build.conf")):
        text = open(conf).read()

        def key(k):
            m = re.search(rf'^{k}="?([^"\n#]*)"?', text, re.M)
            return m[1].split() if m else []
        name = os.path.basename(os.path.dirname(conf))
        mods[name] = (set(key("groups")), key("files"))
        needs[name] = {"devices": key("devices") + key("exclusives"),
                       "isa": key("isa"), "modules": key("modules"),
                       "external": key("heap_in") == ["external"]}
    return mods, needs


def device_drivers():
    """Each device's driver, from the descriptors: /cam0 is camera."""
    out = {}
    for conf in glob.glob(os.path.join(ROOT, "modules", "*", "descriptor.conf")):
        text = open(conf).read()
        d = re.search(r"^dev_name=(\S+)", text, re.M)
        v = re.search(r"^driver=(\S+)", text, re.M)
        if d and v:
            out[d[1]] = v[1]
    return out


def need_warnings(carried, sysdef, drivers, cfg):
    """Each carried module whose declared needs this system cannot meet,
    gathered by reason: {"needs /cam0, which this firmware lacks": [snap]}."""
    _, needs = modules()
    dev_driver = device_drivers()
    isa_here = TARGET_ISA.get(sysdef["target"], set())
    psram = cfg.get("CONFIG_SPIRAM") == "y"
    out = {}
    for n in sorted(carried):
        nd = needs.get(n)
        if nd is None:
            continue
        for path in nd["devices"]:
            dev = "/" + path.split("/")[1]
            drv = dev_driver.get(dev)
            if drv and drv not in CORE_DRIVERS and drv not in drivers:
                out.setdefault(f"needs {dev}, which this firmware lacks", []).append(n)
        lack = set(nd["isa"]) - isa_here
        if lack:
            out.setdefault(f"needs {' '.join(sorted(lack))}, which "
                           f"{sysdef['target']} lacks", []).append(n)
        if nd["external"] and not psram:
            out.setdefault("keeps its arena in external memory, which this "
                           "board lacks", []).append(n)
        for m in nd["modules"]:
            if m not in carried:
                out.setdefault(f"starts {m}, which this system does not carry",
                               []).append(n)
    return out


def select(words, mods, what):
    """The modules a list of groups and names picks, as build_modules.sh does."""
    w = set(words.split())
    known = {"all"} | set(mods) | {g for gs, _ in mods.values() for g in gs}
    if w - known:
        fail(f"{what}: '{' '.join(sorted(w - known))}' is no group or module")
    return {n for n, (gs, _) in mods.items() if "all" in w or n in w or gs & w}


def card_plan(build, sysdef, drivers, files_dir):
    """What goes on the card: [(name on the card, file here)], and what is
    missing here. The store's contents are read from its build log."""
    mods, _ = modules()
    log = os.path.join(build, "store.txt")
    text = open(log).read() if os.path.exists(log) else ""
    m = re.search(r"built, not in the store: (.*)", text)
    left = set(m[1].split()) if m else set()
    count = re.search(r"\((\d+) bytes, (\d+) modules\)", text)

    stored = set(mods) - left
    commands = select(sysdef["card"], mods, "card") & left
    if commands and "sdcard" not in drivers:
        fail("card names commands, and the firmware has no sdcard")

    plan, missing = [], []
    for n in sorted(commands):
        plan.append((f"cmd/{n}.mod", os.path.join(ROOT, "build", "modules", n + ".mod")))
    if "sdcard" in drivers:
        wanted = sorted({f[len("/sd0/"):] for n in stored | commands
                         for f in mods[n][1] if f.startswith("/sd0/")})
        for f in wanted:
            src = os.path.join(files_dir, f)
            if os.path.exists(src):
                plan.append((f, src))
            else:
                missing.append(f)
    return (plan, missing, left - commands, int(count[2]) if count else 0,
            (stored | commands) & set(mods))


def make_card(build, plan):
    card = os.path.join(build, "card")
    shutil.rmtree(card, ignore_errors=True)
    os.makedirs(os.path.join(card, "cmd"))
    for name, src in plan:
        os.symlink(os.path.abspath(src), os.path.join(card, name))
    return card


def kb(n):
    return f"{(n + 1023) // 1024:,} KB"


def report(name, build, sysdef, drivers, files_dir):
    cfg = sdkconfig(build)
    parts = partitions(cfg)
    app = os.path.join(build, "rv9.bin")
    store = os.path.join(build, "modules.bin")
    if not os.path.exists(app) or not os.path.exists(store):
        fail(f"no build in {os.path.relpath(build, ROOT)}; run without --report")
    fw, st = os.path.getsize(app), os.path.getsize(store)
    plan, missing, unused, count, carried = card_plan(build, sysdef, drivers, files_dir)
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
    if "sdcard" in drivers:
        size = sum(os.path.getsize(src) for _, src in plan)
        ncmd = sum(1 for n, _ in plan if n.startswith("cmd/"))
        print(f"  {'card':<10}{kb(size):>10}  {ncmd} commands, {len(plan) - ncmd} files")
        if missing:
            print(f"  missing   {' '.join(missing)}, not in {os.path.relpath(files_dir)}"
                  " (docs/tutorial.md, \"Putting the models on the card\")")
    else:
        print(f"  {'card':<10}{'none':>10}")
    descs = {n for n in unused if os.path.exists(os.path.join(ROOT, "modules", n, "descriptor.conf"))}
    if unused - descs:
        print(f"  not here  {' '.join(sorted(unused - descs))}")
    print(f"  drivers   {' '.join(sorted(drivers)) or '(the core only)'}")

    warned = False
    for why, names in need_warnings(carried, sysdef, drivers, cfg).items():
        print(f"  warning: {' '.join(names)}: {why}")
        warned = True
    if sysdef["target"] != "esp32p4" and P4_ONLY & drivers:
        print(f"  warning: {' '.join(sorted(P4_ONLY & drivers))} attach only on "
              "the P4; here they are built and do nothing")
        warned = True
    for what, n, part in (("firmware", fw, "factory"), ("store", st, "modules")):
        if part in parts and n > parts[part]:
            fail(f"the {what} does not fit its partition")
    return warned


def lan_address():
    """This machine's address on the way out, which the board can reach."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("192.0.2.1", 9))
        return s.getsockname()[0]


class Console:
    """The board's shell, over its console. Opened without the reset that
    toggling DTR and RTS would be; a C5's or C3's own USB console restarts
    the chip when it is opened all the same, so the network may not be up
    yet when the prompt is."""

    def __init__(self, port):
        import serial
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.2
        self.s.dtr = self.s.rts = False
        self.s.open()

    def run(self, line, timeout):
        self.s.reset_input_buffer()
        self.s.write(line.encode() + b"\r")
        out, end = b"", time.time() + timeout
        while time.time() < end:
            out += self.s.read(4096)
            if out.endswith(b"rv9> "):
                return out.decode(errors="replace")
        fail(f"no prompt after '{line}' in {timeout} s:\n{out.decode(errors='replace')[-500:]}")

    def listing(self, dev):
        out = self.run(f"dir {dev}", 20)
        if "cannot open" in out:
            fail(f"{dev} is not there: no card in the board?")
        return {m[1]: int(m[2]) for m in re.finditer(r"^(\S+)\s+(\d+)\r?$", out, re.M)}


def install(port, card, plan, host, http_port, sign_key=None):
    handler = functools.partial(QuietHandler, directory=card)
    server = http.server.ThreadingHTTPServer(("", http_port), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    con = Console(port)
    con.run("", 10)

    # The key the card's modules are signed with, in /f0/trust: from then
    # on the board runs modules from files only when they are signed by it
    # (rv9_mod_sig_t). Added once; `trust remove` takes it away.
    if sign_key and os.path.exists(sign_key):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import mkkey
        from cryptography.hazmat.primitives import serialization
        key = serialization.load_pem_private_key(open(sign_key, "rb").read(), None)
        line = mkkey.trust_line(os.environ.get("USER", "builder"), key)
        if line.split()[1] not in con.run("trust", 10):
            out = con.run(f"trust add {line}", 20)
            if "trusted" not in out:
                fail(f"could not add the signing key to /f0/trust:\n{out[-300:]}")
            print(f"trust: added key {line.split()[1]}; modules from files must "
                  "now be signed by it")

    have = con.listing("/sd0")
    todo = [(n, os.path.getsize(src)) for n, src in plan
            if have.get(n) != os.path.getsize(src)]
    print(f"card: {len(plan) - len(todo)} of {len(plan)} already there"
          + (f", fetching {len(todo)} from {host}:{http_port}" if todo else ""))
    for n, size in todo:
        t = time.time()
        print(f"  {n} ({kb(size)})...", end="", flush=True)
        # The headers reach the console, the body the file. Anything but a
        # 200 is retried for half a minute: the board's WiFi may still be
        # joining, and a fetch then fails at once with an empty file.
        for tries in range(10):
            out = con.run(f"fetch {host}:{http_port} /{n} > /sd0/{n}", 60 + size // 20000)
            if re.search(r"HTTP/1\.\d 200", out):
                break
            time.sleep(3)
        else:
            fail(f"fetching {n} kept failing:\n{out[-500:]}")
        print(f" {time.time() - t:.0f} s")

    have = con.listing("/sd0")
    bad = [n for n, src in plan if have.get(n) != os.path.getsize(src)]
    server.shutdown()
    if bad:
        fail(f"wrong size on the card afterwards: {' '.join(bad)}")
    print("card: every file there, at its size")


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("system", help="a systems/*.sys file")
    ap.add_argument("--report", action="store_true", help="report only")
    ap.add_argument("--flash", metavar="PORT", help="write firmware and store")
    ap.add_argument("--install", metavar="PORT", help="fill the card, over WiFi")
    ap.add_argument("--files", default=os.path.join(ROOT, "out"),
                    help="where the card's files are (default out/)")
    ap.add_argument("--host", help="this machine's address, as the board sees it")
    ap.add_argument("--http-port", type=int, default=8000)
    ap.add_argument("--sign", default=os.path.expanduser("~/.config/rv9/sign.key"),
                    help="sign the modules with this key, when it exists "
                         "(default ~/.config/rv9/sign.key; tools/mkkey.py)")
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
        if a.sign and os.path.exists(a.sign):
            cmd += ["--sign", a.sign]
        print("+ " + " ".join(cmd), flush=True)
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        with open(os.path.join(build, "store.txt"), "w") as f:
            f.write(r.stdout + r.stderr)
        if r.returncode:
            sys.stderr.write(r.stdout[-2000:] + r.stderr)
            fail("the store did not build")

    report(name, build, sysdef, drivers, a.files)
    plan, missing, _, _, _ = card_plan(build, sysdef, drivers, a.files)
    card = make_card(build, plan)

    if a.flash:
        run(["idf.py", "-B", build, "-p", a.flash, "flash"])
        run([os.path.join(ROOT, "tools", "flash_modules.sh"), a.flash,
             os.path.join(build, "sdkconfig"), os.path.join(build, "modules.bin")])

    if a.install:
        if "sdcard" not in drivers:
            fail("this system has no card")
        if missing:
            fail(f"not in {a.files}: {' '.join(missing)}")
        if a.flash:
            time.sleep(8)       # the board's boot, after flashing
        install(a.install, card, plan, a.host or lan_address(), a.http_port, a.sign)


if __name__ == "__main__":
    main()
