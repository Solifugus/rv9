#!/usr/bin/env bash
#
# Write the module store image into the 'modules' partition.
#
# The offset is read out of whichever partition table this build actually
# used, not written down here. It used to be a constant with a comment saying
# it must match partitions.csv -- which was true and sufficient for exactly as
# long as there was one board. The P4 puts its store at 0x500000, and a tool
# that writes 0x190000 on a 32 MB chip does not fail, it writes the store into
# the middle of the application.

#
# "Whichever build actually used" is the root sdkconfig unless told
# otherwise, and a side build (idf.py -B build-c3 -D SDKCONFIG=...) is
# otherwise: name its sdkconfig as the second argument.
#
#     tools/flash_modules.sh /dev/ttyACM0 build-c3/sdkconfig
#
# Without it, the C3 got the P4's table read for it.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${1:-/dev/ttyACM0}"
CFG="${2:-$ROOT/sdkconfig}"
[[ "$CFG" = /* ]] || CFG="$ROOT/$CFG"

CSV="$ROOT/partitions.csv"
if [[ -f "$CFG" ]]; then
    named=$(sed -n 's/^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="\(.*\)"$/\1/p' \
            "$CFG")
    [[ -n "$named" && -f "$ROOT/$named" ]] && CSV="$ROOT/$named"
fi

OFFSET=$(awk -F',' '/^modules,/ { gsub(/ /, "", $4); print $4 }' "$CSV")
[[ -n "$OFFSET" ]] || {
    echo "error: no 'modules' partition in $(basename "$CSV")" >&2
    exit 1
}
echo "store -> $OFFSET, per $(basename "$CSV")" 

[[ -f "$ROOT/build/modules.bin" ]] || {
    echo "error: build/modules.bin missing. Run tools/build_modules.sh first." >&2
    exit 1
}

exec python -m esptool --port "$PORT" write-flash "$OFFSET" "$ROOT/build/modules.bin"
