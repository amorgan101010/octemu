#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Create a FAT32 CompactFlash image the firmware will mount: MBR with one
# FAT32-LBA partition at LBA 2048, 64 MiB by default (127,006 one-sector
# clusters — genuine FAT32 and a FAT inside the firmware's ceiling).
# scripts/card.py does the work with mtools: no root, no mount, Linux too.
set -euo pipefail
OUT=${1:?usage: mkcard.sh PATH [MiB]}
MB=${2:-64}

python3 "$(dirname "$0")/card.py" create "$OUT" "$MB"
echo "ok: $OUT (${MB} MiB FAT32)"
