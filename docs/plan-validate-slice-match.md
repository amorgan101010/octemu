# Plan: validate instruction-matched DSP stepping (bug-031) on macOS

Status: handed over from the Linux agent 2026-09-26; not started on macOS.
Branch: `diag/handoff-trace` (on `linux` + the handoff diagnostics).
Background: `docs/PERF-NOTES.md`, the "Traced (2026-09-25)" and "bug-031"
sections. Read both first.

## The short version

The shim lockstepped the two DSP cores by JIT-block count. The codec retires
about 36.6 instructions per JIT block and core 1 about 14-24, so core 1 ran at
roughly half the real chip's rate. Core 1's record-delivery DMA lands in
whichever bank core 1 took last (its 0x88 handler's mask is rewritten at each
take), so a slow core 1 lets the ColdFire's late record transfer land a block
early. Where that flips between a note-on and the next record, the note's
sustain is cut. `stepPair` (74930ca) fixed one project; the user's second
project still drops on T1 (bug-031).

`OCTA_SLICE_MATCH=1` gives the peer of each slice exactly the instructions
its partner just retired. The codec's pace is unchanged. On Linux it cleared
everything tested (see the bug-031 table). The job here is to decide whether
it can become the default.

## Ground rules

- The user moved this to the Mac so test load doesn't disturb their Linux
  machine. Keep runs polite anyway: `tests/trigsweep.sh` already uses `nice`,
  and `P=2` is its default parallelism.
- The user's real Octatrack is on permanent loan. **Do not ask for hardware
  verification.** Treat emulator/silicon disagreements as emulator bugs and
  settle them with traces and the firmware disassembly.
- Never play audio on the speakers without asking: every test here records
  headless (`--recording`).
- Write commit messages with a file, not a heredoc. Record results in
  PERF-NOTES under bug-031, replacing "still to do" items as you close them.
- Everything below is opt-in by environment variable; defaults must stay
  bit-for-bit today's behaviour until the switch is flipped deliberately.

## Step 0: build and reproduce

Build QEMU from this branch your usual way. Then:

```sh
tests/trigsweep.sh base                        # expect: q12 drops [9, 17, ... 81], q1 silent, rest clean
OCTA_SLICE_MATCH=1 tests/trigsweep.sh match    # expect: all clean (q1 silent)
OCTA_LONE_CORE=1 tests/trigsweep.sh lone       # expect: q3, q4, q12 drop (the pre-74930ca bug)
```

If macOS disagrees with the Linux results, stop and report it before going
on: the rest assumes the same behaviour.

## Step 1: regression gates with OCTA_SLICE_MATCH=1 exported

- `make test-dsp test-dsp-metro test-emu`. The first two don't use the shim;
  run them anyway.
- `make test-emu-audio` at least 3 times. It fails in clusters on unchanged
  binaries; read `ship_nz` in `out/trig/walk.log` as the recipe explains.
- trig8, 8 runs: `for i in 1 2 3 4 5 6 7 8; do tests/trig8-repro.sh m$i; done`.
  Expect `drops at beats []` for each; median body ~2233.
- `make fixtures` and the USB tests you normally run.

## Step 2: cost and calibration

- `scripts/bench.sh` A/B, with and without the switch. Core 1 now runs more
  instructions per slice, so expect some cost. Linux paid about 2% for
  stepPair; say what this costs on the Mac.
- Project-load time in guest seconds, method as in PERF-NOTES "0017 is
  back". The ColdFire:DSP ratio shouldn't move (the codec's slice is
  unchanged), but core 1's extra progress during codec holds could change
  load behaviour.
- Delivery-hold hatch count (`hatch=` in the exit stats) must stay 0 in
  steady playback.

## Step 3: the margin, on the right clock

Measure it rather than infer it from drops. The race is decided while the
codec is frozen holding its bank-index word, so use the guest-instruction
clock (`g=` on the trace's M and T lines), not the codec's.

```sh
W=201e,203e,205e,207e,401e,403e,405e,407e
OCTA_HANDOFF_TRACE=out/q12-base.trace OCTA_HANDOFF_WATCH=$W OCTA_HANDOFF_FROM=999999999 \
  OCTA_HANDOFF_PCS=fffff FX=tests/fixtures/trigsweep WALK=out/trigsweep-walks/q12.jsonl \
  tests/trig8-repro.sh q12-base
# again with OCTA_SLICE_MATCH=1, and for the trig8 fixture
python3 scripts/diag/handoff-flip.py out/q12-base.trace <play block>
python3 scripts/diag/handoff-margin.py out/q12-base.trace <play block> out/q12-match.trace <play block>
```

The play block is `[mark] blk=... play` in `out/trig8-<label>.log`. Report,
per track: the regime mix (every landing should be in-render), the minimum
`to next` in guest instructions, and how that minimum compares with one
interleave quantum (512 guest instructions). A matched run that is clean but
with a tiny minimum is not a fix, only a smaller window.

## Step 4: the combo sweep

bug-031 shows the machine/FX setup decides which tracks are exposed. Build
variants of the trigsweep fixture and run `tests/trigsweep.sh` under
shipped, `OCTA_SLICE_MATCH=1` and `OCTA_LONE_CORE=1` (the last as the
positive control: a variant where lone stepping never drops tests nothing).

- Tracks 5-8 run on the OTHER core (the codec, payload A). The user asked
  specifically whether a Neighbor or the T1/T3 FX (comb, spring reverb) on
  T5-T8 can expose them. Check: Neighbor on T6 fed by T5 with comb on T5 FX2,
  spring reverb on T7, and the same on T1-T4 for comparison.
- Then the other machine types (Static, Thru, Pickup) and other stock FX.

To generate variants, OctaBam (github.com/sambanks/octabam, MIT) has project
and bank writers: `tools/hw/ot_project.py` (`set_machine_type`, `set_fx`,
`set_pattern_trig`, checksum in `_bank_write`) and `tools/hw/ot_bank.py`.
Its format notes are in those docstrings. Caveats:

- Its default backup guard writes to a path on the author's machine; pass
  `guard=False`.
- It has no mute field and no Neighbor parameters.
- Some offsets are marked INFERRED.

Validate every generated project by loading it: the per-slot note-on counts
from `handoff-flip.py`, and audio from the sweep. If you copy code from
OctaBam, attribute it.

## Deliverables

- The bug-031 section of PERF-NOTES updated with each step's results.
- A recommendation: make `OCTA_SLICE_MATCH` the default (and how: a constant
  plus a comment in `ot-dsp-shim.cc` explaining the unit), keep
  investigating, or reject it, with the evidence.
- Any new failing combination written up with a `tests/trigsweep.sh` case or
  fixture that reproduces it.
