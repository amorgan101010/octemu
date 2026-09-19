#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fetch the official Octatrack OS (you supply your own copy; nothing Elektron
# ships is redistributed here) and unpack MAIN OS section 3 -> out/os/main.bin.
set -euo pipefail
cd "$(dirname "$0")/.."

OS_VER=${OS_VER:-1.40C}
ZIP=downloads/OCTATRACK_OS${OS_VER}_dist.zip
EFT=vendor/elektron-firmware-tool/elektron-firmware-tool
[ -x "$EFT" ] || { echo "run 'make setup' first" >&2; exit 1; }

# The distribution zip this repo is developed against, pinned. Every firmware
# address in this repo and every patch offset in custom/ is an offset into THIS
# image, so a different OS is effectively a different Octatrack: if the hash
# does not match, stop rather than spend an afternoon debugging arithmetic
# against the wrong firmware. To work against another release deliberately, pass its hash:
#   OS_VER=1.40B OS_SHA256=<hash> make os
case $OS_VER in
1.40C) PINNED=370c55a3dad3996b8e4b46400a205066fdaf185ad4d0255a3a3f835060573ff0 ;;
*)     PINNED= ;;
esac
WANT=${OS_SHA256:-$PINNED}

mkdir -p downloads out/os
if [ ! -f "$ZIP" ]; then
    # The dated wp-content path rotates when Elektron reorganizes their site.
    # A 404 here is not a broken build.
    curl -fL --retry 3 -o "$ZIP" \
        "https://www.elektron.se/wp-content/uploads/2025/03/OCTATRACK_OS${OS_VER}_dist.zip" \
        || { rm -f "$ZIP"; cat >&2 <<MSG
could not download the OS distribution.

The URL above is a dated path on elektron.se and it rotates. Get
OCTATRACK_OS${OS_VER}_dist.zip yourself from

    https://www.elektron.se/support/?connected_products=octatrack-mkii

(Downloads -> OS), save it as $ZIP, and run 'make os' again. Nothing
Elektron ships is redistributed here — the copy has to be yours.
MSG
        exit 1; }
fi

GOT=$(shasum -a 256 "$ZIP" | awk '{print $1}')
if [ -n "$WANT" ] && [ "$GOT" != "$WANT" ]; then
    cat >&2 <<MSG
$ZIP does not match the pinned hash.

  expected  $WANT
  got       $GOT

This is either a truncated download (delete it and retry) or a different
build of the OS. Every firmware address in this repo and every patch offset in
custom/ belongs to the pinned image, so refusing is the only useful answer.
OS_SHA256=$GOT overrides this if that is deliberate.
MSG
    exit 1
fi
echo "sha256    : $GOT${WANT:+  (pinned, ok)}"
unzip -oq "$ZIP" -d downloads/extracted

SYX=$(find downloads/extracted -iname '*.syx' | head -1)
[ -n "$SYX" ] || { echo "no .syx in the distribution zip" >&2; exit 1; }
"$EFT" -i "$SYX" -d 3 -o out/os
cp out/os/section_3_MAIN_OS.bin out/os/main.bin
ls -la out/os/main.bin
