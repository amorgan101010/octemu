#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# usb-midi-imgcheck.sh — P6 / D9: nothing corrupts the patch's code region.
#
# The patch lives in the in-image free zone at 0x400d24f0, so using that zone
# at all is only safe if nothing else in the firmware writes it. Equivalent
# to a zero-hit write-watch but far cheaper: run a heavy session (enumerate,
# MIDI both directions, sequencer clock, DISK MODE) and confirm the code +
# constant span (ZONE_ADDR .. usbmidi_up, everything before the writable
# vars) is byte-identical to the image afterwards. The writable vars past
# usbmidi_up are written only by the shims and are excluded.
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=${IMG:-$(ls -t out/OCTATRACK_OS*_usb-midi_*.os 2>/dev/null | head -1)}
[ -n "$IMG" ] || { echo "no usb-midi image — run: make fw-usb-midi" >&2; exit 1; }
SYM=out/usb-midi.sym
[ -f "$IMG" ] && [ -f "$SYM" ] || { echo "build $IMG first"; exit 1; }
ZONE=0x400d24f0
UP=$(sed -n 's/^usbmidi_up=//p' "$SYM")           # first writable var
LEN=$(( UP - ZONE ))
OFF=$(( ZONE - 0x40000400 ))                      # file offset of the span

SOCK=$(mktemp -u /tmp/octa-usbimg.XXXXXX)
LOG=$(mktemp /tmp/octa-usbimg-log.XXXXXX)
GDB=3454
./octemu --headless --os "$IMG" --cf-card out/fx2/card.img \
    --nvram out/fx2/nvram.bin --read-only --usb-host "$SOCK" --gdb $GDB \
    --script tests/walks/usb-midi-stress.jsonl --timeout 200 > "$LOG" 2>&1 &
EMU=$!
trap 'kill -9 $EMU 2>/dev/null || true; wait $EMU 2>/dev/null || true; rm -f "$LOG"' EXIT

for i in $(seq 1 60); do
    grep -q '\[mark\].*ready' "$LOG" && break
    kill -0 $EMU 2>/dev/null || { echo "emu died"; tail -5 "$LOG"; exit 1; }
    sleep 2
done
grep -q '\[mark\].*ready' "$LOG" || { echo "no ready"; exit 1; }

# heavy activity: enumerate, clock+transport out, notes in, sequencer running
python3 tests/usb-host.py "$SOCK" enum >/dev/null
tests/rsp-io.py $GDB w:80000028=03 w:8000002a=01 w:8000004b=01 w:80000012=01 >/dev/null
python3 tests/usb-host.py "$SOCK" midi-send 903c64 >/dev/null || true
timeout 14 python3 tests/usb-host.py "$SOCK" midi-recv 10 >/dev/null 2>&1 || true

# read the code+const span live and compare to the image bytes
LIVE=$(tests/rsp-io.py $GDB r:$(printf '%x' $ZONE):$LEN | awk '{print $2}')
FILE=$(python3 - "$IMG" "$OFF" "$LEN" <<'EOF'
import sys
img=open(sys.argv[1],'rb').read(); off=int(sys.argv[2]); n=int(sys.argv[3])
sys.stdout.write(img[off:off+n].hex())
EOF
)
if [ "$LIVE" = "$FILE" ]; then
    echo "imgcheck OK: patch code region ($LEN B at $ZONE) unchanged after a heavy run"
else
    echo "FAIL: patch code region changed at runtime — something wrote the free zone"
    exit 1
fi
