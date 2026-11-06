#!/usr/bin/env bash
#
# Say something through the P4's speaker, from the PC.
#
#     tools/say.sh "Come and look at the camera, please."
#
# A stand-in until RV-9 can speak for itself: the speech is made here, by
# espeak-ng, turned into what `play -1` takes (16 kHz, one channel, 16-bit),
# served over HTTP, and the board is told on its serial console to fetch it
# to the card and play it. The card, not /r0: the RAM disk is 16 KB, and
# four seconds of speech is 130.
#
# Environment: PORT (/dev/ttyACM0), HOST (this PC's address on the board's
# network, 192.168.1.12), HTTP_PORT (8077). The web server is left running
# for the next time, and its PID is in build/say/http.pid.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-/dev/ttyACM0}"
HOST="${HOST:-192.168.1.12}"
HTTP_PORT="${HTTP_PORT:-8077}"
D="$ROOT/build/say"
mkdir -p "$D/www"

[[ $# -gt 0 ]] || { echo "usage: tools/say.sh <text>" >&2; exit 1; }

espeak-ng -v en-us -s 150 --stdout "$*" |
    ffmpeg -loglevel error -y -i - -ar 16000 -ac 1 -f s16le "$D/www/say.raw"

if ! { [[ -f "$D/http.pid" ]] && kill -0 "$(cat "$D/http.pid")" 2>/dev/null; }; then
    # exec, so that the PID recorded is the server's and not a shell's.
    (cd "$D/www" && exec python3 -m http.server "$HTTP_PORT" --bind "$HOST" \
        >/dev/null 2>&1) &
    echo $! > "$D/http.pid"
    sleep 1
fi

# Long enough for the speech to finish before the next line is typed.
SECS=$(( $(stat -c %s "$D/www/say.raw") / 32000 + 3 ))

[[ -n "${IDF_PATH:-}" ]] || . ~/esp/esp-idf/export.sh >/dev/null 2>&1

python - "$PORT" "$SECS" \
    "fetch $HOST:$HTTP_PORT /say.raw > /sd0/say.raw" \
    "play -1 -v 0 /sd0/say.raw" <<'EOF'
import serial, sys, time
port, secs, cmds = sys.argv[1], float(sys.argv[2]), sys.argv[3:]
s = serial.Serial(port, 115200, timeout=0.2)
time.sleep(0.3); s.read(65536); s.write(b'\r\n'); time.sleep(0.3); s.read(65536)
for c in cmds:
    s.write(c.encode() + b'\r\n')
    buf, t = b'', time.time()
    while time.time() - t < secs:
        buf += s.read(4096)
        if buf.rstrip().endswith(b'rv9>') and time.time() - t > 0.5:
            break
    for l in buf.decode('utf8', 'replace').splitlines():
        if 'cannot' in l or 'error' in l or 'usage' in l:
            print(l, file=sys.stderr)
EOF
