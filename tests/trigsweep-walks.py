#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Walks for tests/trigsweep.sh: one track at a time, thinned to quarter notes.

    tests/trigsweep-walks.py OUTDIR

Writes OUTDIR/q<N>.jsonl for N = 1..8, and OUTDIR/q12.jsonl (T1 heard through
its Neighbor on T2). Each walk boots the trigsweep fixture, waits out LOADING,
enters grid record, turns off every step of track N except 1/5/9/13, leaves
grid record, mutes every other track, presses PLAY and records 40 s.

What the fixture has to satisfy (tests/fixtures/trigsweep, the user's
TESTDROPOUT project): every track trigs all 16 steps, and nothing is saved
muted. Both matter, because on the Octatrack:

  - a trig key in grid record TOGGLES the step, it does not clear it, and
  - FUNC + TRACKn TOGGLES the mute, it does not set it.

Quarter notes, not 16ths: the drop (bug-025 / bug-031) renders a note's block
with the previous record's gate word. At 16ths the previous note's gate is
still up and nothing is audible; it needs the gap before a quarter note.
Muted tracks keep trigging and their FX keep running, so a solo keeps the
DSP load of the whole project, which is what exposes the race.
"""
import os
import sys

HEAD = [
    '{"wait_text":"PTCH","timeout_ms":300000}',
    '{"wait_guest_ms":3000}',
    '{"wait_gone":"L0ADING","timeout_ms":300000}',
    '{"wait_guest_ms":2000}',
]
TAIL = [
    '{"wait_guest_ms":1000}',
    '{"mark":"play"}',
    '{"tap":"PLAY","until_lamp":1,"lit":1,"verify_ms":3000,"timeout_ms":60000}',
    '{"wait_guest_ms":40000}',
    '{"mark":"end"}',
]


def walk(thin, keep):
    w = list(HEAD)
    w += ['{"tap":"REC"}', '{"wait_guest_ms":500}',
          '{"tap":"TRACK%d"}' % thin, '{"wait_guest_ms":400}']
    for k in range(1, 17):
        if k % 4 != 1:
            w += ['{"tap":"TRIG%d"}' % k, '{"wait_guest_ms":150}']
    w += ['{"tap":"REC"}', '{"wait_guest_ms":500}',
          '{"press":"FUNC"}', '{"wait_guest_ms":300}']
    for t in range(1, 9):
        if t not in keep:
            w += ['{"tap":"TRACK%d"}' % t, '{"wait_guest_ms":300}']
    w += ['{"release":"FUNC"}']
    return '\n'.join(w + TAIL) + '\n'


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for n in range(1, 9):
        with open(os.path.join(out, 'q%d.jsonl' % n), 'w') as f:
            f.write(walk(n, {n}))
    with open(os.path.join(out, 'q12.jsonl'), 'w') as f:
        f.write(walk(1, {1, 2}))


if __name__ == '__main__':
    main()
