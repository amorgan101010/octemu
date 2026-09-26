#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Per-track dropped-trig sweep (bug-025 / bug-031, docs/PERF-NOTES.md).
#
#   tests/trigsweep.sh LABEL [CASES...]      default cases: q1..q8 q12
#   P=2 tests/trigsweep.sh ...               runs in parallel (default 2)
#   OCTEMU_QEMU=/path/qemu tests/trigsweep.sh ...   another binary
#   OCTA_SLICE_MATCH=1 / OCTA_LONE_CORE=1 tests/trigsweep.sh ...  stepping modes
#
# Each case solos one track of tests/fixtures/trigsweep (the user's TESTDROPOUT
# project: 8 FLEX tracks trigging every 16th, comb filter on T1 FX2, Neighbor
# on T2, spring reverb on T3 FX2), thins it to quarter notes and scores the
# sustained body of every beat with tests/trig8-body.py. q12 is T1 heard
# through its Neighbor on T2 (T1 alone is silent: its output IS T2's input).
#
# Expected on the shipped stepping: q12 drops [9, 17, ... 81] (bug-031),
# q1 silent (median body 0), everything else clean. With OCTA_LONE_CORE=1, q3
# and q4 drop too. Runs are ~2-6 min each; they run at the lowest CPU and IO
# priority where nice/ionice exist, so the machine stays usable.
#
# Exit status: number of cases with drops (0 = clean).
set -uo pipefail
cd "$(dirname "$0")/.."
LABEL=${1:?label}
shift
CASES=${*:-q1 q2 q3 q4 q5 q6 q7 q8 q12}
P=${P:-2}
W=out/trigsweep-walks
python3 tests/trigsweep-walks.py "$W"

# A plain string, word-split on use: macOS ships bash 3.2, where an empty
# array under set -u is an error.
LOW=
command -v nice >/dev/null && LOW="nice -n 19"
command -v ionice >/dev/null && LOW="$LOW ionice -c3"

run() {
    local c=$1 l="sweep-$LABEL-$1" b s
    FX=tests/fixtures/trigsweep WALK="$W/$c.jsonl" $LOW tests/trig8-repro.sh "$l" \
        > /dev/null 2>&1
    b=$(grep -ao '\[mark\] blk=[0-9]* [0-9]* play' "out/trig8-$l.log" 2>/dev/null \
        | grep -o 'blk=[0-9]*' | cut -d= -f2)
    if [ -z "$b" ]; then
        echo "$c: RUN FAILED (see out/trig8-$l.log)"
        return
    fi
    s=$(python3 tests/trig8-body.py "out/trig8-$l.wav" "$b" | head -1)
    echo "$c: $s $(grep -ao 'hatch=[0-9]*' "out/trig8-$l.log" | tail -1)"
}
export -f run
export LABEL W LOW
out=$(printf '%s\n' $CASES | xargs -P "$P" -I{} bash -c 'run {}' | sort -k1.2n)
echo "$out"
n=$(echo "$out" | grep -c 'drops at beats \[[0-9]' || true)
echo "$LABEL: $n case(s) with drops"
exit "$n"
