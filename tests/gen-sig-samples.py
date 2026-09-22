#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate eight track-signature samples for the USB-audio channel fixture.

Each track gets a STEREO tone whose LEFT and RIGHT channels are DISTINCT and
unique across all 16 channels, so a capture identifies every channel
unambiguously and proves L/R order: channel c (1..16) = BASE + STEP*c Hz, i.e.
track N left = BASE+STEP*(2N-1), right = BASE+STEP*(2N). With BASE=200, STEP=50
that is 250/300 (track 1) … 950/1000 (track 8), sixteen tones 50 Hz apart.

Pure sines, integer cycles trimmed to a zero crossing at both ends so a
one-shot plays cleanly. 16-bit, 44.1 kHz, the format the STATIC machine plays
at rate 1:1 (no timestretch), so the emitted frequency IS the sample's.

  tests/gen-sig-samples.py OUTDIR [SECONDS]
"""
import math, struct, sys, wave, os

BASE, STEP, RATE, AMP = 200, 50, 44100, 12000


def freq(ch):                      # ch 1..16
    return BASE + STEP * ch


def write(path, fl, fr, secs):
    n = int(RATE * secs)
    w = wave.open(path, "wb")
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(RATE)
    buf = bytearray()
    for i in range(n):
        l = int(AMP * math.sin(2 * math.pi * fl * i / RATE))
        r = int(AMP * math.sin(2 * math.pi * fr * i / RATE))
        buf += struct.pack("<hh", l, r)
    w.writeframes(bytes(buf)); w.close()


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "out/sig"
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    os.makedirs(out, exist_ok=True)
    for t in range(1, 9):
        fl, fr = freq(2 * t - 1), freq(2 * t)
        p = os.path.join(out, "SIG%d.WAV" % t)
        write(p, fl, fr, secs)
        print("track %d: L %d Hz  R %d Hz -> %s (%.0fs)" % (t, fl, fr, p, secs))


main()
