"""pchist.py PCS.bin ATALOG: guest-time PC profile over the project-load window
(first to last 1-sector read after the PTCH mark), by firmware symbol and IPL."""
import re, sys, collections
import numpy as np

syms = []
for l in open('re/coldfire.syms'):
    m = re.match(r'\s*(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;', l)
    if m:
        syms.append((int(m.group(2), 16), m.group(1)))
syms.sort()
addrs = np.array([s[0] for s in syms], dtype=np.int64)
names = [s[1] for s in syms]

ptch = None; reads = []
for l in open(sys.argv[2], errors='replace'):
    m = re.search(r'\[mark\] blk=(\d+) \d+ ptch', l)
    if m:
        ptch = int(m.group(1))
    m = re.search(r'ATARD cmd lba=\d+ n=1 ret=\d+ blk=(\d+)', l)
    if m:
        reads.append(int(m.group(1)))
reads = [b for b in reads if ptch is None or b >= ptch]
b0, b1 = reads[0], reads[-1]
d = np.fromfile(sys.argv[1], dtype=np.uint32).reshape(-1, 3)
w = d[(d[:, 2] >= b0) & (d[:, 2] <= b1)]
print(f"load window blk {b0}..{b1} ({(b1 - b0) / 2756.25:.2f} s), {len(w)} samples "
      f"= {len(w) * 512 / 1e6:.0f}M guest insns, {len(w) / max(b1 - b0, 1):.1f} quanta/block")
idx = np.searchsorted(addrs, w[:, 0].astype(np.int64), side='right') - 1
ipl = (w[:, 1] >> 8) & 7
c = collections.Counter()
for i, p in zip(idx, ipl):
    c[(names[i] if i >= 0 else '?', int(p))] += 1
tot = len(w)
for (n, p), k in c.most_common(22):
    print(f"{100 * k / tot:5.1f}%  ipl{p}  {n}")
print("by IPL:", {p: f"{100 * np.mean(ipl == p):.1f}%" for p in range(8) if np.any(ipl == p)})
