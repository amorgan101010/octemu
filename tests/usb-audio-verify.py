#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prove the USB-audio iso stream is real, continuous, guest-produced audio.

WHAT THE STREAM IS. The payload's producer runs in the firmware's own per-block
frame ISR and sums the post-FX readback arena (0x80003190 + prev*1024 +
track*128), which the eDMA fills every block on real hardware exactly as it
does in the emulator. So the stream is the SUMMED POST-FX, PRE-FADER TRACK BUS.
It is NOT the MAIN output: track level, the crossfader and MAIN volume are
downstream of it, and the master path is applied inside the DSP where no CPU
can read it. `--recording` still captures MAIN, so the two are related signals,
not identical ones, and this no longer asserts a sample-exact match.

WHAT IS ASSERTED, and why each one can fail:

  1. the capture carries real audio (not silence, which would pass anything);
  2. its dominant frequency is the fixture's tone, so it is THIS machine's
     audio and not noise;
  3. it is CONTINUOUS through the sustained part of the burst — no dropped or
     duplicated run of frames. A gap in a sine shows up as a sample-to-sample
     jump far larger than the waveform's own maximum slope, which is what the
     producer/consumer cursor logic would get wrong if it got anything wrong;
  4. its level tracks the recording's within a factor-of-two band, which is
     what catches a wrong RB_SHIFT calibration.

A POSITIVE CONTROL runs every time: a run of frames is spliced out of the
capture and the continuity test must then FAIL. A test that has never been
seen to fail proves nothing.

☠ KNOWN BLIND SPOT, found by that control: against a pure tone the continuity
test cannot see a gap whose length is a multiple of the tone's period, because
the waveform reconnects in phase (at 440 Hz / 44.1 kHz that is every ~100
frames). Total-frame accounting is covered by the cadence gate (P3), not here;
this test covers the CONTENT of what was delivered. The control therefore
splices half a period, where the discontinuity is largest.

  tests/usb-audio-verify.py REF.wav ISO.pcm [--tone 440]
"""
import math
import struct
import sys


def wav_pcm(path):
    b = open(path, "rb").read()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        sys.exit(f"{path}: not a RIFF/WAVE file")
    i = 12
    while i + 8 <= len(b):
        cid, sz = b[i:i + 4], struct.unpack("<I", b[i + 4:i + 8])[0]
        if cid == b"data":
            return b[i + 8:i + 8 + sz]
        i += 8 + sz
    sys.exit(f"{path}: no data chunk")


def frames(pcm):
    n = len(pcm) // 4
    return [struct.unpack_from("<hh", pcm, 4 * i) for i in range(n)]


def peak(fr):
    return max((max(abs(l), abs(r)) for l, r in fr), default=0)


def dom_freq(fr, rate=44100):
    """Zero-crossing estimate on the loud part — no numpy needed."""
    pk = peak(fr)
    loud = [l for l, _ in fr if True]
    gate = pk / 4.0
    # count rising zero crossings inside the sustained region only
    xs = [i for i in range(1, len(loud))
          if loud[i - 1] < 0 <= loud[i] and max(abs(loud[i - 1]), abs(loud[i])) > 0]
    span = [i for i, v in enumerate(loud) if abs(v) > gate]
    if len(span) < 2 or len(xs) < 3:
        return 0.0
    lo, hi = span[0], span[-1]
    xs = [x for x in xs if lo <= x <= hi]
    if len(xs) < 3:
        return 0.0
    return rate * (len(xs) - 1) / float(xs[-1] - xs[0])


def continuity_breaks(fr, tone, rate=44100):
    """Sample-to-sample jumps larger than a sine of this tone can produce.

    Applied only where both neighbours are in the sustained part of the burst,
    so a legitimate note onset or release is not counted.
    """
    pk = peak(fr)
    if pk == 0:
        return -1, 0.0
    gate = pk / 4.0
    # max slope of A*sin(2*pi*f*t) sampled at `rate`, with generous margin
    bound = pk * 2 * math.pi * tone / rate * 3.0
    breaks, worst = 0, 0.0
    for i in range(1, len(fr)):
        a, b = fr[i - 1][0], fr[i][0]
        if abs(a) < gate or abs(b) < gate:
            continue
        d = abs(b - a)
        worst = max(worst, d)
        if d > bound:
            breaks += 1
    return breaks, bound


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    ref_path, iso_path = sys.argv[1], sys.argv[2]
    tone = 440.0
    if "--tone" in sys.argv:
        tone = float(sys.argv[sys.argv.index("--tone") + 1])

    iso = frames(open(iso_path, "rb").read())
    ref = frames(wav_pcm(ref_path))
    ok = True

    # 1. real audio
    ipk, rpk = peak(iso), peak(ref)
    loud = sum(1 for l, r in iso if abs(l) > 64 or abs(r) > 64)
    print(f"capture: {len(iso)} frames, peak {ipk}, {loud} frames with audio")
    print(f"recording (MAIN): {len(ref)} frames, peak {rpk}")
    if loud < 2000:
        print("  FAIL: capture is effectively silent")
        ok = False

    # 2. it is this machine's tone
    f = dom_freq(iso)
    if abs(f - tone) > tone * 0.05:
        print(f"  FAIL: dominant frequency {f:.1f} Hz, expected {tone:.0f} Hz")
        ok = False
    else:
        print(f"  ok: dominant frequency {f:.1f} Hz (expected {tone:.0f})")

    # 3. continuity
    breaks, bound = continuity_breaks(iso, tone)
    if breaks:
        print(f"  FAIL: {breaks} discontinuities (jump > {bound:.0f} LSB)")
        ok = False
    else:
        print(f"  ok: continuous, no jump over {bound:.0f} LSB in the sustain")

    # 4. level tracks the recording (catches a mis-calibrated RB_SHIFT)
    if rpk == 0:
        print("  FAIL: the recording is empty — the level check cannot run")
        ok = False
    else:
        ratio = ipk / float(rpk)
        if not 0.5 <= ratio <= 2.0:
            print(f"  FAIL: level ratio to MAIN {ratio:.3f} outside 0.5..2.0 "
                  f"— RB_SHIFT is probably mis-calibrated")
            ok = False
        else:
            print(f"  ok: level ratio to MAIN {ratio:.3f} (track bus vs MAIN)")

    # positive control: splice a run out and the continuity test MUST fail
    # ☠ Splice INSIDE the sustained burst. Cutting at the midpoint of the
    # capture lands in silence, where the continuity test correctly skips —
    # and a control that removes frames nothing looks at proves nothing.
    gate = ipk / 2.0
    loud_ix = [i for i, (l, _) in enumerate(iso) if abs(l) > gate]
    cut = list(iso)
    if len(loud_ix) > 200:
        at = loud_ix[len(loud_ix) // 2]
        del cut[at:at + 50]        # ~half a period: worst-case phase jump         # ~half a period: worst-case phase jump
    cbreaks, _ = continuity_breaks(cut, tone)
    if cbreaks == 0:
        print("  FAIL: positive control — a 50-frame splice went undetected, "
              "so the continuity test proves nothing")
        ok = False
    else:
        print(f"  ok: positive control, spliced capture shows {cbreaks} breaks")

    print("USB-AUDIO STREAM OK" if ok else "USB-AUDIO STREAM FAILED")
    sys.exit(0 if ok else 1)


main()
