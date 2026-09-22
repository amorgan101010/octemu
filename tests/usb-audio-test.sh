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
# demand. A TRIG9-style walk can be supplied via WALK=tests/walks/foo.jsonl.
set -euo pipefail
cd "$(dirname "$0")/.."

SCEN=${1:?usage: usb-audio-test.sh SCENARIO [ARGS...]}
shift || true
IMG=${IMG:-$(ls -t out/OCTATRACK_OS*_usb-audio_*.os 2>/dev/null | head -1)}
CARD=out/usb-audio-card
WALK=${WALK:-tests/walks/boot-hold.jsonl}
RECORD=${RECORD:-}
TIMEOUT=${TIMEOUT:-200}
[ -n "$IMG" ] && [ -f "$IMG" ] || { echo "no usb-audio image — run: make fw-usb-audio" >&2; exit 1; }
# ☠ Restage whenever the payload is newer than the card. The trampoline's
# checksum gate REJECTS a card whose blob does not match this image (by
# design), so a stale fixture silently degrades the machine to the stock
# usb-midi composite and every audio assertion fails for the wrong reason.
# ☠ Serialize the restage under a lock: parallel gates all sharing $CARD
# otherwise raced to rewrite it and one caught a half-written image ("card
# image shorter than a sector"). The lock makes the check-and-restage atomic;
# a private RUN copy (below) is what each emulator actually boots.
LOCK="$CARD.lock.d"                       # mkdir is atomic on POSIX; macOS has no flock
# A lock older than ten minutes is the leftover of a run killed between mkdir
# and rmdir (kill -9 skips the trap); without this every later run waits the
# full five minutes for a holder that no longer exists.
if [ -d "$LOCK" ] && [ -n "$(find "$LOCK" -maxdepth 0 -mmin +10 2>/dev/null)" ]; then
    rmdir "$LOCK" 2>/dev/null || true
fi
for _ in $(seq 1 600); do mkdir "$LOCK" 2>/dev/null && break; sleep 0.5; done
trap 'rmdir "$LOCK" 2>/dev/null || true' EXIT
if [ ! -f "$CARD/card.img" ] || [ out/USBAUDIO.BIN -nt "$CARD/card.img" ]; then
    tests/usb-audio-card.sh "$CARD" >/dev/null
fi
rmdir "$LOCK" 2>/dev/null || true

# ☠ A PRIVATE copy of the fixture per run. Gates run in parallel, and the
# exit trap below kills by nvram path: with a shared fixture the first gate
# to finish killed every other gate's emulator ("bench closed the socket").
RUN=$(mktemp -d /tmp/octa-usba-run.XXXXXX)
cp "$CARD/card.img" "$CARD/nvram.bin" "$RUN/"
SOCK=$RUN/usbh.sock
LOG=$RUN/emu.log
./octemu --headless --os "$IMG" --cf-card "$RUN/card.img" \
    --nvram "$RUN/nvram.bin" --read-only --usb-host "$SOCK" --hw-faithful \
    ${RECORD:+--recording "$RECORD"} \
    --script "$WALK" --timeout "${TIMEOUT:-200}" > "$LOG" 2>&1 &
EMU=$!
trap 'kill -9 $EMU 2>/dev/null || true; wait $EMU 2>/dev/null || true; pkill -9 -f "nvram=$RUN/nvram.bin" 2>/dev/null || true; rm -rf "$RUN"' EXIT

for i in $(seq 1 90); do
    grep -q '\[mark\].*ready' "$LOG" && break
    kill -0 $EMU 2>/dev/null || { echo "emu died:"; tail -8 "$LOG"; exit 1; }
    sleep 2
done
grep -q '\[mark\].*ready' "$LOG" || { echo "no ready in 180 s"; tail -8 "$LOG"; exit 1; }

timeout 120 python3 tests/usb-host.py "$SOCK" "$SCEN" "$@"
