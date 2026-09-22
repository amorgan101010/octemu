#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""CompactFlash card images without root, hdiutil or a mount: a hand-written
MBR plus mtools for the filesystem. Same behaviour on macOS and Linux.

  scripts/card.py create IMG MiB        MBR + empty FAT32, atomically
  scripts/card.py copy IMG SRC DST      host SRC -> card path DST
  scripts/card.py extract IMG SRC DIR   card SRC (file or directory) -> host DIR
  scripts/card.py copytree IMG SRC DST  host directory SRC -> card directory DST
  scripts/card.py ls IMG [DIR]          full card paths, dirs first
  scripts/card.py finddir IMG NAME      first directory whose name is NAME

The partition offset is read back out of the image's own MBR for every
command but `create`, so these work on any card here, whatever built it.

☠ The geometry is load-bearing: the consumer is the Octatrack firmware, not
Finder. 1 sector per cluster over a 64 MiB image gives ~127,000 clusters —
genuinely FAT32 (> 65,525) and a FAT small enough for the firmware's ceiling.
Raising sectors-per-cluster would drop the count towards FAT16 territory and
the firmware would stop mounting it. Heads 16 / 63 sectors per track match
what the emulated CF reports in IDENTIFY (src/board/ot-ata.c).
"""
import os
import struct
import subprocess
import sys

SECTOR = 512
START_LBA = 2048          # LBA-aligned partition start
TYPE_FAT32_LBA = 0x0C
HEADS = 16
SPT = 63
LABEL = "OCTATRACK"


def die(msg):
    print(f"card.py: {msg}", file=sys.stderr)
    sys.exit(1)


def mtool(argv):
    """Run an mtools command; its stderr is the error message on failure."""
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    r = subprocess.run(argv, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        die(f"{argv[0]} failed: {(r.stderr or r.stdout).strip()}")
    return r.stdout


def chs(lba):
    """CHS triple for the MBR, clamped to the legacy maximum like every other
    partitioner: LBA is what actually addresses the partition."""
    c, rem = divmod(lba, HEADS * SPT)
    if c > 1023:
        return 1023, HEADS - 1, SPT
    h, s = divmod(rem, SPT)
    return c, h, s + 1


def write_mbr(path, total_sectors):
    c0, h0, s0 = chs(START_LBA)
    c1, h1, s1 = chs(total_sectors - 1)
    entry = struct.pack(
        "<BBBBBBBBII",
        0x00,                                   # not bootable
        h0, ((c0 >> 2) & 0xC0) | s0, c0 & 0xFF,
        TYPE_FAT32_LBA,
        h1, ((c1 >> 2) & 0xC0) | s1, c1 & 0xFF,
        START_LBA, total_sectors - START_LBA)
    mbr = bytearray(SECTOR)
    mbr[446:462] = entry
    mbr[510:512] = b"\x55\xaa"
    with open(path, "r+b") as f:
        f.write(mbr)


def offset(path):
    """Byte offset of partition 1, from the image's own MBR."""
    with open(path, "rb") as f:
        mbr = f.read(SECTOR)
    if len(mbr) < SECTOR or mbr[510:512] != b"\x55\xaa":
        die(f"{path}: no MBR signature")
    start, count = struct.unpack_from("<II", mbr, 446 + 8)
    if not start or not count:
        die(f"{path}: MBR partition 1 is empty")
    return start * SECTOR


def img(path):
    return f"{path}@@{offset(path)}"


def create(path, mib):
    if mib < 8:
        die(f"{mib} MiB is too small for a FAT32 card")
    total = mib * 1024 * 1024 // SECTOR
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    # Build beside the target and rename: an interrupted run never leaves a
    # half-written card where a caller would pick it up.
    tmp = f"{path}.tmp{os.getpid()}"
    try:
        with open(tmp, "wb") as f:
            f.truncate(mib * 1024 * 1024)
        write_mbr(tmp, total)
        mtool(["mformat",
               "-i", f"{tmp}@@{START_LBA * SECTOR}",
               "-F",                             # FAT32, not mtools' guess
               "-c", "1",                        # 1 sector per cluster (☠ above)
               "-R", "32",                       # reserved sectors, FAT32 norm
               "-M", str(SECTOR),
               "-h", str(HEADS), "-s", str(SPT),
               "-H", str(START_LBA),             # hidden sectors = our start
               "-T", str(total - START_LBA),
               "-v", LABEL, "::"])
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def copy(path, src, dst):
    if not os.path.isfile(src):
        die(f"{src}: not a file")
    mtool(["mcopy", "-i", img(path), "-o", src, f"::{dst}"])


def extract(path, src, dstdir):
    """card path SRC (a file, or a directory copied recursively) -> host
    directory DSTDIR, created if needed."""
    os.makedirs(dstdir, exist_ok=True)
    mtool(["mcopy", "-i", img(path), "-s", "-n", f"::{src}", dstdir])


def copytree(path, src, dst):
    """host directory SRC -> the card, recursively, as directory DST."""
    if not os.path.isdir(src):
        die(f"{src}: not a directory")
    mtool(["mcopy", "-i", img(path), "-s", "-o", src, f"::{dst}"])


def ls(path, top="/"):
    out = mtool(["mdir", "-/", "-b", "-i", img(path), f"::{top}"])
    return [l[2:] for l in out.splitlines() if l.startswith("::/")]


def finddir(path, name):
    for p in ls(path):
        if p.endswith("/") and p.rstrip("/").rsplit("/", 1)[-1] == name:
            return p.rstrip("/")
    return None


def main():
    a = sys.argv[1:]
    if not a:
        die(__doc__.strip())
    cmd = a[0]
    if cmd == "create" and len(a) == 3:
        create(a[1], int(a[2]))
    elif cmd == "copy" and len(a) == 4:
        copy(a[1], a[2], a[3])
    elif cmd == "extract" and len(a) == 4:
        extract(a[1], a[2], a[3])
    elif cmd == "copytree" and len(a) == 4:
        copytree(a[1], a[2], a[3])
    elif cmd == "ls" and len(a) in (2, 3):
        print("\n".join(ls(a[1], a[2] if len(a) == 3 else "/")))
    elif cmd == "finddir" and len(a) == 3:
        d = finddir(a[1], a[2])
        if d is None:
            die(f"{a[1]}: no directory named {a[2]}")
        print(d)
    else:
        die(__doc__.strip())


if __name__ == "__main__":
    try:
        main()
    except OSError as e:
        die(str(e))                       # loud, but without the traceback
