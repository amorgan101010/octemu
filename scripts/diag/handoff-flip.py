# SPDX-License-Identifier: MIT
"""handoff-flip.py TRACE PLAY_BLOCK [-v]: split note-ons in an OCTA_HANDOFF_TRACE.

Run with OCTA_HANDOFF_WATCH set to word 30 of all four core-1 slots in both
banks: 201e,203e,205e,207e,401e,403e,405e,407e (tracks 1-4; 5-8 are on the
other core). PLAY_BLOCK is the [mark] blk= of the play mark in the run log.

For all 8 slots. For every note-on landing
(word 30 gains bit 8) after PLAY, is the landing regime (in-render vs ahead of
core 1's bank-index take) different for the next landing in the other bank?
Prints note-ons, the regime mix and which note-ons are split, per slot.
With -v, the split note-ons' landing details."""
import re, sys, collections

fn, play = sys.argv[1], int(sys.argv[2])
cur = None
land = collections.defaultdict(list)
for l in open(fn):
    if l[0] == 'T':
        m = re.search(r' w=([01]) ', l)
        if m:
            cur = 0x4000 if m.group(1) == '1' else 0x2000
    elif l[0] == 'M':
        m = re.search(r'blk=(-?\d+) x:(0x\w+) (\w+)>(\w+) .*e1=(\d+)', l)
        b, a, o, n, e1 = (int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16),
                          int(m.group(4), 16), int(m.group(5)))
        bank = a & ~0x1ff
        slot = ((a & 0x1ff) - 0x1e) // 0x20
        land[slot].append((b, bank, o, n, 'in' if bank == cur else 'ahead', e1))
for slot in range(8):
    L = land.get(slot, [])
    st = [i for i, x in enumerate(L) if x[0] >= play and x[3] & 0x100 and not x[2] & 0x100]
    split = []
    for k, i in enumerate(st, 1):
        s = L[i]
        f = next((x for x in L[i + 1:i + 4] if x[1] != s[1]), None)
        if f and f[4] != s[4]:
            split.append((k, s, f))
    reg = collections.Counter(x[4] for x in L if x[0] >= play)
    print(f"T{slot + 1}: {len(st)} note-ons, landings {dict(reg)}, split {len(split)}"
          f" {[k for k, _, _ in split][:24]}")
    if '-v' in sys.argv:
        for k, s, f in split[:6]:
            print(f"    #{k} blk={s[0]} strobe {s[4]}@{s[5]} {s[2]:03x}>{s[3]:03x}"
                  f"  next {f[4]}@{f[5]} {f[2]:03x}>{f[3]:03x}")
