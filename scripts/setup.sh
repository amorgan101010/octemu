#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Vendor toolchain: dsp56300 (the DSP core) and elektron-firmware-tool (OS
# unpacker). Idempotent.
set -euo pipefail
cd "$(dirname "$0")/.."

command -v cmake >/dev/null || { echo "cmake not found — brew install cmake" >&2; exit 1; }

echo "== dsp56300 =="
DSP=vendor/dsp56300
PIN=$(cat patches/dsp56300/BASE_COMMIT.txt)
if [ ! -d "$DSP/.git" ]; then
    git clone https://github.com/dsp56300/dsp56300.git "$DSP"
fi
git -C "$DSP" cat-file -e "$PIN^{commit}" 2>/dev/null || git -C "$DSP" fetch --all
git -C "$DSP" checkout -q --detach "$PIN"
git -C "$DSP" reset -q --hard "$PIN"
git -C "$DSP" submodule update --init --depth 1 --recursive
for p in patches/dsp56300/0*.patch; do
    git -C "$DSP" apply "$(pwd)/$p"
    echo "   applied $(basename "$p")"
done
scripts/build-dsp.sh

echo "== elektron-firmware-tool =="
EFT=vendor/elektron-firmware-tool
# Pinned, like the other two dependencies, because the `make fw-*` targets
# depend on this tool's CLI and on how it rebuilds a container — both of which
# have changed upstream before. EFT_PIN= tracks HEAD instead, deliberately.
EFT_PIN=${EFT_PIN-a5bce9a6af644386d900924082993a805e812874}
if [ ! -d "$EFT" ]; then
    git clone https://github.com/mischa85/elektron-firmware-tool "$EFT"
fi
if [ -n "$EFT_PIN" ]; then
    git -C "$EFT" cat-file -e "$EFT_PIN^{commit}" 2>/dev/null || git -C "$EFT" fetch --all
    git -C "$EFT" checkout -q --detach "$EFT_PIN"
    git -C "$EFT" reset -q --hard "$EFT_PIN"
fi
# The two Octatrack-specific behaviours this repo needs are UPSTREAM as of the
# pin above and need no patching: the ELEK version field is the full 10-byte
# run from container offset 0x08 (not just the alphanumeric tail at 0x0D), and
# `--emit-container` writes the rebuilt container out for custom/make-bin.py to
# wrap for the CF-card ELUP path. If a future pin loses either, `make fw-*`
# says so rather than writing a bad file.
rm -f "$EFT/elektron-firmware-tool"
if [ -f "$EFT/Makefile" ]; then
    make -C "$EFT"
else
    cc -O2 -o "$EFT/elektron-firmware-tool" "$EFT"/*.c
fi
"$EFT/elektron-firmware-tool" -h 2>&1 | grep -q -- "--emit-container" || {
    echo "   this elektron-firmware-tool has no --emit-container; the" >&2
    echo "     'make fw-*' targets need it. Pin EFT_PIN to a commit that has it." >&2
    exit 1; }
echo "   ok: $EFT/elektron-firmware-tool"
