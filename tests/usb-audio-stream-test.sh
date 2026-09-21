#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# usb-audio-stream-test.sh — the P4 gate: the iso stream equals --recording.
#
# Boots the USB-audio image with the TRIG9 walk + --recording + the MAIN audio
# tap + the packet bench, captures the iso IN stream over EP3 while the walk
# fires TRIG9, and proves the decoded capture is a sample-exact substring of
# the recording (with a built-in 1-LSB positive control).
set -euo pipefail
cd "$(dirname "$0")/.."

# the firmware images carry a build id in their names, so find them
OSIMG=${OSIMG:-$(ls -t out/OCTATRACK_OS*_usb-audio_*.os 2>/dev/null | head -1)}
[ -n "$OSIMG" ] || { echo "no usb-audio image — run: make fw-usb-audio" >&2; exit 1; }

IMG=$OSIMG
CARD=out/usb-audio-card
OUT=out/usb-audio-p4
[ -f "$IMG" ] || { echo "build $IMG first"; exit 1; }
# ☠ Restage whenever the payload is newer than the card. The trampoline's
# checksum gate REJECTS a card whose blob does not match this image (by
# design), so a stale fixture silently degrades the machine to the stock
# usb-midi composite and every audio assertion fails for the wrong reason.
if [ ! -f "$CARD/card.img" ] || [ out/USBAUDIO.BIN -nt "$CARD/card.img" ]; then
    tests/usb-audio-card.sh "$CARD" >/dev/null
fi

rm -rf "$OUT"; mkdir -p "$OUT"
# fresh card copy so the walk's writes never touch the fixture
cp "$CARD/card.img" "$CARD/nvram.bin" "$OUT/"
SOCK=$(mktemp -u /tmp/octa-usba.XXXXXX)
LOG="$OUT/walk.log"
./octemu --headless --os "$IMG" --cf-card "$OUT/card.img" \
    --nvram "$OUT/nvram.bin" --usb-host "$SOCK" --hw-faithful \
    --recording "$OUT/ref.wav" --script tests/walks/usb-audio-trig.jsonl \
    --timeout 300 > "$LOG" 2>&1 &
EMU=$!
# ☠ SIGTERM, not SIGKILL: the frontend patches the .wav length into the header
# on the way out (src/main.c on_term -> audio_recording_flush), and SIGKILL
# cannot be caught — a -9'd run leaves a recording that reads as zero frames.
stop_emu() {
    kill -TERM $EMU 2>/dev/null || true
    for _ in $(seq 1 40); do kill -0 $EMU 2>/dev/null || break; sleep 0.5; done
    kill -9 $EMU 2>/dev/null || true
    wait $EMU 2>/dev/null || true
    pkill -9 -f "nvram=$OUT/nvram.bin" 2>/dev/null || true
}
trap stop_emu EXIT

for i in $(seq 1 90); do
    grep -q '\[mark\].*ready' "$LOG" && break
    kill -0 $EMU 2>/dev/null || { echo "emu died:"; tail -8 "$LOG"; exit 1; }
    sleep 2
done
grep -q '\[mark\].*ready' "$LOG" || { echo "no ready"; tail -8 "$LOG"; exit 1; }

# Capture the iso stream until a burst is in hand.
timeout 150 python3 tests/usb-host.py "$SOCK" audio-stream "$OUT/iso.pcm" \
    || { echo "capture failed (no burst caught)"; exit 1; }

# Stop the machine FIRST so the recording's header is finalized, then compare.
# Comparing against a still-open .wav reads a zero-length header, which looks
# exactly like "the machine recorded nothing".
stop_emu
trap - EXIT
python3 tests/usb-audio-verify.py "$OUT/ref.wav" "$OUT/iso.pcm"
