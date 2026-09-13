#!/usr/bin/env bash
#
# Run the renderer on this machine, where the output can be looked at.
#
# The panel is the one part of RV-9 nothing here can check: a picture can be
# geometrically perfect and still be wrong in a way only an eye catches --
# the circles that came out lumpy had the right radius and the right area.
# So the rasteriser and the SVG parser are built natively and asserted
# against, and `render` writes a PPM you can open.
#
# raster.c has no ESP dependencies at all. drv_svgwin.c needs about forty
# lines of stubs, in stub/, which is a small price for being able to see
# what the thing draws without flashing a board.
#
#   tools/hosttest/run.sh            assertions only
#   tools/hosttest/run.sh pic.svg    render one, to out.ppm

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../../components/rv9_io/src"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

cp "$SRC"/raster.c "$SRC"/raster.h "$SRC"/drv_svgwin.c "$SRC"/font.c "$SRC"/font.h "$OUT/"

CC=${CC:-gcc}
FLAGS="-O1 -Wall -Wextra -I$HERE/stub -I$OUT"

if [[ $# -ge 1 ]]; then
    $CC $FLAGS -o "$OUT/render" "$HERE/render.c" "$OUT/raster.c" "$OUT/font.c"
    ( cd "$OUT" && ./render "$HERE/$1" )
    cp "$OUT/out.ppm" "$HERE/out.ppm"
    echo "wrote $HERE/out.ppm"
    exit 0
fi

$CC $FLAGS -o "$OUT/raster_test" "$HERE/raster_test.c" "$OUT/raster.c" "$OUT/font.c"
$CC $FLAGS -o "$OUT/path_test"   "$HERE/path_test.c"   "$OUT/raster.c" "$OUT/font.c"

"$OUT/raster_test"
"$OUT/path_test" | grep -v '^I rv9-svgwin'

# The target profile is generated from the sources. A stale one tells a
# compiler something that is no longer true, which is worse than none.
python3 "$HERE/../mkprofile.py" --check
