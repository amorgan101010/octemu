#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# usb-bench-test.sh — the Phase-2 USB gate, self-contained.
#
# Boots the out/fx2 fixture with the packet bench listening, waits for the
# Octatrack to reach the main page, then runs tests/usb-host.py's msc
# scenario: reset, full enumeration (device/config descriptors,
# SET_ADDRESS, SET_CONFIGURATION), SCSI INQUIRY and TEST UNIT READY over
# bulk EP1. Exits 0 only if enumeration succeeds and INQUIRY answers with
# CSW status 0. Cleans up the emulator regardless.
#
#   tests/usb-bench-test.sh [scenario]   (default: msc)
set -euo pipefail
cd "$(dirname "$0")/.."

SCEN=${1:-msc}
SOCK=$(mktemp -u /tmp/octa-usbh.XXXXXX)
LOG=$(mktemp /tmp/octa-usbh-log.XXXXXX)

./octemu --headless --cf-card out/fx2/card.img \
    --nvram out/fx2/nvram.bin --read-only --usb-host "$SOCK" \
    --script tests/walks/boot-hold.jsonl --timeout 300 > "$LOG" 2>&1 &
EMU=$!
trap 'kill $EMU 2>/dev/null || true; wait $EMU 2>/dev/null || true; rm -f "$LOG"' EXIT

for i in $(seq 1 60); do
    grep -q '\[mark\].*ready' "$LOG" && break
    kill -0 $EMU 2>/dev/null || { echo "emulator died:"; tail -5 "$LOG"; exit 1; }
    sleep 2
done
grep -q '\[mark\].*ready' "$LOG" || { echo "no ready mark in 120 s"; tail -5 "$LOG"; exit 1; }

timeout 90 python3 tests/usb-host.py "$SOCK" "$SCEN"
