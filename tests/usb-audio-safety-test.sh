#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# usb-audio-safety-test.sh — the recovery gate.
#
# The ONLY recovery this feature is allowed to need is "delete /USBAUDIO.BIN
# from the CF card". That makes the card file's integrity a SAFETY property,
# not a convenience: any card the machine cannot use in full must leave it
# running as the stock usb-midi composite, booting normally.
#
# Four cards, each booted in --hw-faithful mode (poisoned scratch, host-driven
# frame signal — see qemu/ot-board.c):
#
#   absent     no /USBAUDIO.BIN at all          -> stock composite, boots
#   truncated  the blob cut short               -> stock composite, boots
#   corrupt    one byte flipped mid-blob        -> stock composite, boots
#   good       the real blob                    -> UAC1 composite, boots
#
# ☠ "UAC1 composite" is 5 interfaces since the descriptors were split into two
# audio functions (AudioControl+MIDIStreaming, AudioControl+AudioStreaming) —
# macOS rejects a single AudioControl that collects both, with
# AUAErrorCode.noAudioFunctions.
#
# "stock composite" is asserted structurally: 3 interfaces and NO isochronous
# endpoint. "UAC1 composite" is 4 interfaces WITH the iso EP. A machine that
# fails to reach PTCH fails the gate outright — that is the exception screen.
set -uo pipefail
cd "$(dirname "$0")/.."

IMG=${IMG:-$(ls -t out/OCTATRACK_OS*_usb-audio_*.os 2>/dev/null | head -1)}
[ -n "$IMG" ] || { echo "no usb-audio image — run: make fw-usb-audio" >&2; exit 1; }
PAY=out/USBAUDIO.BIN
OUT=out/usb-audio-safety
[ -f "$PAY" ] || { echo "missing $PAY"; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT"
# the three damaged variants
head -c 4096 "$PAY" > "$OUT/truncated.bin"
python3 - "$PAY" "$OUT/corrupt.bin" <<'PY'
import sys
b=bytearray(open(sys.argv[1],'rb').read())
b[len(b)//2] ^= 0xff          # one byte, mid-blob: magic still matches
open(sys.argv[2],'wb').write(bytes(b))
PY

# ☠ The frontend spawns QEMU as a child; killing the frontend alone ORPHANS
# it, and orphans from previous runs compete for the CPU with the run you are
# timing. Kill the child too, matched on this case's own nvram path so the
# pattern can never reach anything else.
stop_emu() {
    local emu=$1 dir=$2
    kill -9 "$emu" 2>/dev/null
    wait "$emu" 2>/dev/null
    pkill -9 -f "nvram=$dir/nvram.bin" 2>/dev/null
    return 0
}

fail=0
run_case() {
    local name=$1 payload=$2 want_ifaces=$3 want_iso=$4
    local dir="$OUT/$name"
    mkdir -p "$dir"
    if [ "$payload" = "none" ]; then
        cp out/fx2/card.img out/fx2/nvram.bin "$dir/"
    else
        tests/usb-audio-card.sh "$dir" "$payload" >/dev/null || {
            echo "  $name: FAIL (could not stage card)"; fail=1; return; }
    fi
    local sock; sock=$(mktemp -u /tmp/octa-safe.XXXXXX)
    ./octemu --headless --os "$IMG" --cf-card "$dir/card.img" \
        --nvram "$dir/nvram.bin" --usb-host "$sock" --hw-faithful \
        --script tests/walks/boot-hold.jsonl --timeout 240 > "$dir/walk.log" 2>&1 &
    local emu=$!
    local i
    for i in $(seq 1 90); do
        grep -q '\[mark\].*ready' "$dir/walk.log" && break
        kill -0 $emu 2>/dev/null || break
        sleep 2
    done
    if ! grep -q '\[mark\].*ready' "$dir/walk.log"; then
        echo "  $name: FAIL — machine never reached the UI (this is the exception screen)"
        stop_emu $emu "$dir"; fail=1; return
    fi
    timeout 60 python3 tests/usb-host.py "$sock" enum > "$dir/enum.log" 2>&1
    stop_emu $emu "$dir"
    local ifaces iso
    ifaces=$(grep -o 'bNumInterfaces [0-9]*' "$dir/enum.log" | awk '{print $2}' | head -1)
    iso=$(grep -c 'type 1 ' "$dir/enum.log")
    if [ "$ifaces" = "$want_ifaces" ] && [ "$iso" = "$want_iso" ]; then
        echo "  $name: ok — booted, bNumInterfaces=$ifaces, iso EPs=$iso"
    else
        echo "  $name: FAIL — bNumInterfaces=$ifaces (want $want_ifaces), iso EPs=$iso (want $want_iso)"
        fail=1
    fi
}

echo "USB-audio recovery gate (--hw-faithful):"
run_case absent    none                 3 0
run_case truncated "$OUT/truncated.bin" 3 0
run_case corrupt   "$OUT/corrupt.bin"   3 0
run_case good      "$PAY"               5 1

if [ $fail -eq 0 ]; then
    echo "ALL USB-AUDIO SAFETY GATES PASSED"
else
    echo "USB-AUDIO SAFETY GATES FAILED"
fi
exit $fail
