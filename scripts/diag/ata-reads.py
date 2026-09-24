"""atalog.py LOG: summarize OCTA_ATA_LOG read commands (guest-instruction stamps)."""
import re, sys
import numpy as np

cmds = []
cur = None
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'ATARD cmd lba=(\d+) n=(\d+) ret=(\d+)', line)
    if m:
        cur = [int(m.group(1)), int(m.group(2)), int(m.group(3)), None]
        cmds.append(cur)
        continue
    m = re.search(r'ATARD done ret=(\d+)', line)
    if m and cur is not None and cur[3] is None:
        cur[3] = int(m.group(1))
done = [c for c in cmds if c[3] is not None]
print(f"read commands {len(cmds)} ({len(done)} completed), sectors {sum(c[1] for c in cmds)}")
if not done:
    sys.exit()
dur = np.array([c[3] - c[2] for c in done], dtype=float)
per = np.array([(c[3] - c[2]) / c[1] for c in done])
gaps = np.array([done[i + 1][2] - done[i][3] for i in range(len(done) - 1)], dtype=float)
span = done[-1][3] - done[0][2]
print(f"insns in commands {dur.sum() / 1e6:.1f}M, in gaps {gaps.sum() / 1e6:.1f}M, span {span / 1e6:.1f}M")
print(f"per-sector insns: median {np.median(per):.0f}  p90 {np.percentile(per, 90):.0f}  max {per.max():.0f}")
print(f"sectors/command: {np.bincount([c[1] for c in done]).nonzero()[0].tolist()[:12]}")
print(f"gap between commands: median {np.median(gaps):.0f}  p90 {np.percentile(gaps, 90):.0f}  max {gaps.max():.0f}")
