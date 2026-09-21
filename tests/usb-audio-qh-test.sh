#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# usb-audio-qh-test.sh — the detector's positive control.
#
# The emulator's "UNINITIALIZED dQH" report is what stands between a
# partially-initialized endpoint queue head and a bricked unit: on hardware
# the USB controller is a real bus master that acts on a stale token and the
# buffer pointers beside it, and writes transfer status back through them —
# an arbitrary memory write. That exact defect crashed a real Octatrack twice,
# corrupting image code in the eDMA audio state machine.
#
# A detector that has never been seen to fire proves nothing, so this gate
# runs it BOTH ways:
#
#   broken  payload built with --omit-qh-clear  -> the report MUST appear
#   fixed   the shipping payload                -> it MUST NOT, and EP3 must
#                                                  still deliver a packet
#
# Both halves run --hw-faithful; without the poison over the queue-head array
# the broken build looks fine, which is precisely how this escaped to hardware.
set -uo pipefail
cd "$(dirname "$0")/.."

# the firmware images carry a build id in their names, so find them
OSIMG=${OSIMG:-$(ls -t out/OCTATRACK_OS*_usb-audio_*.os 2>/dev/null | head -1)}
MIDIIMG=${MIDIIMG:-$(ls -t out/OCTATRACK_OS*_usb-midi_*.os 2>/dev/null | head -1)}
[ -n "$OSIMG" ] && [ -n "$MIDIIMG" ] || {
    echo "missing firmware images — run: make fw-usb-audio" >&2; exit 1; }
OUT=out/usb-audio-qh
rm -rf "$OUT"; mkdir -p "$OUT"
fail=0

run_case() {          # name  payload  want_report(0|1)
    local name=$1 payload=$2 want=$3
    local card="$OUT/$name"
    tests/usb-audio-card.sh "$card" "$payload" >/dev/null || {
        echo "  $name: FAIL (could not stage card)"; fail=1; return; }
    local sock; sock=$(mktemp -u /tmp/octa-qh.XXXXXX)
    local log="$card/emu.log"
    ./octemu --headless --os $OSIMG \
        --cf-card "$card/card.img" --nvram "$card/nvram.bin" --read-only \
        --usb-host "$sock" --hw-faithful --script tests/walks/boot-hold.jsonl \
        --timeout 240 > "$log" 2>&1 &
    local emu=$!
    local i
    for i in $(seq 1 90); do
        grep -q '\[mark\].*ready' "$log" && break
        kill -0 $emu 2>/dev/null || break
        sleep 2
    done
    if ! grep -q '\[mark\].*ready' "$log"; then
        echo "  $name: FAIL — machine never reached the UI"
        kill -9 $emu 2>/dev/null; pkill -9 -f "nvram=$card/nvram.bin" 2>/dev/null
        fail=1; return
    fi
    timeout 90 python3 tests/usb-host.py "$sock" audio-alt > "$card/host.log" 2>&1
    local alt=$?
    kill -TERM $emu 2>/dev/null; sleep 1
    kill -9 $emu 2>/dev/null; wait $emu 2>/dev/null
    pkill -9 -f "nvram=$card/nvram.bin" 2>/dev/null

    # the poison must have landed, or the whole run is meaningless
    if grep -q "POISON DID NOT LAND" "$log"; then
        echo "  $name: FAIL — scratch poison did not land; result is meaningless"
        fail=1; return
    fi
    local n; n=$(grep -c "UNINITIALIZED dQH" "$log")
    if [ "$want" = 1 ]; then
        if [ "$n" -gt 0 ]; then
            echo "  $name: ok — detector fired ($n), as it must on the real defect"
        else
            echo "  $name: FAIL — detector SILENT on a known-bad queue head; it proves nothing"
            fail=1
        fi
    else
        if [ "$n" -eq 0 ] && [ "$alt" -eq 0 ]; then
            echo "  $name: ok — detector silent and EP3 still streams"
        else
            echo "  $name: FAIL — detector fired ($n) or EP3 broke (alt exit $alt)"
            fail=1
        fi
    fi
}

echo "USB-audio queue-head detector control (--hw-faithful):"
python3 custom/usb-audio.py --omit-qh-clear --in "$MIDIIMG" \
    --out "$OUT/broken-image.bin" --payload "$OUT/broken-payload.bin" \
    >/dev/null 2>&1 || { echo "  could not build the broken payload"; exit 1; }
run_case broken "$OUT/broken-payload.bin" 1
run_case fixed  out/USBAUDIO.BIN 0

if [ $fail -eq 0 ]; then echo "QUEUE-HEAD DETECTOR CONTROL PASSED"
else echo "QUEUE-HEAD DETECTOR CONTROL FAILED"; fi
exit $fail
