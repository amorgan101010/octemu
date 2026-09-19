#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the patched QEMU. Our own board model (src/board/) is COPIED into
# hw/m68k/ rather than patched in, so the patch series cannot rot against it.
# Idempotent.
set -euo pipefail
cd "$(dirname "$0")/.."

BASE=$(head -1 patches/qemu/BASE_COMMIT.txt | awk '{print $1}')
DST=vendor/qemu
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}
DSPSRC=$PWD/vendor/dsp56300
DSPBLD=$DSPSRC/build

for f in src/board/ot-board.c src/board/ot-ata.c src/board/ot-dsp-port.c \
         src/board/ot-panel-uart.c src/board/ot-dsp-shim.cc src/board/ot-dsp56k.cc; do
    [ -f "$f" ] || { echo "missing $f — the device sources are not written yet" >&2; exit 1; }
done
[ -f "$DSPBLD/source/dsp56kEmu/libdsp56kEmu.a" ] || { echo "run 'make setup' first" >&2; exit 1; }

if [ ! -d "$DST/.git" ]; then
    git clone https://gitlab.com/qemu-project/qemu.git "$DST"
fi
git -C "$DST" cat-file -e "$BASE^{commit}" 2>/dev/null || git -C "$DST" fetch --all --tags
git -C "$DST" checkout -q --detach "$BASE"
git -C "$DST" reset -q --hard "$BASE"

cp src/board/ot-*.c src/board/ot-*.cc src/board/ot-*.h "$DST/hw/m68k/"

# A patch that ADDS a file leaves it behind: `git reset --hard` restores tracked
# files and does not remove untracked ones, so the next run's `git apply` dies
# with "already exists in working directory". Remove exactly what the series
# creates — a blanket `git clean` here would take the build directory with it.
awk '/^--- \/dev\/null/ { getline; sub(/^\+\+\+ [a-z]\//, ""); print }' \
    "$PWD"/patches/qemu/0*.patch | while read -r f; do
    rm -f "$DST/$f"
done

for p in "$PWD"/patches/qemu/0*.patch; do
    git -C "$DST" apply --check "$p" || { echo "$p does not apply to $BASE" >&2; exit 1; }
    git -C "$DST" apply "$p"
    echo "   applied $(basename "$p")"
done

# A rebuilt core archive does not trigger a relink (meson link_args are flags,
# not inputs), so force it.
rm -f "$DST"/build/qemu-system-m68k "$DST"/build/qemu-system-m68k-unsigned

cd "$DST"
# ☠ --enable-plugins is NOT optional and is not about plugins: meson.build has
# `enable_cpp = host_os == 'windows' or get_option('plugins')`, so on macOS it
# is the only thing that adds the C++ language, and ot-dsp-shim.cc is
# C++. Dropping it does not save anything either — with no plugin loaded the
# instrumentation hooks are branch-skipped in the TRANSLATION path, and this
# board translates almost nothing in steady state (TB flush count 0).
#
# QEMU_EXTRA_CONFIGURE passes extra options through, e.g. -Db_lto=true (LTO is
# meson's builtin b_lto; there is no --enable-lto in this QEMU).
OCTATRACK_DSP56300=$DSPSRC OCTATRACK_DSP56300_BUILD=$DSPBLD \
    ./configure --target-list=m68k-softmmu --enable-plugins \
        ${QEMU_EXTRA_CONFIGURE:-} >/dev/null
ninja -C build qemu-system-m68k -j "$JOBS"

./build/qemu-system-m68k -M help | grep -q '^octatrack ' \
    || { echo "built, but -M octatrack is missing" >&2; exit 1; }
# grep -c, not -q: -q exits early, strings dies of SIGPIPE, pipefail fails a good build.
n=$(strings -a build/qemu-system-m68k | grep -c 'octatrack\.dsp' || true)
[ "${n:-0}" -ge 1 ] || { echo "built, but the DSP core is not linked" >&2; exit 1; }
echo "ok: $DST/build/qemu-system-m68k"
