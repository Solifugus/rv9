#!/usr/bin/env bash
#
# Write the module store image into the 'modules' partition.
# Offset must match partitions.csv.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${1:-/dev/ttyACM0}"
OFFSET=0x190000

[[ -f "$ROOT/build/modules.bin" ]] || {
    echo "error: build/modules.bin missing. Run tools/build_modules.sh first." >&2
    exit 1
}

exec python -m esptool --port "$PORT" write-flash "$OFFSET" "$ROOT/build/modules.bin"
