"""capdiff.py A B: split two --capture-dsp logs into blocks (a block ends at a
ctrl arm), align them, and report where the CPU->DSP arm stream diverges."""
import sys, hashlib


def blocks(path):
    out, cur = [], []
    with open(path) as f:
        for line in f:
            if not line.startswith('A '):
                continue
            p = line.split()
            core, kind = int(p[1]), p[2]
            words = p[4:]
            cur.append((core, kind, words))
            if kind == 'ctrl':
                out.append(cur)
                cur = []
    return out


def h(b):
    return hashlib.md5(repr(b).encode()).hexdigest()[:10]


A = blocks(sys.argv[1]); B = blocks(sys.argv[2])
print(f"A {len(A)} blocks, B {len(B)} blocks")
ha = [h(b) for b in A]; hb = [h(b) for b in B]
# align: find offset so the first 50 blocks of B match A best
best = None
for off in range(-40, 41):
    m = sum(1 for i in range(200) if 0 <= i + off < len(A) and i < len(B) and ha[i + off] == hb[i])
    if best is None or m > best[1]:
        best = (off, m)
off = best[0]
print(f"alignment: B[i] ~ A[i{off:+d}], {best[1]}/200 of the first blocks identical")
diffs = [i for i in range(len(B)) if 0 <= i + off < len(A) and ha[i + off] != hb[i]]
print(f"differing blocks: {len(diffs)} of {min(len(B), len(A) - off)}")
if diffs:
    print("first differing (B index):", diffs[:30])
    i = diffs[0]
    a, b = A[i + off], B[i]
    print(f"--- block B[{i}] / A[{i + off}]: {len(a)} vs {len(b)} arms")
    for k in range(max(len(a), len(b))):
        x = a[k] if k < len(a) else None
        y = b[k] if k < len(b) else None
        if x != y:
            print(f" arm {k}: A={x[0:2] if x else None} B={y[0:2] if y else None}")
            if x and y and x[2] != y[2]:
                dw = [j for j in range(min(len(x[2]), len(y[2]))) if x[2][j] != y[2][j]]
                print(f"   words differ at {dw[:20]} (of {len(x[2])})")
                for j in dw[:8]:
                    print(f"     [{j}] A={x[2][j]} B={y[2][j]}")
