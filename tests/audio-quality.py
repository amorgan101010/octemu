#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Judge a recorded MAIN capture the way an ear does: is the tone continuous,
and does its waveform stay a waveform?

Reports, per detected tone burst, the zero-crossing period histogram (a clean
44.1 kHz 440 Hz tone is 100 +/- 1 samples), every amplitude dropout, and every
phase discontinuity. Exits non-zero when a burst is dirtier than the limits.

  audio-quality.py X.wav [HZ] [--min-burst SEC] [--max-dropouts N]
                          [--expect-bursts N]

--expect-bursts is the one that catches the failure this file exists for. The
periodic-mute bug did not punch holes INSIDE a burst — it shattered each note
into ~20 fragments of ~48 ms, which a dropout count cannot see and a
per-burst quality gate skips as "too short to judge". Asserting the number of
sustained bursts is what fails loudly when a note is chopped.
"""
import math, struct, sys, wave

path = sys.argv[1]
hz = float(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("-") else 440.0
args = sys.argv[1:]
min_burst = float(args[args.index("--min-burst") + 1]) if "--min-burst" in args else 0.5
max_drop = int(args[args.index("--max-dropouts") + 1]) if "--max-dropouts" in args else 0
expect = int(args[args.index("--expect-bursts") + 1]) if "--expect-bursts" in args else None

w = wave.open(path, "rb")
rate, nch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
raw = w.readframes(w.getnframes())
n = len(raw) // (nch * sw)
x = [0.0] * n
for i in range(n):
    o = i * nch * sw
    v = struct.unpack_from("<h", raw, o)[0] if sw == 2 else \
        struct.unpack_from("<i", raw, o)[0] >> 16
    x[i] = v / 32768.0
print(f"{path}: {n} frames, {n/rate:.2f}s, {rate} Hz, {nch}ch")

# Envelope on 1 ms blocks; a burst is a run of blocks above 1% of its own peak.
BL = max(1, rate // 1000)
env = [max(abs(v) for v in x[i:i + BL]) or 0.0 for i in range(0, n - BL, BL)]
peak = max(env) if env else 0.0
if peak <= 0:
    print("SILENT — nothing to judge")
    sys.exit(1)
thr = peak * 0.02
runs, s = [], None
for i, e in enumerate(env):
    if e > thr and s is None:
        s = i
    elif e <= thr and s is not None:
        runs.append((s, i))
        s = None
if s is not None:
    runs.append((s, len(env)))

bad = 0
sustained = 0
for (a, b) in runs:
    dur = (b - a) * BL / rate
    if dur >= min_burst:
        sustained += 1
    if dur < 0.02:
        continue
    seg = x[a * BL:b * BL]
    # dropouts: interior blocks that fall below 20% of the burst's median level
    lv = sorted(env[a:b])[len(env[a:b]) // 2]
    # A note's attack and release ramp through the threshold from below, so
    # skip a few ms at each end: those are the envelope, not a dropout.
    EDGE = max(1, int(0.006 * rate / BL))
    holes, hs = [], None
    for i in range(a + EDGE, b - EDGE):
        if env[i] < lv * 0.2 and hs is None:
            hs = i
        elif env[i] >= lv * 0.2 and hs is not None:
            holes.append(((hs - a) * BL / rate, (i - hs) * BL / rate))
            hs = None
    # zero crossings (positive-going) -> period histogram
    zc = [i for i in range(1, len(seg)) if seg[i - 1] <= 0 < seg[i]]
    per = [zc[i + 1] - zc[i] for i in range(len(zc) - 1)]
    want = rate / hz
    good = sum(1 for p in per if abs(p - want) <= 1)
    print(f"\nburst @{a*BL/rate:7.3f}s  len {dur:6.3f}s  peak {max(abs(v) for v in seg):.3f}")
    if per:
        hist = {}
        for p in per:
            hist[p] = hist.get(p, 0) + 1
        top = sorted(hist.items(), key=lambda kv: -kv[1])[:6]
        print(f"  periods: {len(per)} total, {100*good/len(per):5.1f}% within 1 "
              f"sample of {want:.1f}  top={top}")
        print(f"  implied pitch {rate/(sum(per)/len(per)):.1f} Hz")
    if holes:
        print(f"  DROPOUTS: {len(holes)}")
        for t, d in holes[:12]:
            print(f"    at +{t:.4f}s for {d*1000:.1f} ms")
        if len(holes) > 1:
            gaps = [holes[i + 1][0] - holes[i][0] for i in range(len(holes) - 1)]
            print(f"    spacing avg {1000*sum(gaps)/len(gaps):.1f} ms")
    else:
        print("  dropouts: none")
    if dur >= min_burst and (len(holes) > max_drop or (per and good / len(per) < 0.95)):
        bad += 1

print(f"\n{len(runs)} bursts, {sustained} of them >= {min_burst}s; {bad} failing")
if expect is not None and sustained != expect:
    print(f"FAIL: expected {expect} sustained bursts, found {sustained} "
          f"({len(runs)} total) — a chopped note shows up here, not as dropouts")
    sys.exit(1)
sys.exit(1 if bad else 0)
