#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Machine/FX/stress variants of the trigsweep and trig8 fixtures (bug-031).

    tests/trigsweep-variant.py [--from trig8] OUTDIR [T<n>:<setting>=<value> ...]

    settings:  type=flex|neighbor|static|thru|pickup   (machine type)
               fx1=<id|name>  fx2=<id|name>             (stock FX)
               stress=1   trig all 16 steps of pattern A01, lock 15 parameters
                          on every step and run three LFOs (a Static track
                          becomes Flex on T3's slot first)

    tests/trigsweep-variant.py out/tsv/hi T5:fx2=comb T6:type=neighbor T7:fx2=spring
    FX=out/tsv/hi tests/trigsweep.sh hi q56 q7 q12

Writes OUTDIR/set.tgz and OUTDIR/nvram.bin.gz, a drop-in replacement for the
fixture (FX=OUTDIR for tests/trigsweep.sh or tests/trig8-repro.sh). Edits bank
1 of the project the fixture's NVRAM opens: part 1 (the part it plays), then
parts 1-4 copied over their saved copies 5-8 so nothing the firmware restores
(RELOAD PART, a transport start re-applying the saved part) can bring back the
old setup. The user's projects were never part-saved: their parts 5-8 are
blank, so even an empty setting list changes the fixture (the "mirrored"
control).

The NVRAM matters as much as the card: it holds the firmware's working copy
of the open bank, and the firmware boots from that copy, not from the card's
bank file (edits to the card alone change nothing: measured, T2 set to Flex
still played as a Neighbor). So every edit goes to both. In the NVRAM, part p
(RAM layout, the file's record without its 9-byte header) sits at 0xa4ece +
p * 0x18b2 and pattern A01's track t at 0x1614e + t * 0x91a. Those addresses
are checked against the unedited bank file before anything is written.

Stress is for the tracks a walk mutes (muted tracks keep trigging and their
FX keep running): locks and LFOs change the per-block track records that
bug-031's late transfer carries. Keep it off the scored track and anything
the scored track feeds (a Neighbor).

Byte layout and checksum after OctaBam (github.com/sambanks/octabam, MIT,
(c) 2026 Sam Banks): tools/hw/ot_project.py for the part records (0x8eed6 +
part * 0x18bb; FX1 ids +0x09, FX2 ids +0x11, machine types +0x2b, flex slot
+0x2d4 + track * 5, LFO page 1 +0x123 + track * 24, LFO setup +0x2fb + track *
30) and the big-endian u16 additive sum over bytes[0x10:-2]; tools/hw/ot_bank.py
for the patterns (PTRN at 0x16 + pattern * 0x8eec, TRAC data at +8 + track *
0x922 + 9, big-endian 64-step masks, lock records of 32 bytes per step at
TRAC data + 0x59, 0xff = none); tools/harness/stress_project.py for the lock
slots, lock values and LFO bytes. Checked against these fixtures: part 1 reads
the flex/neighbor types and the comb (0x13) / spring reverb (0x15) FX2 the
projects were built with, and the trig masks read 0xffff (every 16th) and
0x1111 (quarters) as the walks expect. Other FX ids follow the stock FX list
and are unverified until a variant using them is load-checked.
"""
import gzip
import pathlib
import shutil
import sys
import tarfile
import tempfile

HERE = pathlib.Path(__file__).resolve().parent / 'fixtures'
FIXTURES = {'trigsweep': 'TESTDROPOUT', 'trig8': 'PROJECT 260924'}  # the project each NVRAM opens
PART_BASE, PART_STRIDE, NPARTS = 0x8eed6, 0x18bb, 4
FX1_OFF, FX2_OFF, TYPE_OFF = 0x09, 0x11, 0x2b
FLEX_SLOT, LFO_P1, LFO_SETUP = 0x2d4, 0x123, 0x2fb
TRAC0, PTRN_STRIDE, TRAC_STRIDE = 0x16 + 8 + 9, 0x8eec, 0x922   # pattern A01 data
LOCKS, LOCK_LEN = 0x59, 32
NV_PART, NV_PART_STRIDE, NV_TRAC, NV_TRAC_STRIDE = 0xa4ece, 0x18b2, 0x1614e, 0x91a
PART_DATA, TRAC_DATA = PART_STRIDE - 9, TRAC_STRIDE - 9
LOCK_SLOTS = (0, 3, 6, 7, 9, 10, 15, 16, 18, 19, 20, 21, 22, 23, 24)
TYPES = {'static': 0, 'flex': 1, 'thru': 2, 'neighbor': 3, 'pickup': 4}
FX = {'none': 0x00, 'filter': 0x04, 'spatializer': 0x05, 'delay': 0x08,
      'eq': 0x0c, 'djeq': 0x0d, 'phaser': 0x10, 'flanger': 0x11,
      'chorus': 0x12, 'comb': 0x13, 'plate': 0x14, 'spring': 0x15,
      'dark': 0x16, 'compressor': 0x18, 'lofi': 0x1c}


def parse(arg):
    track, rest = arg.split(':', 1)
    key, val = rest.split('=', 1)
    t = int(track.lstrip('Tt')) - 1
    if not 0 <= t < 8:
        sys.exit(f'{arg}: track is T1..T8')
    if key == 'type':
        return t, key, TYPES[val]
    if key in ('fx1', 'fx2'):
        return t, key, FX[val] if val in FX else int(val, 0)
    if key == 'stress':
        return t, key, int(val)
    sys.exit(f'{arg}: setting is type, fx1, fx2 or stress')


def checksum(d):
    return sum(d[0x10:-2]) & 0xFFFF


def stress(d, t):
    """stress_project.py's recipe for one track, on pattern A01, 16 steps."""
    part = PART_BASE
    if d[part + TYPE_OFF + t] == TYPES['static']:
        d[part + TYPE_OFF + t] = TYPES['flex']
        d[part + FLEX_SLOT + t * 5] = d[part + FLEX_SLOT + 2 * 5]   # T3's slot
    d[part + LFO_P1 + t * 24:part + LFO_P1 + t * 24 + 6] = bytes((20, 36, 52, 20, 28, 18))
    d[part + LFO_SETUP + t * 30:part + LFO_SETUP + t * 30 + 6] = bytes((18, 19, 24, 1, 1, 1))
    trac = TRAC0 + t * TRAC_STRIDE
    d[trac:trac + 8] = (0xffff).to_bytes(8, 'big')                  # every step
    for step in range(16):
        rec = trac + LOCKS + step * LOCK_LEN
        for slot in LOCK_SLOTS:
            v = (step * 17 + t * 11 + slot * 7) % 128
            if slot == 0:
                v = (52, 64, 76, 64)[(step + t) % 4]                # pitch
            elif slot == 3:
                v = (96, 112, 127)[(step + t) % 3]                  # rate
            elif slot == 24:
                v = 20 + v % 51
            elif slot in (15, 16):
                v = 48 + v % 33                                     # vol, bal
            d[rec + slot] = v


def edit(path, edits):
    d = bytearray(path.read_bytes())
    if int.from_bytes(d[-2:], 'big') != checksum(d):
        sys.exit(f'{path.name}: checksum wrong before editing')
    for t, key, v in edits:
        if key == 'stress':
            if v:
                stress(d, t)
        else:
            d[PART_BASE + {'type': TYPE_OFF, 'fx1': FX1_OFF, 'fx2': FX2_OFF}[key] + t] = v
    for p in range(NPARTS):         # parts 1-4 -> their saved copies 5-8
        a = PART_BASE + p * PART_STRIDE
        b = PART_BASE + (p + NPARTS) * PART_STRIDE
        d[b:b + PART_STRIDE] = d[a:a + PART_STRIDE]
    d[-2:] = checksum(d).to_bytes(2, 'big')
    path.write_bytes(bytes(d))
    back = path.read_bytes()        # read back: every edit and the sum
    assert int.from_bytes(back[-2:], 'big') == checksum(back)
    for t, key, v in edits:
        if key in ('type', 'fx1', 'fx2'):
            off = {'type': TYPE_OFF, 'fx1': FX1_OFF, 'fx2': FX2_OFF}[key]
            for p in (0, NPARTS):
                assert back[PART_BASE + p * PART_STRIDE + off + t] == v
        elif v:
            trac = TRAC0 + t * TRAC_STRIDE
            assert int.from_bytes(back[trac:trac + 8], 'big') == 0xffff
            assert back[trac + LOCKS + 15 * LOCK_LEN + 24] != 0xff


def nv_regions():
    """(file offset, NVRAM offset, length) of every bank region the NVRAM mirrors."""
    for p in range(2 * NPARTS):
        yield PART_BASE + p * PART_STRIDE + 9, NV_PART + p * NV_PART_STRIDE, PART_DATA
    for t in range(8):
        yield TRAC0 + t * TRAC_STRIDE, NV_TRAC + t * NV_TRAC_STRIDE, TRAC_DATA


def sync_nvram(nv, before, after):
    """Copy the edited bank into the NVRAM's working copy, after checking
    that the NVRAM held exactly the unedited bank at every mapped address."""
    for f, n, ln in nv_regions():
        if nv[n:n + ln] != before[f:f + ln]:
            sys.exit(f'NVRAM {n:#x} does not mirror bank offset {f:#x}: layout unknown, not writing')
        nv[n:n + ln] = after[f:f + ln]


def main():
    args = sys.argv[1:]
    fixture = 'trigsweep'
    if args[:1] == ['--from']:
        fixture, args = args[1], args[2:]
    if not args or fixture not in FIXTURES:
        sys.exit(__doc__)
    src, out = HERE / fixture, pathlib.Path(args[0])
    edits = [parse(a) for a in args[1:]]
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        with tarfile.open(src / 'set.tgz') as tf:
            tf.extractall(tmp)
        banks = sorted(tmp.glob(f'*/{FIXTURES[fixture]}/bank01.*'))
        if len(banks) != 2:
            sys.exit(f'expected bank01.work and .strd, found {banks}')
        work = [b for b in banks if b.suffix == '.work'][0]
        before = work.read_bytes()
        for b in banks:
            edit(b, edits)
        nv = bytearray(gzip.decompress((src / 'nvram.bin.gz').read_bytes()))
        sync_nvram(nv, before, work.read_bytes())
        (sset,) = [p for p in tmp.iterdir() if p.is_dir()]
        with tarfile.open(out / 'set.tgz', 'w:gz') as tf:
            tf.add(sset, arcname=sset.name)
    (out / 'nvram.bin.gz').write_bytes(gzip.compress(bytes(nv)))
    if (src / 'walk.jsonl').exists():
        shutil.copy(src / 'walk.jsonl', out / 'walk.jsonl')
    (out / 'VARIANT').write_text(' '.join(sys.argv[1:]) + '\n')
    print(f'{out}: {fixture} {" ".join(args[1:]) or "(mirrored only)"}')


if __name__ == '__main__':
    main()
