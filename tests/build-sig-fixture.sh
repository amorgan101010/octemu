#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the USB-audio channel-SIGNATURE fixture: a set/project where tracks
# play DISTINCT continuous stereo tones on one-shot trigs, so a 16-channel
# capture identifies every channel by frequency and proves L/R order. Track N:
# STATIC, sample SIGN.WAV (left 200+50*(2N-1) Hz, right 200+50*(2N) Hz),
# one-shot trig on step 1, timestretch off (STATIC default).
#
# All eight tracks are treated the same way: load SIGn into the track's own
# (empty) slot, set FX1 and FX2 to NONE, set LOOP and TSTR to OFF on the SRC
# SETUP page (measured on hardware: STATIC tracks come up with both AUTO, and
# a fixture that trusts the default is not the fixture the unit plays), then
# in grid recording place a trig on step 1 (gated on its lamp) and FUNC+TRIG
# it into a one-shot.
#
# ☠ FX NONE on every track, verified by reading published_fx_ids out of the
# finished fixture (as tests/build-fixture.sh does for track 1). With the
# defaults a fresh project publishes (FLTR, DELAY) the 16-channel stream came
# back as the readback arena times exactly 63/64 on all eight tracks: the
# readback DUMP is only a sample-exact oracle for a dry track, and the point
# of this fixture is pure tones anyway.
#
# ☠ Built from out/fx, NOT out/fx2. fx2 already has SINE440 (1 s) in slot 1
# with a trig on track 1; loading SIG1 OVER that slot left the slot's old
# length in the restored machine state, and track 1 then played exactly one
# second of SIG1 and stopped — measured in the readback (peak for 1 s, then
# silence) on both a manual trigger and PLAY, while tracks 2-8, loaded into
# fresh slots, ran the full minute. Starting from fx (a set, a project, the
# sine in the pool, nothing assigned) makes every track take the same
# fresh-slot path. The walk is self-verifying: each load is gated on the
# sample name and each trig on its step lamp, so a dropped tap under host load
# FAILS the build at that step instead of silently producing a wrong fixture —
# re-run if it does.
#
# Output: out/sig8/{card.img,nvram.bin}. On a 256 MB card because eight 60 s
# stereo samples (~85 MB) do not fit the 64 MB fixture card. The card is
# assembled with scripts/card.py (MBR + mtools): no mount, no root, same on
# Linux.
#
# ☠ One-shots fire once on PLAY from stopped, then disarm, so the long sample
# plays out gap-free with no retrigger — the point of the fixture (a retrigger
# is a discontinuity that is not a USB defect). Re-arm by STOP then PLAY.
#
# Idempotent: delete out/sig8 to rebuild. ~12 min (the build walk assigns eight
# samples, clears sixteen FX and places eight trigs, every gesture but the FX
# list taps gated on the screen, and those are read back at the end).
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=out/sig8
[ -f "$OUT/card.img" ] && [ -f "$OUT/nvram.bin" ] && { echo "ok: $OUT (cached)"; exit 0; }
[ -f out/fx/card.img ] || { echo "need out/fx first (tests/build-fixture.sh)"; exit 1; }
SECS=${SECS:-60}
tests/gen-sig-samples.py out/sig "$SECS" >/dev/null

# base: a 256 MB FAT32 card carrying out/fx's Set plus the eight SIG samples
BASE=out/sigbase2
rm -rf "$BASE"; mkdir -p "$BASE"
scripts/mkcard.sh "$BASE/card.img" 256 >/dev/null
cp out/fx/nvram.bin "$BASE/nvram.bin"
setname=$(python3 scripts/card.py ls out/fx/card.img / | sed -n 's|^/\(Set[^/]*\)/$|\1|p' | head -1)
[ -n "$setname" ] || { echo "sig fixture: no Set directory on out/fx/card.img"; exit 1; }
python3 scripts/card.py extract out/fx/card.img "/$setname" "$BASE/tree"
cp out/sig/SIG*.WAV "$BASE/tree/$setname/AUDIO/"
python3 scripts/card.py copytree "$BASE/card.img" "$BASE/tree/$setname" "/$setname"
# Prove the pool landed: a card missing SIG8 would build a fixture whose last
# track loads nothing, and the walk's sample-name gate would then fail late.
python3 scripts/card.py ls "$BASE/card.img" | grep -qx "/$setname/AUDIO/SIG8.WAV" \
    || { echo "sig fixture: SIG samples did not land on $BASE/card.img"; exit 1; }

# run the build walk (assign + one-shot trigs + save)
mkdir -p "$OUT"; cp "$BASE/card.img" "$BASE/nvram.bin" "$OUT/"
./octemu --headless --cf-card "$OUT/card.img" --nvram "$OUT/nvram.bin" \
    --script tests/walks/sig8-build.jsonl --timeout 1500 2> "$OUT/build.log" \
    || { echo "sig fixture: build walk failed, see $OUT/build.log"; exit 1; }
grep -qa "\[mark\] .* saved" "$OUT/build.log" \
    || { echo "sig fixture: walk did not reach 'saved'"; exit 1; }

# ☠ The 320 UP taps that drive every track's FX1 and FX2 to NONE are the only
# gestures with no screen gate — a lost or extra tap changes the selection and
# nothing on screen says so. Read the answer out of the guest, as
# tests/build-fixture.sh does: published_fx_ids (0x80000ec4) is 16 bytes
# [sel*8 + track], sel 0 = FX1, sel 1 = FX2, id 0 = NONE; all 32 hex digits
# must be zero. The card is mounted --read-only and the battery is a copy.
CHK=$OUT/fxcheck
rm -rf "$CHK"; mkdir -p "$CHK"
cp "$OUT/nvram.bin" "$CHK/nvram.bin"
PORT=$((20000 + $$ % 10000))
./octemu --headless --read-only --cf-card "$OUT/card.img" \
    --nvram "$CHK/nvram.bin" --gdb "$PORT" \
    --script tests/walks/boot-hold.jsonl --timeout 300 > "$CHK/boot.log" 2>&1 &
EMU=$!
cleanup_chk() {
    kill -9 "$EMU" 2>/dev/null || true
    wait "$EMU" 2>/dev/null || true
    pkill -9 -f "$CHK/nvram.bin" 2>/dev/null || true
    rm -rf "$CHK"
    return 0
}
trap cleanup_chk EXIT
for _ in $(seq 1 90); do
    LC_ALL=C grep -qa '\[mark\].*ready' "$CHK/boot.log" && break
    kill -0 "$EMU" 2>/dev/null || break
    sleep 2
done
LC_ALL=C grep -qa '\[mark\].*ready' "$CHK/boot.log" || {
    echo "sig fixture: the finished fixture would not boot — see $CHK/boot.log" >&2
    cat "$CHK/boot.log" >&2; exit 1; }
IDS=$(python3 tests/rsp-io.py "$PORT" r:80000ec4:16 | awk '{print $2}') || IDS=
cleanup_chk; trap - EXIT
[ ${#IDS} = 32 ] || { echo "sig fixture: bad published_fx_ids read '$IDS'" >&2; exit 1; }
[ "$IDS" = 00000000000000000000000000000000 ] || {
    echo "sig fixture: FX not NONE on every track (published_fx_ids=$IDS) —" \
         "an UP tap in tests/walks/sig8-build.jsonl did not land" >&2
    exit 1; }
echo "ok: $OUT/card.img + $OUT/nvram.bin (boot + PLAY = eight distinct tones; FX NONE, LOOP/TSTR OFF)"
