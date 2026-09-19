#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# usb-midi-enum.sh — USB-MIDI descriptor + enumeration conformance (P3).
#
# Boots the patched image with the packet bench and runs the enum-conform
# scenario: an independent structural validation of the composite config
# against USB 2.0 + USB-MIDI 1.0 (interface/jack/endpoint topology, AC
# header linkage, class-specific endpoint descriptors), plus the standard
# requests a real host issues — GET_STATUS, GET_CONFIGURATION,
# GET/SET_INTERFACE, and CLEAR_FEATURE(ENDPOINT_HALT) on EP2.
#
# Exits 0 only if the descriptors validate and every request answers.
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=out/usb-midi.bin
[ -f "$IMG" ] || { echo "build $IMG first (make out/usb-midi.bin)"; exit 1; }
SOCK=$(mktemp -u /tmp/octa-usbenum.XXXXXX)
LOG=$(mktemp /tmp/octa-usbenum-log.XXXXXX)

./octemu --headless --os "$IMG" --cf-card out/fx2/card.img \
    --nvram out/fx2/nvram.bin --read-only --usb-host "$SOCK" \
    --script tests/walks/boot-hold.jsonl --timeout 200 > "$LOG" 2>&1 &
EMU=$!
trap 'kill -9 $EMU 2>/dev/null || true; wait $EMU 2>/dev/null || true; rm -f "$LOG"' EXIT

for i in $(seq 1 60); do
    grep -q '\[mark\].*ready' "$LOG" && break
    kill -0 $EMU 2>/dev/null || { echo "emu died"; tail -5 "$LOG"; exit 1; }
    sleep 2
done
grep -q '\[mark\].*ready' "$LOG" || { echo "no ready in 120 s"; exit 1; }

timeout 90 python3 tests/usb-host.py "$SOCK" enum-conform
