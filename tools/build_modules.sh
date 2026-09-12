#!/usr/bin/env bash
#
# Build every RV-9 module and concatenate them into a module store image.
#
# Modules are compiled outside the IDF build: they are freestanding blobs
# with no libc and no linkage to the kernel, which is the entire point.
#
# Requires the IDF toolchain on PATH (. $HOME/esp/esp-idf/export.sh).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/modules"
STORE="$ROOT/build/modules.bin"

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

mkdir -p "$OUT"
: > "$STORE"

# Same script, different base, for the position-independence check below.
PROBE_LD="$OUT/module_probe.ld"
sed 's/^    \. = 0;$/    . = 0x4000;/' "$ROOT/modules/module.ld" > "$PROBE_LD"

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
            "$OUT/$name.bin" "$OUT/$name.mod"
        cat "$OUT/$name.mod" >> "$STORE"
        size=$(stat -c%s "$OUT/$name.mod")
        pad=$(( (4 - size % 4) % 4 ))
        (( pad > 0 )) && head -c "$pad" /dev/zero >> "$STORE"
        continue
    fi

    srcs=("$dir"*.c)
    [[ ${#srcs[@]} -gt 0 ]] || continue

    # Per-module build knobs live in the module's own build.conf.
    static_size=0
    stack_size=0        # 0 lets the loader decide (PROC_DEFAULT_STACK)
    revision=0
    mtype=program
    [[ -f "$dir/build.conf" ]] && source "$dir/build.conf"

    "$CC" "${CFLAGS[@]}" "${LDFLAGS[@]}" -o "$OUT/$name.elf" "${srcs[@]}"
    "$OBJCOPY" -O binary "$OUT/$name.elf" "$OUT/$name.bin"

    # Prove position independence instead of assuming it.
    #
    # Link the same objects at a different base and compare the bytes. Code
    # that only uses PC-relative references is byte-identical wherever it is
    # linked; anything holding an absolute address differs. This catches the
    # constructs that quietly break the loader -- pointer tables built from
    # a switch over string literals being the one that actually bit us.
    "$CC" "${CFLAGS[@]}" -nostdlib -nostartfiles -T"$PROBE_LD" \
        -Wl,--gc-sections -o "$OUT/$name.probe.elf" "${srcs[@]}"
    "$OBJCOPY" -O binary "$OUT/$name.probe.elf" "$OUT/$name.probe.bin"

    if ! cmp -s "$OUT/$name.bin" "$OUT/$name.probe.bin"; then
        echo "error: module '$name' is not position independent." >&2
        echo "  Linking it at a different address produced different code," >&2
        echo "  which means it holds an absolute address somewhere." >&2
        echo "  Usual causes: a table of pointers (often a switch over" >&2
        echo "  string literals), or a static array of function pointers." >&2
        echo "  Use if/else returning literals, or index into a char array." >&2
        exit 1
    fi

    python3 "$ROOT/tools/mkmodule.py" \
        --name "$name" \
        --type "$mtype" \
        --static-size "$static_size" \
        --stack-size "$stack_size" \
        --revision "$revision" \
        "$OUT/$name.bin" "$OUT/$name.mod"

    # A module marked .nostore is built but kept out of the flash image --
    # it has to reach the board some other way, which is the point of it.
    if [[ -f "$dir/.nostore" ]]; then
        echo "  ($name kept out of the store)"
        continue
    fi

    cat "$OUT/$name.mod" >> "$STORE"

    # Modules are 4-byte aligned in the store.
    size=$(stat -c%s "$OUT/$name.mod")
    pad=$(( (4 - size % 4) % 4 ))
    (( pad > 0 )) && head -c "$pad" /dev/zero >> "$STORE"
done

echo "module store: $STORE ($(stat -c%s "$STORE") bytes)"
