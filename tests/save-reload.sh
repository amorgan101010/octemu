#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Save-and-reload regression test: the path behind the user's original freezes
# (a project save, and assigning a sample to a STATIC slot).
#
#   tests/save-reload.sh [label]
#   OCTEMU_QEMU=/path/qemu tests/save-reload.sh label   another QEMU binary
#
# Stage 1 walks tests/walks/fixture-trig.jsonl on a COPY of out/fx: FX1/FX2 to
# NONE, the 440 Hz sine into STATIC slot 1 on track 1, a trig on step 1, then
# PROJECT > SAVE. Asserts every mark was reached, sectors were written, no
# write INTRQ had to be forced and no ATA gate escape fired.
#
# Stage 2 boots the SAVED card fresh (a real reload from the card, not the
# in-memory project) and runs the audio test's walk (tests/walks/trig-one.jsonl)
# with --recording, scored by tests/audio-quality.py: three clean 440 Hz bursts
# prove the saved project came back with its STATIC sample and trig.
#
# out/fx and out/fx2 are never written. Needs out/fx (make fixtures).
# ~4 min on a 5600G. Exit status: 0 pass, 1 fail, 2 run failed.
set -uo pipefail
cd "$(dirname "$0")/.."
LABEL=${1:-save-reload}
[ -f out/fx/card.img ] || { echo "$LABEL: needs out/fx (make fixtures)" >&2; exit 2; }
W=$(mktemp -d out/save-reload.XXXX)
trap 'rm -rf "$W"' EXIT
cp out/fx/card.img out/fx/nvram.bin "$W/"

./octemu --headless --cf-card "$W/card.img" --nvram "$W/nvram.bin" \
    ${OCTEMU_ARGS:-} --script tests/walks/fixture-trig.jsonl --timeout 2400 \
    > "$W/save.log" 2>&1 || { echo "$LABEL: save walk failed"; tail -5 "$W/save.log"; exit 2; }

python3 - "$W/save.log" "$LABEL" <<'PY' || exit 1
import re, sys
t = open(sys.argv[1], errors='replace').read()
marks = re.findall(r'\[mark\] blk=\d+ \d+ ([\w-]+)', t)
want = ['fx1-none', 'fx2-none', 'assigned', 'trig-placed', 'saved', 'done']
g = lambda k: int((re.search(k + r'=(\d+)', t) or [0, -1])[1])
bad = []
if marks != want:
    bad.append(f"marks {marks} != {want}")
if g('sectors_w') <= 0:
    bad.append(f"sectors_w={g('sectors_w')}")
if 'INTRQ forced' in t:
    bad.append(f"{t.count('INTRQ forced')} forced write INTRQ")
if g('gate_caps') != 0:
    bad.append(f"gate_caps={g('gate_caps')}")
print(f"{sys.argv[2]}: save: sectors_w={g('sectors_w')} gate_caps={g('gate_caps')} "
      f"resyncs={g('resyncs')} -> {'FAIL: ' + '; '.join(bad) if bad else 'ok'}")
sys.exit(1 if bad else 0)
PY

./octemu --headless --cf-card "$W/card.img" --nvram "$W/nvram.bin" \
    ${OCTEMU_ARGS:-} --script tests/walks/trig-one.jsonl --recording "$W/trig.wav" \
    --timeout 1800 > "$W/reload.log" 2>&1 || { echo "$LABEL: reload walk failed"; tail -5 "$W/reload.log"; exit 2; }
echo -n "$LABEL: reload: "
python3 tests/audio-quality.py "$W/trig.wav" 440 --expect-bursts 3 | tail -1 | tee "$W/score"
grep -q ' 0 failing' "$W/score" && grep -q '^3 bursts' "$W/score"
