#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the audio-test fixtures, both stages, by driving the firmware's own UI.
#
#   tests/build-fixture.sh          -> out/fx    a card with a set + project +
#                                        a 1 s 440 Hz sine in the set's AUDIO
#                                        pool, and the NVRAM holding it open
#   tests/build-fixture.sh trig     -> out/fx2   that project with FX1 and FX2
#                                        set to NONE, the sine loaded into
#                                        STATIC slot 1 on track 1, and a trig on
#                                        step 1 — the state every audio
#                                        assertion starts from
#
# WHY IT TAKES WHAT IT TAKES. The Octatrack runs at real time, so each stage
# costs about what its walk asks the guest to live through: the project walk
# consumes 76,521 audio blocks = 28 s of guest time, the trig walk 331,008
# blocks = 120 s, and the rest is boot, the firmware's bank reload after project
# creation, and stage 2's second short boot to read the FX ids back. Measured
# wall time: stage 1 about 32 s, stage 2 about 2.5 min — under three minutes
# for both.
#
# Cards are built and filled with scripts/card.py (MBR + mtools): no mount, no
# root, same on Linux.
#
# Both stages are idempotent: they skip the work if their outputs exist, so
# test runs boot into the interesting state in seconds and you pay this once.
# Delete the directory to rebuild.
set -euo pipefail
cd "$(dirname "$0")/.."

# ---- stage 2: out/fx2, the trig fixture, walked out of out/fx --------------
if [ "${1:-}" = trig ]; then
    FX2=out/fx2
    [ -f "$FX2/card.img" ] && [ -f "$FX2/nvram.bin" ] && { echo "ok: $FX2 (cached)"; exit 0; }
    [ -f out/fx/card.img ] || { echo "trig fixture: needs out/fx first" >&2; exit 1; }

    mkdir -p "$FX2"
    cp out/fx/card.img out/fx/nvram.bin "$FX2/"
    ./octemu --headless --cf-card "$FX2/card.img" \
        --nvram "$FX2/nvram.bin" --script tests/walks/fixture-trig.jsonl \
        --timeout 2400 2> "$FX2/build.log" \
        || { echo "trig fixture: walk failed, see $FX2/build.log" >&2; exit 1; }

    # Every gesture in the walk is gated on the screen or a lamp, so a walk that
    # ran to the end already proves a lot — but the two gates that MAKE this
    # fixture are worth asserting by name, because a walk that lost either one
    # still "finishes" and would leave out/fx with extra steps:
    #   saw lamp    — the trig lamp lit, so step 1 really carries a trig
    #   gone: C0NT  — the save's CONTINUE? dialog appeared and closed, so the
    #                 project was written to the card (OCR renders I as 1, hence
    #                 the truncated needle)
    for needle in "saw lamp" "gone: C0NT"; do
        LC_ALL=C grep -qa "$needle" "$FX2/build.log" || {
            echo "trig fixture: '$needle' never appeared — see $FX2/build.log" >&2
            exit 1; }
    done

    # ☠ The 40 UP taps that drive FX1 and FX2 to NONE are the only gestures in
    # the walk with no screen gate — a lost or extra tap changes the selection
    # and nothing on screen says so. So read the answer out of the guest.
    # published_fx_ids (re/coldfire.syms, 0x80000ec4) is 16 bytes indexed
    # [sel*8 + track], sel 0 = FX1, sel 1 = FX2, id 0 = NONE. A fresh project
    # publishes 04 (FLTR) for FX1 and 08 (DELAY) for FX2 on EVERY track, so
    # track 1 reading 00 in both halves is exactly the edit this stage makes:
    #   out/fx    04040404040404040808080808080808
    #   out/fx2   00040404040404040008080808080808
    # The card is mounted --read-only and the battery is a copy, so this boot
    # cannot touch the fixture it is checking.
    CHK=$FX2/fxcheck
    rm -rf "$CHK"; mkdir -p "$CHK"
    cp "$FX2/nvram.bin" "$CHK/nvram.bin"
    PORT=$((20000 + $$ % 10000))          # per-build, so two builds never clash
    ./octemu --headless --read-only --cf-card "$FX2/card.img" \
        --nvram "$CHK/nvram.bin" --gdb "$PORT" \
        --script tests/walks/boot-hold.jsonl --timeout 300 > "$CHK/boot.log" 2>&1 &
    EMU=$!
    # ☠ Two traps here. The frontend spawns QEMU as a child, so killing the
    # frontend alone orphans it — match the child on this check's own nvram
    # path too. And every step needs || true: `wait` on a process you just
    # killed reports 137, which under set -e aborts the build (and this trap)
    # before the temp directory is gone.
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
        echo "trig fixture: the finished fixture would not boot — see $CHK/boot.log" >&2
        cat "$CHK/boot.log" >&2; exit 1; }
    IDS=$(python3 tests/rsp-io.py "$PORT" r:80000ec4:16 | awk '{print $2}') || IDS=
    cleanup_chk; trap - EXIT
    [ ${#IDS} = 32 ] || { echo "trig fixture: bad published_fx_ids read '$IDS'" >&2; exit 1; }
    [ "${IDS:0:2}" = 00 ] && [ "${IDS:16:2}" = 00 ] || {
        echo "trig fixture: track 1 is FX1=${IDS:0:2} FX2=${IDS:16:2}, want 00/00 —" \
             "the UP taps in tests/walks/fixture-trig.jsonl did not land" \
             "(published_fx_ids=$IDS)" >&2
        exit 1; }

    echo "ok: $FX2/card.img + $FX2/nvram.bin (track 1 FX1/FX2 = NONE)"
    exit 0
fi
if [ -n "${1:-}" ]; then echo "usage: $0 [trig]" >&2; exit 2; fi

# ---- stage 1: out/fx, the project fixture ---------------------------------
FX=out/fx
CARD=$FX/card.img
NVRAM=$FX/nvram.bin
[ -f "$CARD" ] && [ -f "$NVRAM" ] && { echo "ok: $FX (cached)"; exit 0; }

mkdir -p "$FX"
scripts/mkcard.sh "$CARD" 64 >/dev/null
dd if=/dev/zero of="$NVRAM" bs=1024 count=1024 2>/dev/null

python3 - "$FX/sine440.wav" <<'EOF'
import math, struct, sys, wave
w = wave.open(sys.argv[1], "wb")
w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100)
w.writeframes(b"".join(struct.pack("<h", int(16000 * math.sin(2*math.pi*440*i/44100)))
                       for i in range(1*44100)))
w.close()
EOF

./octemu --headless --cf-card "$CARD" --nvram "$NVRAM" \
    --script tests/walks/fixture-project.jsonl --timeout 1200 \
    || { echo "fixture: project-creation walk failed" >&2; exit 1; }

# The set the walk just made owns the AUDIO pool; ask the card where it is
# rather than guessing the set name, which carries today's date.
AUDIO=$(python3 scripts/card.py finddir "$CARD" AUDIO) \
    || { echo "fixture: no AUDIO dir on card" >&2; exit 1; }
python3 scripts/card.py copy "$CARD" "$FX/sine440.wav" "$AUDIO/SINE440.WAV"

echo "ok: $CARD + $NVRAM"
