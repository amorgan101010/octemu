#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Interleaved wall-clock A/B of qemu-system-m68k builds, for hosts without
# `perf` (macOS). Each round runs every binary once, in turn, so they share
# the same thermal state and background load: on a fanless laptop the first
# minute after a rest runs ~10% faster than the rest, which swamps a small
# difference in any non-interleaved comparison.
#
#   scripts/ab-wall.sh ROUNDS BIN...      e.g. 3 out/a/qemu out/b/qemu
#
# Plays tests/walks/bench-play.jsonl (boot, PLAY) headless on a read-only copy
# of CARD (default out/fx2) for SECS wall seconds and reports blocks consumed
# as a fraction of real time (2756.25 blocks/s). BENCH_ARGS adds octemu flags:
# --unthrottled measures raw capacity instead of the paced rate. A run with no
# block count keeps its log as out/ab-failed-*.log.
set -euo pipefail
cd "$(dirname "$0")/.."

ROUNDS=${1:?usage: ab-wall.sh ROUNDS BIN...}; shift
CARD=${CARD:-out/fx2}
SECS=${SECS:-60}

for r in $(seq 1 "$ROUNDS"); do
    for bin in "$@"; do
        log=$(mktemp)
        OCTEMU_QEMU=$bin ./octemu --headless --read-only ${BENCH_ARGS:-} \
            --cf-card "$CARD/card.img" --nvram "$CARD/nvram.bin" \
            --script tests/walks/bench-play.jsonl --timeout "$SECS" >"$log" 2>&1 || true
        b=$(sed -n 's/^octemu: consumed \([0-9]*\) blocks/\1/p' "$log")
        if [ -z "$b" ]; then
            keep=out/ab-failed-$(date +%H%M%S).log
            mv "$log" "$keep"
            echo "round $r  $bin: FAILED, log in $keep"
            exit 1
        fi
        rm -f "$log"
        printf 'round %s  %-40s %7s blocks  %5.1f%% of real time\n' "$r" "$bin" "$b" \
            "$(echo "$b * 100 / ($SECS * 2756.25)" | bc -l)"
    done
done
