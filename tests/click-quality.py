#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Assert a recording carries metronome clicks at a steady spacing.

The click is one decaying tone per beat, so "did it fire" is the count of
onsets separated by silence, and "did it fire on the beat" is the spread of
their spacing. Both are load-invariant: the recording is the Octatrack's own
timeline, not the host's.
"""
import argparse, array, sys, wave

ap = argparse.ArgumentParser()
ap.add_argument("wav")
ap.add_argument("--expect-clicks", type=int, default=1)
ap.add_argument("--spacing", type=float, help="expected seconds between clicks")
ap.add_argument("--tolerance", type=float, default=0.1)
a = ap.parse_args()

w = wave.open(a.wav)
n, rate = w.getnframes(), w.getframerate()
pcm = array.array("h")
pcm.frombytes(w.readframes(n))
left = pcm[0::2]
peak = max((abs(x) for x in left), default=0)
print("%s: %.1fs, peak %d" % (a.wav, n / rate, peak))
if not peak:
    sys.exit("silent recording — no clicks at all")

thr, refractory = peak // 3, rate // 20
onsets, last = [], -rate
for i, v in enumerate(left):
    if abs(v) > thr and i - last > refractory:
        onsets.append(i)
        last = i
gaps = [(onsets[i + 1] - onsets[i]) / rate for i in range(len(onsets) - 1)]
print("%d clicks" % len(onsets), end="")
if gaps:
    print(", spacing %.3f-%.3fs (mean %.3f)"
          % (min(gaps), max(gaps), sum(gaps) / len(gaps)), end="")
print()

fail = []
if len(onsets) < a.expect_clicks:
    fail.append("wanted >= %d clicks, got %d" % (a.expect_clicks, len(onsets)))
if a.spacing and gaps:
    off = [g for g in gaps if abs(g - a.spacing) > a.tolerance]
    if off:
        fail.append("%d of %d gaps off %.3fs by more than %.3fs"
                    % (len(off), len(gaps), a.spacing, a.tolerance))
if fail:
    sys.exit("FAIL: " + "; ".join(fail))
print("PASS")
