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
    -fno-jump-tables            # jump tables would need absolute addresses
    -ffunction-sections
    -fdata-sections
    -Wall -Wextra -Werror
    -I"$ROOT/components/rv9_module/include"
)
LDFLAGS=(-nostdlib -nostartfiles -T"$ROOT/modules/module.ld" -Wl,--gc-sections)

mkdir -p "$OUT"
: > "$STORE"

shopt -s nullglob
for dir in "$ROOT"/modules/*/; do
    name="$(basename "$dir")"

    # A device descriptor is a data module: no code, just the binding of a
    # device name to a file manager and a driver. Adding a device to the
    # system is adding one of these -- no kernel rebuild.
    if [[ -f "$dir/descriptor.conf" ]]; then
        opt0=0; opt1=0; opt2=0; opt3=0
        source "$dir/descriptor.conf"
        python3 "$ROOT/tools/mkdesc.py" \
            --dev-name "$dev_name" --filemgr "$filemgr" --driver "$driver" \
            --opt "$opt0" "$opt1" "$opt2" "$opt3" \
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
    revision=0
    mtype=program
    [[ -f "$dir/build.conf" ]] && source "$dir/build.conf"

    "$CC" "${CFLAGS[@]}" "${LDFLAGS[@]}" -o "$OUT/$name.elf" "${srcs[@]}"
    "$OBJCOPY" -O binary "$OUT/$name.elf" "$OUT/$name.bin"

    python3 "$ROOT/tools/mkmodule.py" \
        --name "$name" \
        --type "$mtype" \
        --static-size "$static_size" \
        --revision "$revision" \
        "$OUT/$name.bin" "$OUT/$name.mod"

    cat "$OUT/$name.mod" >> "$STORE"

    # Modules are 4-byte aligned in the store.
    size=$(stat -c%s "$OUT/$name.mod")
    pad=$(( (4 - size % 4) % 4 ))
    (( pad > 0 )) && head -c "$pad" /dev/zero >> "$STORE"
done

echo "module store: $STORE ($(stat -c%s "$STORE") bytes)"
