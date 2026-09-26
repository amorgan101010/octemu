#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Machine/FX variants of the trigsweep fixture (bug-031 combo sweep).

    tests/trigsweep-variant.py OUTDIR [T<n>:<setting>=<value> ...]

    settings:  type=flex|neighbor|static|thru|pickup   (machine type)
               fx1=<id|name>  fx2=<id|name>             (stock FX)

    tests/trigsweep-variant.py out/tsv/hi T5:fx2=comb T6:type=neighbor T7:fx2=spring
    FX=out/tsv/hi tests/trigsweep.sh hi q56 q7 q12

Writes OUTDIR/set.tgz and OUTDIR/nvram.bin.gz, a drop-in replacement for
tests/fixtures/trigsweep. Edits bank 1 part 1 (the part this project plays),
then copies parts 1-4 over their saved copies 5-8 so nothing the firmware
restores (RELOAD PART, a transport start re-applying the saved part) can
bring back the old setup. The user's fixture was never part-saved: its parts
5-8 are blank, so even an empty setting list changes the fixture (the
"mirrored" control).

Byte layout and checksum after OctaBam's tools/hw/ot_project.py
(github.com/sambanks/octabam, MIT, (c) 2026 Sam Banks): part records at
0x8eed6 + part * 0x18bb, FX1 ids at +0x09, FX2 ids at +0x11, machine types at
+0x2b (one byte per track), and a big-endian u16 additive sum over
bytes[0x10:-2] at the end of the file. Checked against this fixture: part 1
reads flex/neighbor types and comb (0x13) / spring reverb (0x15) on T1/T3 FX2,
as the project was built. The other FX ids follow the stock FX list order and
are unverified until a variant using them is load-checked.
"""
import gzip
import pathlib
import shutil
import sys
import tarfile
import tempfile

FIX = pathlib.Path(__file__).resolve().parent / 'fixtures' / 'trigsweep'
PART_BASE, PART_STRIDE, NPARTS = 0x8eed6, 0x18bb, 4
FX1_OFF, FX2_OFF, TYPE_OFF = 0x09, 0x11, 0x2b
TYPES = {'static': 0, 'flex': 1, 'thru': 2, 'neighbor': 3, 'pickup': 4}
FX = {'none': 0x00, 'filter': 0x04, 'spatializer': 0x05, 'delay': 0x08,
      'eq': 0x0c, 'djeq': 0x0d, 'phaser': 0x10, 'flanger': 0x11,
      'chorus': 0x12, 'comb': 0x13, 'plate': 0x14, 'spring': 0x15,
      'dark': 0x16, 'compressor': 0x18, 'lofi': 0x1c}


def parse(arg):
    track, rest = arg.split(':', 1)
    key, val = rest.split('=', 1)
    t = int(track.lstrip('Tt'))
    if not 1 <= t <= 8:
        sys.exit(f'{arg}: track is T1..T8')
    if key == 'type':
        return t - 1, TYPE_OFF, TYPES[val]
    if key in ('fx1', 'fx2'):
        v = FX[val] if val in FX else int(val, 0)
        return t - 1, FX1_OFF if key == 'fx1' else FX2_OFF, v
    sys.exit(f'{arg}: setting is type, fx1 or fx2')


def checksum(d):
    return sum(d[0x10:-2]) & 0xFFFF


def edit(path, edits):
    d = bytearray(path.read_bytes())
    if int.from_bytes(d[-2:], 'big') != checksum(d):
        sys.exit(f'{path.name}: checksum wrong before editing')
    for t, off, v in edits:
        d[PART_BASE + off + t] = v
    for p in range(NPARTS):         # parts 1-4 -> their saved copies 5-8
        a = PART_BASE + p * PART_STRIDE
        b = PART_BASE + (p + NPARTS) * PART_STRIDE
        d[b:b + PART_STRIDE] = d[a:a + PART_STRIDE]
    d[-2:] = checksum(d).to_bytes(2, 'big')
    path.write_bytes(bytes(d))
    back = path.read_bytes()        # read back: every edit and the sum
    assert int.from_bytes(back[-2:], 'big') == checksum(back)
    for t, off, v in edits:
        for p in (0, NPARTS):
            assert back[PART_BASE + p * PART_STRIDE + off + t] == v


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = pathlib.Path(sys.argv[1])
    edits = [parse(a) for a in sys.argv[2:]]
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        with tarfile.open(FIX / 'set.tgz') as tf:
            tf.extractall(tmp)
        # The set also holds an untouched PROJECT 260924; the NVRAM opens TESTDROPOUT.
        banks = sorted(tmp.glob('*/TESTDROPOUT/bank01.*'))
        if len(banks) != 2:
            sys.exit(f'expected bank01.work and .strd, found {banks}')
        for b in banks:
            edit(b, edits)
        (sset,) = [p for p in tmp.iterdir() if p.is_dir()]
        with tarfile.open(out / 'set.tgz', 'w:gz') as tf:
            tf.add(sset, arcname=sset.name)
    shutil.copy(FIX / 'nvram.bin.gz', out / 'nvram.bin.gz')
    (out / 'VARIANT').write_text(' '.join(sys.argv[2:]) + '\n')
    print(f'{out}: {" ".join(sys.argv[2:]) or "(mirrored only)"}')


if __name__ == '__main__':
    main()
