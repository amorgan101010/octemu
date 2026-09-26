# Plan: trace the lost FLEX-trig handoff between the two DSP cores

Status: done 2026-09-25, see PERF-NOTES "Traced (2026-09-25)". The switches are named OCTA_LONE_CORE /
OCTA_HANDOFF_TRACE (the board's OCTA_ convention), not OCTEMU_. Written 2026-09-25. Background: `docs/PERF-NOTES.md`, "Dropped FLEX trigs:
ROOT-CAUSED AND FIXED" (lines ~79-125) and the investigation history (lines ~318-406); `.wolf/buglog.json`
bug-021 (wrong theory) and bug-025 (real cause).

## Why

Commit `74930ca` fixed the every-8th silent FLEX trig by stepping both DSP56721 cores in 16-instruction
alternation everywhere (`stepPair` in `src/board/ot-dsp-shim.cc`). Result: 0 drops in 1680+ beats. The
mechanism behind it is inferred, not traced. The commit says the handoff for the voice's sustain "was lost
when a lone-core burst coincided with it", by analogy with the metronome click. No DSP address or PC for
the lost message has been identified.

A trace answers three questions:

1. Which handoff is lost: which words of the shared window (x:$30000..$30047) or mailbox (y:$ffffd3-d7)
   carry the voice-sustain message, and which PCs write and read them.
2. Whether the mechanism is exactly "core 0 overwrites the window before core 1 reads it", or something
   else that lone-core stepping also breaks.
3. How much margin `stepPair` leaves: the distance between core 1's read and core 0's overwrite, in
   instructions. That shows whether 16 is comfortably safe or only just, and whether any remaining
   lone-core path (for example the 250 ms `kDeliveryHold` escape) could still hit it.

The Monomachine emulator had the same class of bug; see "Prior art" at the end.

## Ground rules

- Read `.wolf/cerebrum.md` first, especially: judge with `work/trig8/batch.sh` (8 runs × 84 beats), never
  1-3 runs; build QEMU with JOBS=5-6; never leave experiments in the shared build; write commit messages
  with the Write tool.
- Every hook added here is opt-in through an environment variable. With nothing set, behaviour and output
  must be identical: run `make test-dsp test-dsp-metro test-emu` and a trig8 batch before handing back.
- Keep the trace windowed. A full 40 s trace of both cores will be enormous.

## Step 1: bring the bug back on demand

`stepPair` has no off switch. Add one, for example `OCTEMU_LONE_CORE=1`: `stepPair(c, n)` then does
`c.step(n)` only, which is the pre-`74930ca` behaviour at all four call sites. `ot_dspcore_write_burst`
previously also stepped the peer 64 (`o.step(64)`); check `git show 74930ca` and restore that exactly.

Confirm with the repro, which is deterministic with the old stepping at interleave 1024:

```sh
OCTEMU_LONE_CORE=1 OCTEMU_ARGS="--interleave 1024" tests/trig8-repro.sh lone
python3 tests/trig8-body.py out/trig8-lone.wav <play block>   # expect drops at beats [9, 17, 25, ...]
```

The play block comes from the log: `grep -ao '\[mark\] blk=[0-9]* [0-9]* play' out/trig8-lone.log`.
`work/trig8/batch.sh` shows how the scripts fit together. Also confirm that `OCTEMU_LONE_CORE` unset still
gives `drops at beats []`.

## Step 2: instrument, opt-in and windowed

The shim already advances the cores in slices, so most of this lives in `ot-dsp-shim.cc` and needs no
DSP-emulator change. Suggested switches: `OCTEMU_HANDOFF_TRACE=<file>`, with `..._FROM` / `..._TO` in
audio blocks so only a few beats around a drop are logged.

Log one line per event, with a common timebase. Use each core's instruction or cycle counter plus a global
step sequence number, since the cores have no shared clock in the emulator.

a. **Shared window changes.** After every slice (`kCoreSlice` in `stepRound`, and every `c.step` in the
   lone-core paths), diff x:$30000..$30047 against the previous snapshot. Log which core just ran, its PC,
   and the changed addresses with old and new values. That gives write resolution to one slice without a
   JIT write hook. If finer resolution is needed later, add a PC watch in `vendor/dsp56300`.
b. **Mailbox.** Core 0's post of the bank index (0/1, "that IS the block clock", then 2) and core 1's take
   of it: log core, PC, value and time. Find where the shim or DSP peripheral model sees y:$ffffd3-d7.
c. **Core 1's window read.** Core 1 reads the window three instructions after taking the mailbox word.
   Find that PC from `re/dsp.syms` or a disassembly of the core 1 program. Log each time core 1 reaches it,
   with the window contents it sees.
d. **Lone-core bursts.** Every time a path in step 1 advances one core alone: the call site
   (`drainStep` / write stall / `rx_pop` / `write_burst` / `read_burst`), which core, `n`, and the other
   core's PC.
e. **Per-block summary.** The block index, and whether this block's audio shows the trig (the trig8
   scorer's beat list is enough to label blocks).

## Step 3: capture

Run trig8 with `OCTEMU_LONE_CORE=1 OCTEMU_ARGS="--interleave 1024"`. Log about two seconds around beat 9:
at 120 BPM a beat is 1378.125 blocks, so beat 9 is about 11,025 blocks after the play mark. That covers
dropped beats 9 and 17 and played beats 8, 10 and 16. Capture the same window without `OCTEMU_LONE_CORE`
as the control.

## Step 4: analyse

For each block, reconstruct the handoff sequence:

1. core 0 writes the message into the window;
2. core 0 posts the mailbox word;
3. core 1 takes it;
4. core 1 reads the window;
5. core 0 overwrites the window.

Then answer:

- At dropped beats, does 5 come before 4? Which lone-core burst from step 2d sits between 2 and 4, and at
  which call site?
- Which window words differ between a trig block and a non-trig block? That identifies the sustain
  message and its fields.
- At played beats, and in the control run, what is the margin from 4 to 5 in core 0 instructions?
  Report its distribution over the whole capture. Compare it with 16 and with the largest lone-core burst
  (256).
- Is anything other than this handoff broken at the dropped beats? For example the mailbox value itself,
  or HREQ/host-port timing.

If 5-before-4 does not explain the drop, say so and follow the data: log core 1's full PC sequence for
the dropped block against a played one.

## Step 5: deliverables

- A section in `docs/PERF-NOTES.md` replacing "inferred" with the traced facts: addresses, PCs, the
  message fields, the sequence at a dropped beat, and the measured margin with `stepPair`.
- An audit of every remaining place a single core advances alone: grep the shim for `.step(` outside
  `stepPair` and `stepRound`, plus the `kDeliveryHold` escape. For each, say whether it can land between 2
  and 4, given the measured margin.
- The switches from steps 1-2 committed as opt-in diagnostics, with defaults bit-identical, or dropped if
  they're not worth keeping. Say which in the commit.
- Update `.wolf/buglog.json` bug-025 with the traced cause.

## Prior art: the same class of bug on the Monomachine (gearmulator MD/MM fork)

On 2026-09-25 the Monomachine's dropped track-6 notes turned out to be a cross-DSP handoff broken by
emulator skew. There are two separate DSP56303s. DSP1's once-per-block Port C sync edge reached DSP2 at
whatever cycle DSP2 happened to be at: about 2.8k cycles of spread (5th to 95th percentile). So DSP2's
replies, and the controller's frames that follow them, drifted into a window where DSP1 wipes a
trigger word unseen. The fix time-stamps that edge (PR joelanders/gearmulator-md-mm#98).

The tracing approach that worked there:
- log every event on both chips with its own cycle counter, and map both onto one timebase;
- find the handoff from the program side, using a disassembly of the waiting loop;
- compare a dropped block with its neighbours, measuring each link of the chain rather than guessing.

Write-up: `doc/mm-track6-drop-investigation.md` on branch `investigate/mm-track6-drops` of
`amorgan101010/gearmulator-md-mm` (local worktree
`~/Documents/md_mm_gearmulator/build/fork-mdmm-octfix`).
