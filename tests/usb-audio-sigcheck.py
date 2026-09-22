#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify the 16-channel signature: every channel carries its OWN tone.

The signature fixture (tests/build-sig-fixture.sh) plays a distinct stereo
tone on each of the eight tracks — track N left = 200+50*(2N-1) Hz, right =
200+50*(2N) Hz — so the sixteen USB channels are 250, 300, 350 … 1000 Hz, each
unique. A capture then IDENTIFIES every channel by its frequency: channel c
(1-based) must carry 200+50*c Hz. That proves the track->channel mapping AND
L/R order (left is the lower tone of each pair), which isolation + a swap
control cannot.

Two inputs:
  --pcm FILE.pcm   a raw interleaved 16 x s16 LE capture (the USB iso stream)
  --dump FILE      the emulator readback dump (OCTA_RB_DUMP): per-track L/R,
                   used to validate the FIXTURE itself before it is streamed

Asserts, per loud channel: dominant frequency within TOL Hz of 200+50*c, and
continuity (no sample-to-sample jump beyond a sine of that tone can make).
A channel expected loud but silent, or off-frequency, fails.

  tests/usb-audio-sigcheck.py --pcm cap.pcm [--tol 8] [--channels 16]
  tests/usb-audio-sigcheck.py --dump rb.dump
"""
import struct, sys, math

RATE = 44100
BASE, STEP = 200, 50


def expected(ch):                 # ch 1..16
    return BASE + STEP * ch


def dom_freq(seq):
    """Steady-state zero-crossing frequency over the loud tail."""
    s = seq[-120000:] if len(seq) > 120000 else seq
    pk = max((abs(x) for x in s), default=0)
    if pk < 64:
        return 0.0, pk
    xs = [k for k in range(1, len(s)) if s[k-1] < 0 <= s[k]]
    if len(xs) < 8:
        return 0.0, pk
    return RATE * (len(xs) - 1) / (xs[-1] - xs[0]), pk


def cont_breaks(seq, tone):
    pk = max((abs(x) for x in seq), default=0)
    if pk == 0 or tone == 0:
        return 0
    g = pk // 4
    bound = pk * 2 * math.pi * tone / RATE * 3
    n = 0
    for k in range(1, len(seq)):
        a, b = seq[k-1], seq[k]
        if abs(a) < g or abs(b) < g:
            continue
        if abs(b - a) > bound:
            n += 1
    return n


def load_pcm(path, nch):
    b = open(path, "rb").read()
    fb = nch * 2
    n = len(b) // fb
    chans = [[] for _ in range(nch)]
    for i in range(n):
        fr = struct.unpack_from("<%dh" % nch, b, i * fb)
        for c in range(nch):
            chans[c].append(fr[c])
    return chans


def load_dump(path):
    """Readback dump -> 16 channels (track t L = channel 2t-1, R = 2t)."""
    RB = 0x80003190
    b = open(path, "rb").read()
    chans = [[] for _ in range(16)]
    i = 0
    while i + 8 <= len(b):
        da, nb = struct.unpack_from(">II", b, i); i += 8
        data = b[i:i+nb]; i += nb
        off = da - RB
        if 0 <= off < 2048:
            t = (off % 1024) // 128
            for j in range(0, nb - 7, 8):
                l, r = struct.unpack_from(">ii", data, j)
                chans[2*t].append(l >> 16)
                chans[2*t+1].append(r >> 16)
    return chans


def main():
    a = sys.argv
    tol = float(a[a.index("--tol")+1]) if "--tol" in a else 8.0
    nch = int(a[a.index("--channels")+1]) if "--channels" in a else 16
    if "--pcm" in a:
        chans = load_pcm(a[a.index("--pcm")+1], nch)
        src = "capture"
    elif "--dump" in a:
        chans = load_dump(a[a.index("--dump")+1])
        src = "readback dump"
    else:
        sys.exit(__doc__)
    print(f"{src}: {len(chans)} channels, {len(chans[0])} frames each")
    ok = True
    loud = 0
    for c in range(len(chans)):
        want = expected(c + 1)
        f, pk = dom_freq(chans[c])
        if pk < 64:
            print(f"  channel {c+1:2d}: SILENT (peak {pk}) — expected {want} Hz")
            ok = False
            continue
        loud += 1
        br = cont_breaks(chans[c], want)
        good = abs(f - want) <= tol and br == 0
        ok = ok and good
        mark = "ok " if good else "✗  "
        note = ""
        if abs(f - want) > tol:
            note += f" FREQ off (want {want})"
        if br:
            note += f" {br} continuity breaks"
        print(f"  {mark}channel {c+1:2d}: {f:6.1f} Hz (want {want}), peak {pk}"
              f"{note}")
    print(f"{loud}/{len(chans)} channels loud")
    print("SIGNATURE OK" if ok else "SIGNATURE FAILED")
    sys.exit(0 if ok else 1)


main()
