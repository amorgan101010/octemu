#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# usb-midi-conform.sh — USB-MIDI RX message-type conformance, at both USB bus
# speeds (full speed and high speed; tests/usb-host.py's `speed fs|hs`).
#
# Boots the patched image with the packet bench, enumerates the composite
# device, and sends every MIDI message class in on EP2 OUT, asserting the
# RX decoder (custom/coldfire/usb-midi.s) enqueues the right byte count into the DIN FIFO
# (read over RSP) — proving the CIN table and the decoder agree with the
# USB-MIDI 1.0 spec for channel voice, system common, real-time, and
# multi-packet SysEx. The whole run is done twice, once with the bus
# enumerated at full speed and once at high speed.
#
# Exits 0 only if BOTH bus speeds pass every class.
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=out/usb-midi.bin
[ -f "$IMG" ] || { echo "build $IMG first (make out/usb-midi.bin)"; exit 1; }

run_speed() {
    local speed=$1 gdb=$2
    local sock; sock=$(mktemp -u /tmp/octa-usbconf.XXXXXX)
    local log; log=$(mktemp /tmp/octa-usbconf-log.XXXXXX)
    ./octemu --headless --os "$IMG" --cf-card out/fx2/card.img \
        --nvram out/fx2/nvram.bin --read-only --usb-host "$sock" --gdb "$gdb" \
        --script tests/walks/boot-hold.jsonl --timeout 200 > "$log" 2>&1 &
    local emu=$!
    local rc=0
    for i in $(seq 1 60); do
        grep -q '\[mark\].*ready' "$log" && break
        kill -0 $emu 2>/dev/null || { echo "emu died ($speed)"; tail -5 "$log"; }
        sleep 2
    done
    grep -q '\[mark\].*ready' "$log" || { echo "no ready ($speed)"; rc=1; }
    if [ $rc -eq 0 ]; then
        timeout 90 python3 tests/usb-host.py "$sock" midi-conform "$gdb" "$speed" || rc=1
    fi
    kill -9 $emu 2>/dev/null || true; wait $emu 2>/dev/null || true
    rm -f "$log"
    return $rc
}

echo "=== USB full speed ==="
run_speed fs 3440
echo "=== USB high speed ==="
run_speed hs 3441
echo "USB-MIDI RX conformance PASSED at both USB bus speeds (full and high)"
