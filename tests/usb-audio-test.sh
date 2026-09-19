#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# usb-audio-test.sh — boot the USB-audio image with the packet bench + the MAIN
# audio tap, wait for the UI, then run one tests/usb-host.py scenario.
#
#   tests/usb-audio-test.sh SCENARIO [ARGS...]
#
# SCENARIO is passed through to usb-host.py (audio-validate / audio-alt /
# audio-cadence [N] / audio-stream OUT [N]). Exits with the scenario's code.
# The card fixture (out/usb-audio-card, carrying /USBAUDIO.BIN) is staged on
# demand. A TRIG9-style walk can be supplied via WALK=tests/walks/usb-audio-trig.jsonl.
set -euo pipefail
cd "$(dirname "$0")/.."

SCEN=${1:?usage: usb-audio-test.sh SCENARIO [ARGS...]}
shift || true
IMG=out/usb-audio.bin
CARD=out/usb-audio-card
WALK=${WALK:-tests/walks/boot-hold.jsonl}
[ -f "$IMG" ] || { echo "build $IMG first (make out/usb-audio.bin)"; exit 1; }
# Restage whenever the payload is newer than the card. The trampoline's
# checksum gate REJECTS a card whose blob does not match this image (by
# design), so a stale fixture silently degrades the Octatrack to the stock
# usb-midi composite and every audio assertion fails for the wrong reason.
if [ ! -f "$CARD/card.img" ] || [ out/usb-audio-payload.bin -nt "$CARD/card.img" ]; then
    tests/usb-audio-card.sh "$CARD" >/dev/null
fi

SOCK=$(mktemp -u /tmp/octa-usba.XXXXXX)
LOG=$(mktemp /tmp/octa-usba-log.XXXXXX)
./octemu --headless --os "$IMG" --cf-card "$CARD/card.img" \
    --nvram "$CARD/nvram.bin" --read-only --usb-host "$SOCK" --hw-faithful \
    ${RECORD:+--recording "$RECORD"} \
    --script "$WALK" --timeout "${TIMEOUT:-200}" > "$LOG" 2>&1 &
EMU=$!
trap 'kill -9 $EMU 2>/dev/null || true; wait $EMU 2>/dev/null || true; pkill -9 -f "nvram=$CARD/nvram.bin" 2>/dev/null || true' EXIT

for i in $(seq 1 90); do
    grep -q '\[mark\].*ready' "$LOG" && break
    kill -0 $EMU 2>/dev/null || { echo "emu died:"; tail -8 "$LOG"; exit 1; }
    sleep 2
done
grep -q '\[mark\].*ready' "$LOG" || { echo "no ready in 180 s"; tail -8 "$LOG"; exit 1; }

timeout 120 python3 tests/usb-host.py "$SOCK" "$SCEN" "$@"
