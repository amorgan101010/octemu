#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The PGO training run (scripts/build-opt.sh pgo): drive the instrumented
# emulator through what a user actually does, headless. Every walk is gated on
# guest time, so the instrumented build's slowness changes nothing but wall time.
#
#   1. boot a blank card, create a set and a project, save, PLAY (UI, card
#      writes, the bank reload)
#   2. the trig fixture: a STATIC sample streamed from the card on a trig
#   3. steady playback of that project, the state people spend their time in
set -uo pipefail
cd "$(dirname "$0")/.."
[ -f out/fx2/card.img ] || { echo "needs out/fx2 — run 'make fixtures' first" >&2; exit 1; }
W=$(mktemp -d out/pgo-train.XXXX)
trap 'rm -rf "$W"' EXIT

echo "   1/3 project creation walk"
scripts/mkcard.sh "$W/card1.img" 64 >/dev/null
dd if=/dev/zero of="$W/nvram1.bin" bs=1024 count=1024 2>/dev/null
./octemu --headless --cf-card "$W/card1.img" --nvram "$W/nvram1.bin" \
    --script tests/walks/fixture-project.jsonl --timeout 1800 > "$W/1.log" 2>&1 \
    || echo "     (walk 1 failed; profile still counts)"

echo "   2/3 trig walk (STATIC streaming)"
cp out/fx2/card.img "$W/card2.img"; cp out/fx2/nvram.bin "$W/nvram2.bin"
./octemu --headless --cf-card "$W/card2.img" --nvram "$W/nvram2.bin" \
    --script tests/walks/trig-one.jsonl --timeout 1800 > "$W/2.log" 2>&1 \
    || echo "     (walk 2 failed; profile still counts)"

echo "   3/3 steady playback, 40 emulated seconds"
cp out/fx2/card.img "$W/card3.img"; cp out/fx2/nvram.bin "$W/nvram3.bin"
printf '%s\n' '{"wait_text":"PTCH","timeout_ms":300000}' '{"wait_guest_ms":4000}' \
    '{"tap":"PLAY"}' '{"wait_blocks":110250}' '{"tap":"STOP"}' > "$W/play.jsonl"
./octemu --headless --cf-card "$W/card3.img" --nvram "$W/nvram3.bin" \
    --script "$W/play.jsonl" --timeout 1800 > "$W/3.log" 2>&1 \
    || echo "     (walk 3 failed; profile still counts)"
