#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The gates: every check a build must pass before it ships, in one run, so
# "validated on macOS" and "validated on Linux" mean the same thing.
#
#   scripts/gates.sh [label]                       the current build
#   OCTEMU_QEMU=/path/qemu scripts/gates.sh label  another QEMU binary
#   RUNS=5 scripts/gates.sh                        repeat the flaky-prone ones
#   EMAC_REF=/path/old-qemu scripts/gates.sh       also bit-compare EMAC
#
# Needs: out/fx + out/fx2 (make fixtures), numpy (trig8 scoring), octdsp and
# octemu built. Prints one PASS/FAIL line per gate and a summary; exit status
# is the number of failed gates (0 = ship it). ~15-20 min at RUNS=3.
#
# Why each gate exists (see docs/PERF-NOTES.md):
#   test-dsp / -metro / -emu   the DSP core, the inter-core handoff, boot
#   test-emac, emac-diff        the ColdFire EMAC, against the manual and bit-
#                               exact against a reference build (qemu 0015)
#   test-emu-audio              a tapped STATIC trig: clean 440 Hz, no dropouts
#   trig8                       sequencer FLEX trigs: caught the lone-core DSP
#                               stepping race that 0017 made deterministic;
#                               every other gate passed with that bug present
#   keys-72                     key delivery while playing; must be re-tested
#                               whenever pacing changes (ot-board.c throttle)
#   save-reload                 PROJECT > SAVE and a fresh reload from the card
set -uo pipefail
cd "$(dirname "$0")/.."
LABEL=${1:-gates}
RUNS=${RUNS:-3}
QEMU=${OCTEMU_QEMU:-vendor/qemu/build/qemu-system-m68k}
export OCTEMU_QEMU=$QEMU
L=out/gates-$LABEL
rm -rf "$L" && mkdir -p "$L"
results=()
fails=0

gate() {    # gate NAME CMD... : run CMD, log it, record PASS/FAIL
    local name=$1; shift
    if "$@" > "$L/$name.log" 2>&1; then
        results+=("PASS  $name")
    else
        results+=("FAIL  $name   (log: $L/$name.log)")
        fails=$((fails + 1))
    fi
    echo "${results[${#results[@]}-1]}"
}

for need in out/fx/card.img out/fx2/card.img; do
    [ -f "$need" ] || { echo "needs $need (make fixtures)" >&2; exit 99; }
done
python3 -c 'import numpy' 2>/dev/null || { echo "needs numpy (brew install numpy / pacman -S python-numpy)" >&2; exit 99; }
echo "gates: $LABEL, qemu $QEMU, RUNS=$RUNS"

gate test-dsp       make -s test-dsp
gate test-dsp-metro make -s test-dsp-metro
gate test-emu       make -s test-emu
gate test-emac      sh -c "python3 tests/emac-conform.py --qemu '$QEMU' && python3 tests/emac-macload.py --qemu '$QEMU'"
if [ -n "${EMAC_REF:-}" ]; then
    gate emac-diff  python3 tests/emac-diff.py --ref "$EMAC_REF" --qemu "$QEMU" --cases 4000
fi

for i in $(seq 1 "$RUNS"); do
    gate "test-emu-audio-$i" sh -c 'make -s test-emu-audio | tee /dev/stderr | grep -q " 0 failing"'
done

for i in $(seq 1 "$RUNS"); do
    gate "trig8-$i" tests/trig8-repro.sh "$LABEL-$i"
done

keys() {    # every one of the 72 trig taps must land first time
    ./octemu --headless --read-only --cf-card out/fx2/card.img \
        --nvram out/fx2/nvram.bin --script tests/walks/keys-72.jsonl \
        --timeout 400 > "$L/keys-walk.log" 2>&1
    local lost trig timeouts
    trig=$(grep -a -c '"tap":"TRIG' <(grep -a 're-tap' "$L/keys-walk.log"))
    lost=$(grep -a -c 're-tap' "$L/keys-walk.log")
    timeouts=$(grep -a -c -E 'TIMEOUT|FAIL' "$L/keys-walk.log")
    echo "re-taps $lost (trig $trig), timeouts $timeouts, done $(grep -a -c keys-done "$L/keys-walk.log")"
    [ "$trig" -eq 0 ] && [ "$timeouts" -eq 0 ] && grep -a -q keys-done "$L/keys-walk.log"
}
gate keys-72 keys

gate save-reload tests/save-reload.sh "$LABEL"

echo
echo "== gates summary: $LABEL ($QEMU)"
printf '%s\n' "${results[@]}"
echo "== $((${#results[@]} - fails)) passed, $fails failed"
exit "$fails"
