#!/usr/bin/env bash
#
# RV-9 layering check.
#
# The KAL exists so that phase 7 can replace the kernel without touching
# anything above it. That only holds if nothing above the seam reaches past
# it. This script fails the build the moment something does.
#
# If you are here because the build broke: do not add an exemption. Add the
# primitive you need to the KAL instead. That is the whole point.

set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# Everything below the seam. These are allowed to know about the host kernel.
BELOW_SEAM=(
    "components/rv9_kal"
)

# Directories holding RV-9 code that must stay portable.
ABOVE_SEAM=(
    "main"
    "modules"
    "components"
)

is_below_seam() {
    local path="$1"
    for dir in "${BELOW_SEAM[@]}"; do
        [[ "$path" == "$ROOT/$dir/"* ]] && return 0
    done
    return 1
}

violations=0

for area in "${ABOVE_SEAM[@]}"; do
    [[ -d "$ROOT/$area" ]] || continue

    while IFS= read -r -d '' file; do
        is_below_seam "$file" && continue

        # Only #include lines count. Mentioning FreeRTOS in a comment is fine
        # and often necessary when explaining why a wrapper exists.
        if matches=$(grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]freertos/' "$file"); then
            while IFS= read -r m; do
                echo "  ${file#"$ROOT"/}:${m}"
                violations=$((violations + 1))
            done <<< "$matches"
        fi
    done < <(find "$ROOT/$area" -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' \) -print0)
done

if (( violations > 0 )); then
    echo
    echo "RV-9 layering check FAILED: $violations FreeRTOS include(s) above the KAL."
    echo
    echo "Code above the seam must go through rv9/kal.h. If the KAL is missing"
    echo "something you need, extend the KAL -- do not reach around it."
    exit 1
fi

echo "RV-9 layering check passed: no FreeRTOS includes above the KAL."
