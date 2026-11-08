#!/usr/bin/env bash
#
# Build every RV-9 module and concatenate them into a module store image.
#
# Modules are compiled outside the IDF build: they are freestanding blobs
# with no libc and no linkage to the kernel, which is the entire point.
#
# Requires the IDF toolchain on PATH (. $HOME/esp/esp-idf/export.sh).
#
# Every module is built, always. Which of them go into the store is chosen:
#
#   --store "base net pins"   groups (each build.conf says its `groups`) or
#                             module names; "all", the default, is all of
#                             them
#   --sdkconfig FILE          the firmware this store is for. Descriptors
#                             go in only for the drivers it was built with
#                             (CONFIG_RV9_DRV_*), and the `tests` group goes
#                             in when it has the self-tests, which fork them.
#                             Without it, every descriptor goes in.
#   --out FILE                the store image; build/modules.bin by default
#   --sign KEY                sign every module with this private key
#                             (tools/mkkey.py), so a board whose /f0/trust
#                             holds its public half will run it from a file.
#                             ~/.config/rv9/sign.key by default, when it
#                             exists; --no-sign builds them unsigned
#
# What is built but left out stays in build/modules, for the card.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SELECT="all"
CFG=""
SIGN=()
NOSIGN=0
STORE="$ROOT/build/modules.bin"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --store)     SELECT="$2"; shift 2 ;;
        --sdkconfig) CFG="$2"; shift 2 ;;
        --out)       STORE="$2"; shift 2 ;;
        --sign)      SIGN=(--sign "$2"); shift 2 ;;
        --no-sign)   SIGN=(); NOSIGN=1; shift ;;
        *) echo "usage: $0 [--store \"groups or names\"] [--sdkconfig FILE] [--out FILE] [--sign KEY | --no-sign]" >&2
           exit 2 ;;
    esac
done
[[ -z "$CFG" || -f "$CFG" ]] || { echo "error: no $CFG" >&2; exit 2; }
if [[ ${#SIGN[@]} -eq 0 && $NOSIGN -eq 0 && -f "$HOME/.config/rv9/sign.key" ]]; then
    SIGN=(--sign "$HOME/.config/rv9/sign.key")
fi

cfg_on() { [[ -n "$CFG" ]] && grep -q "^CONFIG_$1=y" "$CFG"; }

# The drivers a firmware always has (components/rv9_io/CMakeLists.txt).
CORE_DRIVERS=" uart flashdisk pubmem pipemem "

# Every group any module declares, so that a word in --store that is
# neither a group nor a module is caught rather than quietly selecting
# nothing.
KNOWN=" all "
for c in "$ROOT"/modules/*/build.conf; do
    KNOWN+="$(basename "$(dirname "$c")") "
    g=$(sed -n 's/^groups="\(.*\)".*/\1/p' "$c")
    for w in $g; do [[ "$KNOWN" == *" $w "* ]] || KNOWN+="$w "; done
done
WANT=" $SELECT "
[[ -n "$CFG" ]] && cfg_on RV9_SELFTEST && WANT+="tests "
for w in $WANT; do
    [[ "$KNOWN" == *" $w "* ]] || { echo "error: '$w' is no group or module" >&2; exit 2; }
done

# In the store? A descriptor by its driver, a module by its name or groups.
wanted_desc() {
    [[ -z "$CFG" || "$CORE_DRIVERS" == *" $1 "* ]] && return 0
    cfg_on "RV9_DRV_${1^^}"
}
wanted_mod() {
    [[ "$WANT" == *" all "* || "$WANT" == *" $1 "* ]] && return 0
    for g in $2; do [[ "$WANT" == *" $g "* ]] && return 0; done
    return 1
}
in_store=0
left_out=""
OUT="$ROOT/build/modules"

CC=riscv32-esp-elf-gcc
OBJCOPY=riscv32-esp-elf-objcopy

command -v "$CC" >/dev/null || {
    echo "error: $CC not on PATH. Run: . \$HOME/esp/esp-idf/export.sh" >&2
    exit 1
}

# -mcmodel=medany is what makes modules relocatable: every symbol reference
# becomes PC-relative, so the blob works wherever the loader puts it.
CFLAGS=(
    -march=rv32imac_zicsr_zifencei_zaamo_zalrsc
    -mabi=ilp32
    -mcmodel=medany
    -Os
    -ffreestanding
    -fno-builtin
    -fno-jump-tables            # jump tables hold absolute addresses
    -fno-tree-switch-conversion # so do switch-to-lookup-table rewrites
    -ffunction-sections
    -fdata-sections
    -Wall -Wextra -Werror
    -I"$ROOT/components/rv9_module/include"
    -I"$ROOT/modules"
    -I"$ROOT/components/rv9_io/include"
    -I"$ROOT/components/rv9_ssh/include"
)
LDFLAGS=(-nostdlib -nostartfiles -T"$ROOT/modules/module.ld" -Wl,--gc-sections)

mkdir -p "$OUT" "$(dirname "$STORE")"
: > "$STORE"

# The probe link, for the position-independence check below: the same script
# at a different base. Both bases are above the reach of a 12-bit immediate,
# so neither link can be relaxed into an absolute load -- see module.ld.
PROBE_BASE=(-Wl,--defsym,RV9_BASE=0x8000)

shopt -s nullglob
for dir in "$ROOT"/modules/*/; do
    name="$(basename "$dir")"

    # A device descriptor is a data module: no code, just the binding of a
    # device name to a file manager and a driver. Adding a device to the
    # system is adding one of these -- no kernel rebuild.
    if [[ -f "$dir/descriptor.conf" ]]; then
        opt0=0; opt1=0; opt2=0; opt3=0; opt4=0; opt5=0; opt6=0; opt7=0
        source "$dir/descriptor.conf"
        python3 "$ROOT/tools/mkdesc.py" \
            --dev-name "$dev_name" --filemgr "$filemgr" --driver "$driver" \
            --opt "$opt0" "$opt1" "$opt2" "$opt3" \
            "$opt4" "$opt5" "$opt6" "$opt7" \
            "$OUT/$name.bin"
        python3 "$ROOT/tools/mkmodule.py" \
            --name "$name" --type descriptor --revision "${revision:-1}" \
            "${SIGN[@]}" \
            "$OUT/$name.bin" "$OUT/$name.mod"
        if ! wanted_desc "$driver"; then
            left_out+="$name "
            continue
        fi
        in_store=$((in_store + 1))
        cat "$OUT/$name.mod" >> "$STORE"
        size=$(stat -c%s "$OUT/$name.mod")
        pad=$(( (4 - size % 4) % 4 ))
        (( pad > 0 )) && head -c "$pad" /dev/zero >> "$STORE"
        continue
    fi

    srcs=("$dir"*.c)
    [[ ${#srcs[@]} -gt 0 ]] || continue

    # Source with no build.conf is not a module yet -- modules/detect held
    # only its engine, built and checked on a PC, before it had a program
    # around it. Linked anyway, it was an empty blob with no entry point.
    if [[ ! -f "$dir/build.conf" ]]; then
        echo "  ($name has no build.conf: not a module yet)"
        continue
    fi

    # Per-module build knobs live in the module's own build.conf.
    static_size=0
    stack_size=0        # 0 lets the loader decide (PROC_DEFAULT_STACK)
    revision=0
    mtype=program

    # The resource contract: what the program says it needs, over and above
    # the four numbers the fixed header holds. Everything here is optional,
    # and a module that declares none of it gets no manifest at all --
    # which is every module built before the manifest existed.
    #
    # Suffix a key with ! to make it mandatory, meaning a system that does
    # not understand it must refuse to run the module rather than quietly
    # ignoring the requirement.
    desc=""
    heap_max=""         # bytes of arena, given at fork; 0 means never
    heap_in=""          # internal (the default) or external
    class=""
    period_us=""
    deadline_us=""
    min_inter_us=""
    wcet_us=""
    on_deadline=""      # report (the default) or fault
    mem_max=""          # bytes it and everything it starts may hold
    ext_max=""          # the same, of external memory
    placement=""        # derived (the default), urgent or routine
    failsafes=""        # space separated PATH=VALUE, e.g. "/gpio/2=0"
    devices=""          # space separated, shared access
    exclusives=""       # space separated, wanted alone
    capabilities=""     # space separated
    publishes=""        # space separated cells it writes, e.g. "/pub0/CONTROL"
    watches=""          # space separated cells it reads
    mandatory=""        # space separated key names, e.g. "heap_max class"
    opt=""              # e.g. O2: arithmetic that is the point of the
                        # module, where -Os costs more than bytes. Not a
                        # manifest key; it changes only how it is built.
    cflags=""           # more compiler flags, e.g. an -march with an
                        # extension. Such a module carries those opcodes
                        # to every board and must ask RV9_ISA_* first.
    groups=""           # which systems carry it; see --store above
    files=""            # files it reads, by path: "/sd0/whistle.cact".
                        # tools/mksystem.py puts the card's on the card
    uses=""             # devices only some of its uses need: said, never
                        # refused (devices= is what it cannot run without)
    isa=""              # "fpu pie": what it cannot run without; refused
                        # at fork on a CPU that lacks it
    modules=""          # the modules it starts
    sources=""          # more .c files, relative to the module's directory:
                        # a vendored engine kept in a subdirectory of its own.

    [[ -f "$dir/build.conf" ]] && source "$dir/build.conf"

    MODFLAGS=("${CFLAGS[@]}")
    [[ -n "$opt" ]] && MODFLAGS+=("-$opt")
    [[ -n "$cflags" ]] && read -ra extra <<< "$cflags" && MODFLAGS+=("${extra[@]}")
    for f in $sources; do srcs+=("$dir$f"); done

    # From the module's own directory, so that its cflags and sources can
    # name its files relatively.
    (cd "$dir" && "$CC" "${MODFLAGS[@]}" "${LDFLAGS[@]}" -o "$OUT/$name.elf" "${srcs[@]}")
    "$OBJCOPY" -O binary "$OUT/$name.elf" "$OUT/$name.bin"

    # Prove position independence instead of assuming it.
    #
    # Link the same objects at a different base and compare the bytes. Code
    # that only uses PC-relative references is byte-identical wherever it is
    # linked; anything holding an absolute address differs. This catches the
    # constructs that quietly break the loader -- pointer tables built from
    # a switch over string literals being the one that actually bit us.
    (cd "$dir" && "$CC" "${MODFLAGS[@]}" "${LDFLAGS[@]}" "${PROBE_BASE[@]}" \
        -o "$OUT/$name.probe.elf" "${srcs[@]}")
    "$OBJCOPY" -O binary "$OUT/$name.probe.elf" "$OUT/$name.probe.bin"

    if ! cmp -s "$OUT/$name.bin" "$OUT/$name.probe.bin"; then
        echo "error: module '$name' is not position independent." >&2
        echo "  Linking it at a different address produced different code," >&2
        echo "  which means it holds an absolute address somewhere." >&2
        echo "  Usual causes: a table of pointers (often a switch over" >&2
        echo "  string literals), or a static array of function pointers." >&2
        echo "  Use if/else returning literals, or index into a char array." >&2
        echo "  Plain 'static const' data is fine -- a byte table, a string" >&2
        echo "  blob with integer offsets, inline char[N] in a struct. It is" >&2
        echo "  addresses in rodata that break this, not constants." >&2
        exit 1
    fi

    # Turn the build.conf keys into --tag arguments. A key named in
    # $mandatory gets the "!" suffix mkmodule.py reads as "refuse the
    # module rather than ignore this".
    tags=()
    add_tag() {
        local key="$1" value="$2"
        [[ -n "$value" ]] || return 0
        for m in $mandatory; do
            [[ "$m" == "$key" ]] && { tags+=(--tag "$key!=$value"); return 0; }
        done
        tags+=(--tag "$key=$value")
    }

    add_tag desc         "$desc"
    add_tag heap_max     "$heap_max"
    add_tag heap_in      "$heap_in"
    add_tag class        "$class"
    add_tag period_us    "$period_us"
    add_tag deadline_us  "$deadline_us"
    add_tag min_inter_us "$min_inter_us"
    add_tag wcet_us      "$wcet_us"
    add_tag on_deadline  "$on_deadline"
    add_tag mem_max      "$mem_max"
    add_tag ext_max      "$ext_max"
    add_tag placement    "$placement"
    for d in $devices;      do add_tag device     "$d"; done
    for d in $uses;         do add_tag uses       "$d"; done
    for m in $modules;      do add_tag module     "$m"; done
    for f in $files;        do add_tag file       "$f"; done
    add_tag isa          "$isa"
    for d in $exclusives;   do add_tag exclusive  "$d"; done
    for f in $failsafes;    do add_tag failsafe   "$f"; done
    for c in $capabilities; do add_tag capability "$c"; done
    for c in $publishes;    do add_tag publishes  "$c"; done
    for c in $watches;      do add_tag watches    "$c"; done

    python3 "$ROOT/tools/mkmodule.py" \
        --name "$name" \
        --type "$mtype" \
        --static-size "$static_size" \
        --stack-size "$stack_size" \
        --revision "$revision" \
        "${tags[@]}" "${SIGN[@]}" \
        "$OUT/$name.bin" "$OUT/$name.mod"

    # A module marked .nostore is built but kept out of the flash image --
    # it has to reach the board some other way, which is the point of it.
    if [[ -f "$dir/.nostore" ]]; then
        echo "  ($name kept out of the store)"
        continue
    fi

    if [[ -z "$groups" ]]; then
        echo "error: module '$name' has no groups= in its build.conf" >&2
        exit 1
    fi
    if ! wanted_mod "$name" "$groups"; then
        left_out+="$name "
        continue
    fi
    in_store=$((in_store + 1))
    cat "$OUT/$name.mod" >> "$STORE"

    # Modules are 4-byte aligned in the store.
    size=$(stat -c%s "$OUT/$name.mod")
    pad=$(( (4 - size % 4) % 4 ))
    (( pad > 0 )) && head -c "$pad" /dev/zero >> "$STORE"
done

echo "module store: $STORE ($(stat -c%s "$STORE") bytes, $in_store modules)"
[[ -n "$left_out" ]] && echo "  built, not in the store: $left_out"
true
