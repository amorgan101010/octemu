#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# usb-audio-stream16-test.sh — the 16-channel stream gate: two proofs on one
# capture of the HIGH-SPEED stream, on the signature fixture (out/sig8).
#
#   1. IDENTITY (tests/usb-audio-sigcheck.py): every USB channel carries its
#      own tone. The fixture plays a distinct stereo tone on each track, so
#      channel c must read 200+50*c Hz — the track->channel map and the L/R
#      order are proven positively, not by elimination.
#   2. FIDELITY (tests/usb-audio-verify16.py): every channel pair equals its
#      track's post-FX readback sample-exact, against the emulator's readback
#      DUMP (OCTA_RB_DUMP: every eDMA write into the arena the producer
#      reads), with all eight tracks required loud and a swap positive control.
#
# The walk announces "ready", presses PLAY (the eight one-shots fire together
# and run out gap-free) and holds, while the host pulls ~25 s of the stream.
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=${IMG:-$(ls -t out/OCTATRACK_OS*_usb-audio_*.os 2>/dev/null | head -1)}
CARD=out/usb-audio-cardsig
OUT=out/usb-audio-p16
[ -n "$IMG" ] && [ -f "$IMG" ] || { echo "no usb-audio image — run: make fw-usb-audio" >&2; exit 1; }
[ -f out/sig8/card.img ] || { echo "missing out/sig8 — run: tests/build-sig-fixture.sh" >&2; exit 1; }
# Restage when the payload OR the fixture is newer than the staged card.
if [ ! -f "$CARD/card.img" ] || [ out/USBAUDIO.BIN -nt "$CARD/card.img" ] \
   || [ out/sig8/card.img -nt "$CARD/card.img" ]; then
    SRC=out/sig8 tests/usb-audio-card.sh "$CARD" >/dev/null
fi

rm -rf "$OUT"; mkdir -p "$OUT"
cp "$CARD/card.img" "$CARD/nvram.bin" "$OUT/"
SOCK=$OUT/usbh.sock
LOG="$OUT/walk.log"
OCTA_RB_DUMP="$OUT/rb.dump" ./octemu --headless --os "$IMG" \
    --cf-card "$OUT/card.img" --nvram "$OUT/nvram.bin" --usb-host "$SOCK" \
    --hw-faithful --script tests/walks/usb-audio-sig8.jsonl --timeout 400 \
    > "$LOG" 2>&1 &
EMU=$!
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

# ~25 s of guest audio: PLAY lands 8 s in, the tones run for the rest.
timeout 400 python3 tests/usb-host.py "$SOCK" audio-stream "$OUT/iso.pcm" \
    120000 frames=1100000 || { echo "capture failed"; exit 1; }
# Stop the machine FIRST (TERM, so stdio flushes the dump), then judge.
stop_emu
trap - EXIT
ls -la "$OUT/rb.dump" "$OUT/iso.pcm"
python3 tests/usb-audio-sigcheck.py --pcm "$OUT/iso.pcm"
python3 tests/usb-audio-verify16.py "$OUT/rb.dump" "$OUT/iso.pcm" --all-loud
