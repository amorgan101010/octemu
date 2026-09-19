#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# usb-midi-stress.sh — P2 loss-free TX gate.
#
# The demo's single-dTD TX dropped any message that arrived while the one
# transfer was busy (the 0xFA Start behind the 0xF8 clock, measured). P2
# replaces it with a coalescing accumulator + double-buffered flush; this
# test proves it: run the sequencer with clock+transport send on and drain
# EP2 IN continuously, then assert the Start arrived, a dense clock stream
# arrived, and the firmware's own drop counter (usbmidi_tx_drops) is 0.
#
# Then a second pass stops draining long enough to overflow the accumulator
# and asserts the overflow was COUNTED (drops > 0), never silent.
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=out/usb-midi.bin
SYM=out/usb-midi.sym
[ -f "$IMG" ] && [ -f "$SYM" ] || { echo "build $IMG first"; exit 1; }
DROPS=$(sed -n 's/^usbmidi_tx_drops=//p' "$SYM")
[ -n "$DROPS" ] || { echo "no usbmidi_tx_drops in $SYM"; exit 1; }

SOCK=$(mktemp -u /tmp/octa-usbstress.XXXXXX)
LOG=$(mktemp /tmp/octa-usbstress-log.XXXXXX)
GDB=3450
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

python3 tests/usb-host.py "$SOCK" enum >/dev/null
tests/rsp-io.py $GDB w:80000028=03 w:8000002a=01 >/dev/null

echo "=== pass 1: continuous drain across PLAY, expect zero drops ==="
CAP=$(timeout 26 python3 tests/usb-host.py "$SOCK" midi-recv 22 2>&1 || true)
FA=$(echo "$CAP" | grep -c '0ffa0000' || true)
F8=$(echo "$CAP" | grep -c '0ff80000' || true)
DROPS1=$(tests/rsp-io.py $GDB r:$DROPS:4 | awk '{print $2}')
echo "  Start(0xFA)=$FA  clock(0xF8)=$F8  drops=$DROPS1"
[ "$FA" -ge 1 ] || { echo "FAIL: no Start on EP2 IN"; exit 1; }
[ "$F8" -ge 50 ] || { echo "FAIL: clock stream too thin ($F8)"; exit 1; }
[ "$DROPS1" = "00000000" ] || { echo "FAIL: drops nonzero under normal load ($DROPS1)"; exit 1; }
echo "  pass 1 OK: loss-free sustained TX"

echo "=== pass 2: stop draining ~6 s, expect overflow COUNTED ==="
sleep 6
DROPS2=$(tests/rsp-io.py $GDB r:$DROPS:4 | awk '{print $2}')
echo "  drops after starvation=$DROPS2"
[ "$DROPS2" != "00000000" ] || { echo "FAIL: overflow not counted (silent loss)"; exit 1; }
echo "  pass 2 OK: overflow is counted, never silent"

echo "USB-MIDI TX stress PASSED (loss-free under load, overflow counted)"
