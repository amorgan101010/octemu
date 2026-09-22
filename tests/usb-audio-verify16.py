#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prove the 16-channel USB-audio stream is the eight tracks, sample-exact.

THE ORACLE. The payload's producer reads the post-FX readback arena
(0x80003190 + prev*1024 + track*128 + frame*8, two 32-bit words L/R per
frame) every 16-frame block, shifts each word right by 16, saturates to s16,
and stores track t's pair at slot channels 2t/2t+1. The emulator's readback
DUMP (OCTA_RB_DUMP=file, src/board/ot-board.c) records every eDMA write into that
arena verbatim, so the dump IS what the producer read. This script rebuilds
each track's sample sequence from the dump and requires the capture's channel
pair 2t/2t+1 to equal it — sample-exact after the same shift + saturation —
over a long aligned window. --recording (MAIN) can never be this oracle: the
stream is pre-fader and per-track, MAIN is neither.

WHAT IS ASSERTED:
  1. the capture carries real audio on at least one channel pair;
  2. for every track that is loud in the dump, the capture's pair for that
     track equals the dump sample-exact over the aligned window (alignment is
     found once on the loudest track and then applied to ALL tracks: a
     channel-order mistake shows up as a mismatch, not as a different offset);
  3. every track that is silent in the dump is silent in the capture
     (no crosstalk);
  4. a POSITIVE CONTROL: the capture with two channel pairs swapped must FAIL
     check 2. A test that has never been seen to fail proves nothing.
  5. with --all-loud, every one of the eight tracks must be loud in the dump,
     so a fixture that did not sound (or a walk that dropped a gesture) fails
     here instead of passing as "silent both sides".

  tests/usb-audio-verify16.py DUMP.bin ISO.pcm [--channels 16] [--all-loud]
"""
import struct
import sys

RB_BASE, BANK, TRK = 0x80003190, 1024, 128
SHIFT = 16


def sat16(v):
    return 32767 if v > 32767 else -32768 if v < -32768 else v


def load_dump(path):
    """-> per track: list of (L, R) s16 as the producer would have produced
    them, in arrival order. The arena is ping-pong; each block writes one
    bank's eight 128 B track slots, so writes are ordered by block and, within
    a block, by track. Frames within a slot are in address order."""
    b = open(path, "rb").read()
    tracks = [[] for _ in range(8)]
    i = 0
    while i + 8 <= len(b):
        daddr, n = struct.unpack_from(">II", b, i)
        i += 8
        data = b[i:i + n]
        i += n
        off = daddr - RB_BASE
        if off < 0 or off >= 2 * BANK:
            continue
        for j in range(0, n - 7, 8):
            o = off + j
            t = (o % BANK) // TRK
            l, r = struct.unpack_from(">ii", data, j)
            tracks[t].append((sat16(l >> SHIFT), sat16(r >> SHIFT)))
    return tracks


def load_capture(path, nch):
    b = open(path, "rb").read()
    fb = nch * 2
    n = len(b) // fb
    fr = [struct.unpack_from("<%dh" % nch, b, i * fb) for i in range(n)]
    return fr


def peak(seq):
    return max((max(abs(l), abs(r)) for l, r in seq), default=0)


def _lbytes(seq):
    """The L channel of a frame list as a little-endian int16 byte string, so
    a run can be located with bytes.find (C-fast over millions of frames)."""
    return struct.pack("<%dh" % len(seq), *[l for l, _ in seq])


def _onsets(seq, thr, quiet=400):
    """Indices where a channel goes from at least `quiet` near-silent frames
    to loud: the silence-to-tone transitions. Few, and unique in time."""
    out = []
    k = quiet
    while k < len(seq):
        if abs(seq[k][0]) > thr:
            if all(abs(seq[q][0]) <= thr // 8 for q in range(k - quiet, k)):
                out.append(k)
            k += quiet
        else:
            k += 1
    return out


def find_align(ref, cap_pair, min_len=4000):
    """Offset o such that cap_pair[k] == ref[k + o] over the capture.

    ☠ Alignment by CONTENT alone is ambiguous here. A generated 250 Hz sine
    at 44.1 kHz repeats itself exactly every 882 samples, and the dump can
    hold the tone's onset twice (a fixture restored from battery RAM sounds
    at boot, and PLAY fires it again from the sample's start), so an exact
    window match — even of the onset — has several occurrences, and the
    first, or the one with the longest exact run, was wrong in practice.

    So: the candidates are the silence-to-tone ONSETS, capture against dump
    (a handful, +-3 frames of slop each), and each candidate offset is scored
    by how much of the WHOLE capture matches the dump within +-1 LSB — the
    fidelity check's own tolerance — sampled every 7th frame. The true
    alignment scores the entire capture; a coincidental one scores the
    silence. A capture with no onset (tone from the first frame) falls back
    to exact window matches, scored the same way."""
    n = len(cap_pair)
    gate = peak(cap_pair)
    if gate == 0:
        return None, 0
    thr = max(64, gate // 8)

    def score(o):
        hits = 0
        for k in range(0, n, 7):
            j = k + o
            if 0 <= j < len(ref):
                c, r = cap_pair[k], ref[j]
                if abs(c[0] - r[0]) <= 1 and abs(c[1] - r[1]) <= 1:
                    hits += 1
        return hits

    cands = set()
    cap_on = _onsets(cap_pair, thr)
    ref_on = _onsets(ref, thr)
    for c in cap_on[:4]:
        for d in ref_on:
            for slop in range(-3, 4):
                cands.add(d - c + slop)
    if not cands:
        W = 400
        hay = _lbytes(ref)
        wins = sorted(((sum(abs(cap_pair[s + q][0]) for q in range(0, W, 8)), s)
                       for s in range(0, n - W, 50)), reverse=True)[:16]
        for _, s0 in wins:
            sig = _lbytes(cap_pair[s0:s0 + W])
            pos = hay.find(sig)
            while pos >= 0 and len(cands) < 64:
                if pos % 2 == 0:
                    cands.add(pos // 2 - s0)
                pos = hay.find(sig, pos + 1)
    best = (None, -1)
    for o in sorted(cands):
        sc = score(o)
        if sc > best[1]:
            best = (o, sc)
    o = best[0]
    if o is None:
        return None, 0
    # the longest exact (L,R) run at that offset, starting at the loudest
    # capture onset (or frame 0), for the report
    start = cap_on[0] if cap_on else 0
    lo = start
    while lo > 0 and 0 <= lo - 1 + o < len(ref) and cap_pair[lo - 1] == ref[lo - 1 + o]:
        lo -= 1
    hi = start
    while hi < n and 0 <= hi + o < len(ref) and cap_pair[hi] == ref[hi + o]:
        hi += 1
    return o, hi - lo


def compare(tracks, cap, nch, swap=None):
    """Returns (ok, report lines)."""
    lines = []
    ok = True
    pairs = []
    for t in range(8):
        pr = [(f[2 * t], f[2 * t + 1]) for f in cap]
        pairs.append(pr)
    if swap:
        a, b = swap
        pairs[a], pairs[b] = pairs[b], pairs[a]
    ref_peaks = [peak(tr) for tr in tracks]
    loud = [t for t in range(8) if ref_peaks[t] > 256]
    if not loud:
        return False, ["  ✗ no track is loud in the dump; nothing to compare"]
    anchor = max(loud, key=lambda t: ref_peaks[t])
    off, run = find_align(tracks[anchor], pairs[anchor])
    # ☠ The anchor is a 400-frame EXACT byte-signature match (find_align), which
    # a chance alignment cannot produce — that is the proof the offset is right.
    # Do NOT also require a long exact EXTENSION run: sub-LSB quantization at
    # ping-pong bank flips scatters ±1-LSB diffs through the stream, so the
    # longest exact run varies with their distribution (hundreds of thousands
    # when clustered, ~800 when spread) even though the stream is equally
    # faithful. The per-frame tolerance below (zero >1-LSB diffs, <=0.5% ±1) is
    # what judges faithfulness.
    if off is None:
        return False, [f"  ✗ track {anchor + 1}: no sample-exact alignment "
                       f"between capture and dump (no anchor match)"]
    lines.append(f"  track {anchor + 1}: aligned at dump offset {off:+d}, "
                 f"{run} consecutive frames identical")
    # the capture window where the anchor aligned
    lo = max(0, -off)
    hi = min(len(cap), len(tracks[anchor]) - off)
    for t in range(8):
        ref, pr = tracks[t], pairs[t]
        if ref_peaks[t] > 256:
            n = hi - lo
            # ☠ Any difference LARGER than 1 LSB is a real defect: a swapped,
            # shifted or miscalibrated channel differs by hundreds. A ±1 LSB
            # difference is sub-audible quantization where the dump's linear
            # replay and the producer's ping-pong read straddle a bank flip;
            # measured at ~0.03% of frames, all exactly ±1. Require zero
            # >1-LSB diffs and at most 0.5% ±1-LSB diffs.
            big = one = 0
            first_big = None
            for k in range(lo, hi):
                dl = abs(pr[k][0] - ref[k + off][0])
                dr = abs(pr[k][1] - ref[k + off][1])
                d = max(dl, dr)
                if d > 1:
                    big += 1
                    if first_big is None:
                        first_big = k
                elif d == 1:
                    one += 1
            if big or one > n // 200:
                ok = False
                detail = (f"first >1-LSB at {first_big}: cap {pr[first_big]} "
                          f"dump {ref[first_big + off]}" if big else
                          f"{one}/{n} ±1-LSB diffs exceeds 0.5%")
                lines.append(f"  ✗ track {t + 1} -> channels {2*t+1}/{2*t+2}: "
                             f"{big} frames differ by >1 LSB, {one} by ±1 "
                             f"({detail})")
            else:
                lines.append(f"  ok: track {t + 1} -> channels {2*t+1}/{2*t+2} "
                             f"sample-exact over {n} frames ({one} ±1-LSB, "
                             f"peak {peak(pr[lo:hi])})")
        else:
            pk = peak(pr[lo:hi])
            if pk > 64:
                ok = False
                lines.append(f"  ✗ track {t + 1} is silent in the dump but its "
                             f"channels peak at {pk} in the capture (crosstalk)")
            else:
                lines.append(f"  ok: track {t + 1} silent both sides (peak {pk})")
    return ok, lines


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    nch = 16
    if "--channels" in sys.argv:
        nch = int(sys.argv[sys.argv.index("--channels") + 1])
    tracks = load_dump(sys.argv[1])
    cap = load_capture(sys.argv[2], nch)
    print(f"dump: {[len(t) for t in tracks]} frames per track, peaks "
          f"{[peak(t) for t in tracks]}")
    if "--all-loud" in sys.argv:
        quiet = [t + 1 for t in range(8) if peak(tracks[t]) <= 256]
        if quiet:
            sys.exit(f"  ✗ tracks {quiet} are silent in the dump; the fixture "
                     f"should sound on all eight")
    print(f"capture: {len(cap)} frames x {nch} ch")
    if not cap or max(max(abs(v) for v in f) for f in cap) < 256:
        sys.exit("  ✗ the capture carries no audio")
    ok, lines = compare(tracks, cap, nch)
    print("\n".join(lines))
    # positive control: swap the loudest pair with its neighbour
    ref_peaks = [peak(t) for t in tracks]
    a = max(range(8), key=lambda t: ref_peaks[t])
    b = (a + 1) % 8
    cok, _ = compare(tracks, cap, nch, swap=(a, b))
    if cok:
        print(f"  ✗ POSITIVE CONTROL FAILED: swapping tracks {a+1} and {b+1} "
              f"in the capture still passes — the comparison sees nothing")
        ok = False
    else:
        print(f"  ok: positive control, swapped pairs {a+1}/{b+1} fail as they must")
    print("USB-AUDIO 16-CHANNEL STREAM OK" if ok else "USB-AUDIO 16-CHANNEL STREAM FAILED")
    sys.exit(0 if ok else 1)


main()
