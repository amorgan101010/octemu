#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# usb-midi-coexist.sh — P4 composite coexistence gate.
#
# The likeliest reason USB-MIDI never shipped is composite-device behaviour:
# does mass storage and MIDI on one device step on each other? This test
# runs the sequencer's MIDI clock out EP2 IN while hammering MSC INQUIRY on
# EP1, interleaved on the bus, and asserts BOTH stay correct — every MSC CSW
# valid, MIDI clock still flowing, and the TX drop counter at zero. Then a
# second pass drives a full DISK MODE enter/exit cycle and confirms MIDI
# survives it (EP2 keeps delivering across the mount handoff).
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=out/usb-midi.bin
SYM=out/usb-midi.sym
[ -f "$IMG" ] && [ -f "$SYM" ] || { echo "build $IMG first"; exit 1; }
DROPS=$(sed -n 's/^usbmidi_tx_drops=//p' "$SYM")
ACTIVE=0x460e76a0                        # usb_diskmode_active

SOCK=$(mktemp -u /tmp/octa-usbcoex.XXXXXX)
LOG=$(mktemp /tmp/octa-usbcoex-log.XXXXXX)
GDB=3452
# usb-diskmode walk enters/exits DISK MODE and holds; drive it so pass 2 has
# a real mount cycle to observe while MIDI runs.
./octemu --headless --os "$IMG" --cf-card out/fx2/card.img \
    --nvram out/fx2/nvram.bin --read-only --usb-host "$SOCK" --gdb $GDB \
    --script tests/walks/usb-diskmode.jsonl --timeout 200 > "$LOG" 2>&1 &
EMU=$!
trap 'kill -9 $EMU 2>/dev/null || true; wait $EMU 2>/dev/null || true; rm -f "$LOG"' EXIT

for i in $(seq 1 60); do
    grep -q 'saw PTCH' "$LOG" && break
    kill -0 $EMU 2>/dev/null || { echo "emu died"; tail -5 "$LOG"; exit 1; }
    sleep 2
done
grep -q 'saw PTCH' "$LOG" || { echo "no PTCH"; exit 1; }

python3 tests/usb-host.py "$SOCK" enum >/dev/null
tests/rsp-io.py $GDB w:80000028=03 w:8000002a=01 >/dev/null

echo "=== pass 1: MSC (EP1) + MIDI (EP2) interleaved ==="
python3 tests/usb-host.py "$SOCK" coexist
D=$(tests/rsp-io.py $GDB r:$DROPS:4 | awk '{print $2}')
echo "  tx drops during coexist = $D"
[ "$D" = "00000000" ] || { echo "FAIL: MIDI drops during MSC coexistence"; exit 1; }

echo "=== pass 2: MIDI survives a DISK MODE cycle ==="
# The walk enters DISK MODE ~a few seconds in; watch usb_diskmode_active go
# 0 -> 1 -> 0 while MIDI keeps arriving on EP2 IN.
seen_active=0; seen_clear_after=0; midi_during=0
for i in $(seq 1 40); do
    a=$(tests/rsp-io.py $GDB r:$ACTIVE:4 2>/dev/null | awk '{print $2}')
    pk=$(python3 tests/usb-host.py "$SOCK" midi-recv 1 2>/dev/null | grep -c '0ff80000' || true)
    [ "$pk" -gt 0 ] && midi_during=$((midi_during+pk))
    if [ "$a" = "00000001" ]; then seen_active=1; fi
    if [ "$seen_active" = "1" ] && [ "$a" = "00000000" ]; then seen_clear_after=1; break; fi
    sleep 2
done
echo "  diskmode entered=$seen_active  exited=$seen_clear_after  midi clocks seen during=$midi_during"
[ "$seen_active" = "1" ] || { echo "FAIL: DISK MODE never entered"; exit 1; }
[ "$midi_during" -gt 0 ] || { echo "FAIL: no MIDI during the DISK MODE window"; exit 1; }
echo "USB-MIDI coexistence PASSED (MSC+MIDI interleaved; MIDI survives DISK MODE)"
