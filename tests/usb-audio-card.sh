#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# usb-audio-card.sh — stage a CF card fixture carrying the USB-audio payload.
#
# Copies the fx2 fixture (card.img + nvram.bin) to DST and injects the payload
# blob as /USBAUDIO.BIN at the card root, where custom/coldfire/usb-audio-tramp.s reads it.
# scripts/card.py writes into the image with mtools — no mount, no root.
#
#   tests/usb-audio-card.sh DST [PAYLOAD]   (default payload: out/usb-audio-payload.bin)
set -euo pipefail
cd "$(dirname "$0")/.."

DST=${1:?usage: usb-audio-card.sh DST [PAYLOAD]}
PAYLOAD=${2:-out/usb-audio-payload.bin}
SRC=out/fx2
[ -f "$PAYLOAD" ] || { echo "missing $PAYLOAD (make out/usb-audio.bin)"; exit 1; }
[ -f "$SRC/card.img" ] || { echo "missing $SRC/card.img"; exit 1; }

mkdir -p "$DST"
cp "$SRC/card.img" "$DST/card.img"
cp "$SRC/nvram.bin" "$DST/nvram.bin"

python3 scripts/card.py copy "$DST/card.img" "$PAYLOAD" /USBAUDIO.BIN
# Prove it landed: the gate's whole point is that a card the Octatrack cannot
# read in full must not look like one it can.
python3 scripts/card.py ls "$DST/card.img" | grep -qx /USBAUDIO.BIN \
    || { echo "card.py: /USBAUDIO.BIN did not land on $DST/card.img" >&2; exit 1; }
echo "ok: $DST/card.img with /USBAUDIO.BIN ($(wc -c < "$PAYLOAD") B)"
