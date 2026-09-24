#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Regression test for "every 8th FLEX trig is silent" (see docs/PERF-NOTES.md).
#
#   tests/trig8-repro.sh [label]            current build
#   OCTEMU_QEMU=/path/qemu tests/trig8-repro.sh label    another QEMU binary
#   OCTEMU_ARGS="--interleave 1024" tests/trig8-repro.sh label
#
# The fixture (tests/fixtures/trig8/) is a real user project: a set with a
# single-cycle slice on a FLEX machine on track 4, trigged every quarter note
# at 120 BPM, plus the NVRAM that opens it. This builds a fresh 256 MiB card
# from it, boots headless, waits for LOADING FILES to clear, mutes every track
# but T4 (FUNC + T1/T2/T3/T5..T8), presses PLAY and records 40 s of output
# with --recording (sample-exact, no resampling, no audio device opened).
#
# tests/trig8-body.py then scores each beat by the RMS of its sustained body
# (75-175 ms after the beat) and flags beats below half the median. A good
# build prints "drops at beats []"; the bug prints [9, 17, 25, ...].
#
# Exit status: 0 if no beat dropped, 1 if any did, 2 if the run failed.
set -uo pipefail
cd "$(dirname "$0")/.."
LABEL=${1:-trig8}
FX=tests/fixtures/trig8
W=$(mktemp -d out/trig8.XXXX)
trap 'rm -rf "$W"' EXIT

scripts/mkcard.sh "$W/card.img" 256 >/dev/null
mkdir -p "$W/set" && tar xzf "$FX/set.tgz" -C "$W/set"
python3 scripts/card.py copytree "$W/card.img" "$W/set/Set 260924" "/Set 260924" >/dev/null
gzip -dc "$FX/nvram.bin.gz" > "$W/nvram.bin"

./octemu --headless --read-only --cf-card "$W/card.img" --nvram "$W/nvram.bin" \
    ${OCTEMU_ARGS:-} --script "$FX/walk.jsonl" --recording "$W/out.wav" \
    --timeout 580 > "$W/log" 2>&1 || { echo "$LABEL: run failed"; tail -5 "$W/log"; exit 2; }
blk=$(grep -ao '\[mark\] blk=[0-9]* [0-9]* play' "$W/log" | grep -o 'blk=[0-9]*' | cut -d= -f2)
[ -n "$blk" ] || { echo "$LABEL: no play mark"; exit 2; }
mkdir -p out && cp "$W/out.wav" "out/trig8-$LABEL.wav" && cp "$W/log" "out/trig8-$LABEL.log"
echo -n "$LABEL: "
python3 tests/trig8-body.py "$W/out.wav" "$blk" | tee "$W/score"
grep -q 'drops at beats \[\]' "$W/score"
