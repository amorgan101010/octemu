# SPDX-License-Identifier: MIT
"""handoff-margin.py TRACE PLAY_BLOCK [TRACE PLAY_BLOCK ...]

The record-delivery margin (bug-025 / bug-031) on the GUEST-instruction clock,
from an OCTA_HANDOFF_TRACE run with OCTA_HANDOFF_WATCH on word 30 of the
core-1 slots (see handoff-flip.py). The guest clock is the right ruler: the
race is decided while the codec is frozen holding its bank-index word, when
the codec's own clock does not move but the ColdFire and core 1 do.

For every gate-word landing (M line) after PLAY_BLOCK, per track slot:
  regime   in-render: the last bank index core 1 took selects this bank
           (core 1 had begun this block's render); ahead: the other bank
  since    guest instructions since core 1's last bank-index take
  to_next  guest instructions until its next one

Silicon's contract (the 0x88 handler's DMA mask is rewritten at each take):
every landing in-render, with to_next never small. The minimum to_next over a
run is the margin; a mix of regimes is the bug.
"""
import bisect
import collections
import re
import sys


def pct(a, ps=(0, 1, 50, 99, 100)):
    a = sorted(a)
    if not a:
        return '-'
    return ' '.join(f"p{p}={a[min(len(a) - 1, int(len(a) * p / 100))]}" for p in ps)


def main():
    args = sys.argv[1:]
    for fn, play in zip(args[0::2], args[1::2]):
        play = int(play)
        takes, banks = [], []
        land = collections.defaultdict(list)
        for l in open(fn, errors='replace'):
            if l[0] == 'T':
                m = re.search(r'blk=(-?\d+) w=([01]) got=\d g=(\d+)', l)
                if m and int(m.group(1)) >= play:
                    takes.append(int(m.group(3)))
                    banks.append(0x4000 if m.group(2) == '1' else 0x2000)
            elif l[0] == 'M':
                m = re.search(r'blk=(-?\d+) x:(0x\w+) .* g=(\d+)', l)
                if m and int(m.group(1)) >= play:
                    a = int(m.group(2), 16)
                    land[((a & 0x1ff) - 0x1e) // 0x20].append((int(m.group(3)), a & ~0x1ff))
        print(f"== {fn}  ({len(takes)} bank takes after play)")
        for slot in sorted(land):
            since, to_next, reg = [], [], collections.Counter()
            for g, bank in land[slot]:
                i = bisect.bisect_right(takes, g)
                if not 0 < i < len(takes):
                    continue
                reg['in' if banks[i - 1] == bank else 'ahead'] += 1
                since.append(g - takes[i - 1])
                to_next.append(takes[i] - g)
            print(f"  T{slot + 1}: {dict(reg)}")
            print(f"      since take : {pct(since)}")
            print(f"      to next    : {pct(to_next)}")


if __name__ == '__main__':
    main()
