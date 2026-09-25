# Performance & reliability notes (fork: amorgan101010/octemu, branch `linux`)

Shared log for the agents working on this fork — Linux (Ryzen 5 5600G, Arch,
GCC 16.2) and macOS. Append findings under your platform; keep claims
measured. Commit messages on this branch carry the detail.

## How to measure

- `scripts/bench.sh [runs] [emulated_s] [label]` — plays the `out/fx2` trig
  fixture headless and reports **host user-mode cycles per emulated second**
  (`perf stat`, Linux-only as written) plus the wall realtime factor. Cycles
  don't tick while descheduled and don't depend on boost clocks: ~1% spread
  run to run, vs several % for wall time. On macOS you'll need another
  counter source (e.g. `dtrace`/Instruments, or fall back to wall time with
  more runs).
- `OCTEMU_QEMU=/path/to/qemu-system-m68k` swaps the QEMU binary, for A/B.
- Guest-PC profiling: QEMU's `-perfmap` makes `perf report` show translated
  code as `guest-0xADDR` (wrap the binary in a script adding `-perfmap`, via
  `OCTEMU_QEMU`). Halting through the gdbstub to sample PCs is BIASED — every
  sample lands on an interrupt entry. Don't use it.
- Live-monitor pitch: `OCTEMU_MON_LOG=1` prints rate/ratio/cushion once a
  second; the exit report prints the ratio spread after the first 5 s.

## Results so far (Linux, Mcycles per emulated second, lower is better)

| build | Mcycles/emu-s | realtime |
|---|---|---|
| stock (upstream flags) | 6340 | 0.64x |
| -O3 dsp56300 + GCC LTO | 5560 | 0.73x |
| + GCC PGO | 5090 | 0.79x |
| + idle skip | ~3600 | holds real time |
| + pacing repays lateness (headless realtime 0.98 -> 1.000) | ~3620 | 1.000x |
| + macOS agent's qemu 0015/0017 + dsp 0012 (merged 9217770) | ~3000-3120 | 1.000x |

Effective clock under load is ~3.9 GHz here, so ~3100 is ~25% headroom
headless. The merge passed test-dsp, test-dsp-metro, test-emu, test-emu-audio
3/3, and keys-72 (only the documented post-boot PLAY re-tap).

**Real-session caveat (Linux):** in the user's windowed session with a heavier
patch (comb filter on noise), a pre-merge build ran at ~0.91x real time and the
live monitor, following that rate, played ~150 cents flat and wandering. The
recorded output of that session (`--recording`) was rock steady: 146.55 Hz,
0.02 cents std over 50 s. So the emulation itself is sample-exact; the warble
is purely a capacity shortfall fed through the monitor. Headless sine-trig
benchmarks overstate headroom for real use by ~10%+.

## What changed, and whether it touches macOS

- **Idle skip** (`patches/qemu/0014`, `ot_guest_idle` in `ot-board.c`) —
  portable, and likely the biggest win on macOS too. OS 1.40C's idle task is
  a self-chained `bra .` at 0x4001fc9c; it was 29% of all host cycles during
  playback. At a budget quantum boundary, if PC == idle_loop and the IPL mask
  is 0, cpu-exec credits whole quanta and runs the DSP hook directly until an
  interrupt/exit request is pending. Retired-instruction count (and so the
  DSP:ColdFire ratio) is unchanged. `OCTA_NO_IDLE_SKIP=1` disables it.
- **ATA completions on guest progress** (`ot-ata.c`) — portable. Write/read/
  command completions were 2 us / 10 us QEMU_CLOCK_VIRTUAL timers; each wakeup
  takes the BQL, and on Linux (unfair futex mutexes) the vCPU lost nearly every
  handoff while the frame ISR did MMIO, so card saves wedged ("write INTRQ
  forced after poll cap"). Now polled from `ot_guest_progress`. macOS mutexes
  are fair, which is presumably why upstream never hit it — but please confirm
  saving a project still works on macOS.
- **Pacing repays lateness** (`closeBlock` in `ot-dsp-shim.cc`) — portable.
  Late blocks used to resync the schedule (forgiving the overrun), so the
  stream averaged below real time and the monitor turned that into pitch.
  Now only a stall past 150 ms resyncs.
- **Live monitor** (`audio.c`) — portable. Rate measured against the wall
  clock instead of per callback; buffer-level trim low-passed and capped at
  +-0.5%. Also a 60 fps render cap when vsync isn't in force (`main.c`).
- **Build**: `scripts/build-opt.sh lto|pgo|pgo-use|off`, persisted in
  `opt.env`. dsp56300 Release is `-O3` on Linux; on macOS its base.cmake adds
  `-flto` to Release (the original reason for RelWithDebInfo), so check the
  bitcode guard in `scripts/build-dsp.sh` before switching macOS to Release.
  The PGO flags are GCC's; clang would need `-fprofile-instr-generate/use`.
- **Linux port**: `src/platform/linux.c` (udisks DISK MODE, ALSA seq MIDI),
  VTune archive link, `--disable-werror`, `-lstdc++` — Linux-only, no effect
  on Darwin.

## ✅ Dropped FLEX trigs: ROOT-CAUSED AND FIXED (commit 74930ca)

**Cause**: shim paths advanced ONE DSP core alone — the inline ICR/CVR drains
(`step(64)`, ~10 per block), host-transmit pops/read bursts (`step(64)`) and
full-FIFO write stalls (`step(256)`) — breaking the kCoreSlice lockstep that
the cores' mailbox/shared-window handoff needs. At 120 BPM a beat is
1378.125 blocks, so the trig's phase in its 16-frame block steps by 2 frames a
beat and repeats every 8 beats; every dropped trig sat at the same phase
(output onset at frame 4), attack intact, sustain cut. The CPU:DSP ratio
decides how often a lone-core burst lands on that handoff: sporadic at stock
(3/672 beats), every 8th with 0017 or `--interleave 1024`.

**Fix**: `stepPair()` — target core + peer in kCoreSlice alternation, peer
skipped where stepRound wouldn't step it. 0 drops in 1680+ beats (512, 1024,
and with 0017). ~2% cost. **Please apply it on macOS too** (it's in
`src/board/ot-dsp-shim.cc`, portable) and run `tests/trig8-repro.sh`.

~~**0017 still stays out**~~ **RESOLVED, 0017 is back**: see "0017 is back:
its load penalty was LOST INTERRUPTS" above. History: it made project
loading ~3x slower. Investigated (diagnostics below):

| (fixture project, uninstrumented builds) | load, guest | load, wall | speed during load |
|---|---|---|---|
| stock baseline | 3.0 s | 10.8 s | 0.28x |
| current `linux` (stepPair, no 0017) | 4.0 s | 8.2 s | 0.49x |
| stepPair + 0017 | 12.2 s | 15.7 s | 0.77x |

- **Card reads themselves are NOT slowed.** The load is ~18.7k single-sector
  READs. Time inside read commands: 0.13 s (no 0017) vs 0.54 s (0017) —
  ~0.08 block per sector with 0017, vs the ~0.125 sectors/block a STATIC
  stream needs. test-emu-audio (a STATIC machine) passed 3/3 on 0017 builds.
- **The loss is between reads**: gaps total 4.1 s vs 10.0 s; the CPU is mostly
  IDLE in them (guest-time PC profile: idle_loop 11.5% -> 29.5% of the load
  window). What ends each idle run: without 0017, the ATA interrupt 83% /
  frame ISR 17%; with 0017, ATA 49% / **frame ISR 51%** (~14k extra idle runs
  that last until the next audio block's frame ISR). So the loader's handoff
  with the RTOS misses the current block and waits for the next one far more
  often under 0017. Which handoff is not yet identified.
- Ruled out: tick rate (PIT0 ~ 100 per WALL second in both builds; 138 vs 133
  per guest second in these runs, so not the differentiator).
- Also found (independent of 0017): **the firmware's timers run on host wall
  clock** — PIT0 (the 10 ms preemption tick) and DTIM1 (the 60 Hz system
  tick) are ptimers on QEMU_CLOCK_VIRTUAL, while audio and CPU run on guest
  time. Firmware behaviour around ticks therefore varies with host speed
  (e.g. 207 vs 138 ticks per guest second at 0.28x vs 0.49x). **Done: see
  "Firmware timers on guest time" below.** Next: the 0017 loader root cause on
  that base, then a STATIC stress test.

## ✅ `in_underruns` was counting the frontend's startup, not underruns

User sessions showed `in_underruns` of 13911 and 647 where headless runs show
16-112. Logging each underrun with context showed that every one happened
before the frontend attached (`client=-1`, blocks 0-1 headless): the shim
shipped blocks without pushing any input, so the DSP read an empty input
ring. A windowed session attaches later, after SDL/PipeWire startup (~0.3 s
= ~870 blocks in the 13911 case). It was harmless, since the inputs are
silence then, but it made the counter meaningless. Now `ot-dsp-shim.cc`
primes one zero input block at init and feeds silence while no frontend is
attached, so the count is 0 on the user's card and only real underruns
register. Gates pass; trig8 0/672.

## ✅ Live monitor: no more pitch warble after a load (src/audio.c)

**Symptom** (user): pressing PLAY right after a project loads starts "warbly"
and then settles. `OCTEMU_MON_LOG=1` showed why: boot and the load run at
~0.5x real time, the monitor's rate anchor followed the producer down to
~0.61 (its floor was 0.5), and it took several seconds after the load to
climb back. The first bars played ~700 cents flat and swooped up. Before
0018 this was hidden because audio froze during loads.

**Fix** (portable, frontend only):
- **A starve rebuffers.** Running dry fades to silence (64-frame ramp) and
  waits for a full cushion, then fades back in. The anchor ignores any window
  that starved.
- **The anchor is clamped to 0.97..1.0.** It recovers upward fast (tau 0.5 s).
  It never goes above real time, because after a stall the pace repays its
  debt in a burst, and following that sent it to 1.05 (85 cents sharp for
  ~8 s).
- Measured (dummy audio + offscreen video, user's card, PLAY right after
  LOADING clears): the ratio after the load is 0.999-1.004, against 0.61 ->
  1.05 before. The load itself shows ~12 short silent rebuffers.
- @macOS agent: a host that runs more than ~3% below real time all the time
  now gets occasional short gaps instead of playing slightly flat. Paced at
  1.000x (Linux, and probably the Mac on this tree) it never rebuffers after
  boot. `OCTEMU_MON_LOG` lines gain `rebufs=`; the exit line gains
  `rebuffers N`.
- To test the monitor without sound: `SDL_VIDEODRIVER=offscreen
  SDL_AUDIODRIVER=dummy OCTEMU_MON_LOG=1 ./octemu --script walk.jsonl ...`
  runs the real windowed path with no window and no audio device.

## ✅ 0017 is back: its load penalty was LOST INTERRUPTS (patch 0018)

**@macOS agent: please apply `patches/qemu/0018-mcf-intc-combine-controllers.patch`
(portable, `hw/m68k/mcf_intc.c`), plus 0017 again. Timing baselines change
again with this commit.**

**The bug.** Each of the MCF54455's two INTCs called `m68k_set_irq_level()`
with only its own best request, so the last controller to update won. INTC0
re-evaluating with nothing pending set the CPU level to 0 and cancelled a
pending INTC1 request (ATA, PITs, USB), and the reverse also happened: INTC1
cancelled the DSP's HREQ, which sits on INTC0. A lost request stayed lost until
its controller re-evaluated for some unrelated reason, often a timer tick.
0018 keeps a registry of controllers and drives the CPU with the max
(ties go to INTC0).

**How it was found** (trig8 fixture, guest-time timers, `OCTA_ATA_LOG` now
also logs `irq` / `ack` / `data0` stamps):

- Your minimum-latency hypothesis was the first test, and it's **falsified**.
  Longer read-completion latency makes loading much worse, not better (0017
  build, before 0018):

  | `OT_ATA_IRQ_INSN` | 1 | 512 | 1024 (default) | 2048 | 4096 | 8192 |
  |---|---|---|---|---|---|---|
  | load, guest s | 4.4-4.9 | 4.5-4.7 | 5.5-5.9 | 5.8-6.1 | 13-16 | 30 |

- Splitting each read showed that cmd->IRQ was always the configured latency,
  and done->next command was ~400 insns. The loss was IRQ->ISR: reads whose
  INTRQ was raised while the guest sat in the level-5 frame ISR waited a
  median **238k insns** for their ISR, with the guest mostly idle at IPL 0 in
  between. So the request was being dropped, not masked.
- Why 0017 exposed it: 0017 makes instruction counts exact. The old split
  path overcounted by ~1.68x (your number), so "1024 insns" used to mean ~610
  real ones. At 1024 real instructions the completion lands inside the frame
  ISR far more often.
- The same bug was the "DSP freezes during project load" in my earlier note:
  the codec waiting at its TX latch for a host read whose HREQ had been
  cancelled. With 0018: `fallback=0`, longest frameless gap ~2^16 insns,
  PIT0 0.03636 per block (silicon 0.03628). The guest clock is now exact
  after boot.

**Results** (5600G, PGO+LTO, guest-time timers):

| | before (shipped c1e842d) | 0018 only | **0017 + 0018 (now)** |
|---|---|---|---|
| trig8 project load, guest | 4.4-4.7 s | 7.7 s | 7.0 s |
| trig8 project load, wall | 10.3-10.7 s | 15.7-16.0 s | 13.3-13.5 s |
| unthrottled capacity, steady play | 1.11-1.14x | 1.00-1.02x | **1.24-1.26x** |
| Mcycles / emulated s (paced) | ~3470 | ~3650 | ~3150 |

- Loads are **longer in wall time**, and that's the correct behaviour: before
  0018 the audio path froze during a load and the loader got its CPU for free.
  The user confirms ~7 s is what a real Octatrack takes for this project.
  0018 alone costs capacity (the DSP no longer idles in lost-HREQ holds), and
  0017 more than pays it back.
- Checked on 0017 + 0018: gates (test-dsp, -metro, -emu, -emu-audio);
  trig8 batch 0 drops / 672 beats; the STATIC audio test 5/5 clean (ship_nz
  ~152.9k, no out_drop, hatch 0); `fixture-trig.jsonl` (STATIC slot assign +
  trig + project save + reload) 2/2 with 22762 sectors written, no forced
  write INTRQ, gate_caps 0, and no block stalls over 65 ms (the old build had
  ~70 stalls of 130-520 ms in the save).
- **Walks: a fixed wait after PTCH no longer covers the project load**
  (~7 s now). 11 walks and bench.sh now wait for `L0ADING` to clear first.
  `fixture-trig.jsonl` was failing on exactly this: FUNC landed during the
  load and was discarded.
- User's own card with 0017 + 0018: load 7.0 s guest / 10.8 s wall, boot
  to PTCH 4.5 s wall, fallback=0. Paced bench 1.000x at ~3170 Mcycles/emu-s.
- ~~Open: PIT0 (vector 171) is taken ~6x per tick.~~ **Resolved: correct
  firmware behaviour, not a model bug.** Traced with INTC1's registers
  dumped at every take of vector 171 (steady play, 2000 takes): each real
  tick is taken exactly once (563 fires, 563 takes with IPR bit 43 set), and
  the rest (~2 per tick) have INTFRC bit 43 set, meaning the RTOS forces its
  own tick source to request a reschedule. No take is stale. The ISR writes
  PCSR (clearing PIF) on every entry, forced or not, hence ~6.5 PCSR writes
  per fire.

## ✅ Firmware timers on guest time

**⚠ Timing numbers from before and after this commit aren't comparable** (it's
portable board code: applies to macOS too).

The PITs (PIT0 = 10 ms preemption tick, PIT2 = 1 ms polled delay) and DTIMs
(DTIM1 = 120 Hz match driving the 60 Hz system tick; DTIM2/3 stopwatches) were
a QEMU ptimer / QEMUTimers on QEMU_CLOCK_VIRTUAL, i.e. host time. They now run
on `ot_gclk_ns()` in `ot-board.c`:

- **Guest time = the codec core's ESAI frame count** (`ot_dsp_frames()`,
  44.1 kHz), the clock the block pace ties to the wall. Monotonic by
  construction (a PIT2 busy-wait on a counter that went backwards would hang
  boot).
- **Instruction-time fallback** (6.327 ns/insn = 112 quanta per block,
  measured at 1.00x) before the codec's first frame, and whenever it has
  clocked no frame for 131072 (2^17) retired insns. That threshold comes from
  a histogram of frameless gaps: normal holds are all <= 2^13 insns, a
  cluster of ~30/s at 2^16, then a sparse tail up to 2^27 (see below).
  ⚠ Superseded: those long frameless gaps were the lost-interrupt bug
  (0018). With 0018, `fallback=` is 0 after boot and nonzero means trouble.
- The PIT is QEMU's ptimer LEGACY semantics reproduced on this clock (the
  immediate trigger on a zero count, the PCSR-before-PMR re-arm). Deadlines
  are checked in `ot_guest_progress` against one cached minimum, BQL taken
  only to fire. Late timers fire once and keep their phase.
- `OCTA_HOST_TIMERS=1`: the same code on QEMU_CLOCK_VIRTUAL (the old
  behaviour) for A/B in one binary. `OCTA_GCLK_LOG=1`: logs fallback
  transitions (with the DSP hold reason and codec PC), timer period changes,
  and a histogram of frameless gaps at exit. Exit stats gain
  `gclk= fallback= max_quiet=`.

Results (5600G, PGO+LTO, no 0017):

| | guest timers | host timers (same binary) | previous build |
|---|---|---|---|
| PIT0 per block, whole run (silicon 0.0363) | 0.0388 | 0.0505 | 0.0541 |
| trig8 project load, guest | 4.36, 4.39 s | 5.51, 5.68 s | 5.71, 4.47 s |
| trig8 project load, wall | 10.5, 10.3 s | 12.4, 12.5 s | 14.3, 10.4 s |
| user's card: load guest / wall | 4.6 / 9.6 s | 4.2 / 9.8 s | |
| boot to PTCH, wall (user's card) | 5.2 s | 3.7 s | |

Throughput unchanged (bench.sh, median of 3: 3467 vs 3480 Mcycles/emu-s,
both 1.000x). PIT0 per block ran 7% above silicon over a whole run because the DSP froze
during boot/load (⚠ superseded: with 0018 it's 0.03636, silicon 0.03628).
(13b4f0a shipped a 32768 threshold; 2^17 cuts steady-play fallback from 27 to
21 per 10 s and total fallback entries from 806 to 217, load unchanged.)
Gates pass (test-dsp, -metro, -emu, -emu-audio); trig8 batch 0 drops in 672
beats (8/8 valid). Loads become consistent run to run; delivery hatch 0.
**Cost: boot is ~1.5 s slower in wall time**. Boot runs at ~0.55x, and the
firmware's timed boot waits now wait in emulated time, as on silicon.

**Found on the way: the DSP froze for long stretches during project load,
on every build.** ⚠ Resolved: these were lost interrupts, where one INTC
cancelled the DSP's pending HREQ on the other. See the 0018 section above;
the latency hypothesis is answered there too.

### Diagnostics added (all opt-in, off by default)

- `OCTA_ATA_LOG=1` — every READ command and completion, with retired-guest-
  instruction and audio-block stamps. `scripts/diag/ata-reads.py LOG`. Also
  `ATARD irq` (INTRQ raised: guest SR/PC), `ATARD ack` (who acked it) and
  `ATARD data0` (first data word read), which split a read into device
  latency / ISR entry / PIO copy.
- `OCTA_PCHIST=FILE` — samples guest PC/SR/block every budget quantum (512
  retired insns), i.e. uniform in GUEST time, unlike perf. Unbiased where
  gdbstub halting is not. `scripts/diag/pc-profile.py FILE ATALOG` profiles
  the project-load window by firmware symbol and IPL.
- `OCTA_CAPTURE_BLOCKS=N` — longer `--capture-dsp` window;
  `scripts/diag/capture-diff.py A B` aligns two captures and diffs the
  CPU->DSP arm streams.
- `scripts/diag/trig-phase.py WAV START_BLOCK` — per-beat onset frame and its
  phase within the 16-frame block (how the every-8th pattern was found).

### History (the investigation that led here)

@macOS agent: the text below predates the fix.

### Run the repro yourself

```sh
tests/trig8-repro.sh mylabel                       # the current build
OCTEMU_QEMU=/path/qemu tests/trig8-repro.sh other  # another binary
OCTEMU_ARGS="--interleave 1024" tests/trig8-repro.sh il1024
```

Fixture: `tests/fixtures/trig8/` — the user's real project (set tarball +
NVRAM, 5.7 MB; AKWF single-cycle chains + a white-noise sample; the slice is
on T4, FLEX, trig every quarter note at 120 BPM) and the walk. The script
builds a fresh 256 MiB card, boots headless `--read-only`, waits out LOADING
FILES, mutes T1-T3/T5-T8, PLAYs, records 40 s (`--recording`, sample-exact,
no audio device), and `tests/trig8-body.py` scores each beat's sustained body
(RMS 75-175 ms after the beat, flagged below 50% of median). Clean:
`drops at beats []`, exit 0. The bug: `[9, 17, 25, ...]`, exit 1. The WAV is
kept at `out/trig8-<label>.wav`. ~60 s per run on the 5600G.

### What the investigation found (after the bisect below)

- **It is a ColdFire:DSP ratio sensitivity, and 0017 only moves the ratio.**
  WITHOUT 0017, `--interleave 1024` — one of the two values the upstream code
  calls calibrated — drops every 8th trig identically. 512 is on the good
  side; 0017's exact counting moves the effective ratio across. With 0017,
  384/448/480/512/1024 all drop; 256 breaks differently (with or without).
- The CPU->DSP arm stream (`--capture-dsp`, now `OCTA_CAPTURE_BLOCKS=N` for a
  longer window) is **identical** at the silent beats in both builds — the
  firmware sends the same data; what differs is where it lands in DSP time.
- Not the FIFO stall path: making the full-FIFO drain step both cores in
  kCoreSlice alternation (instead of `c.step(256)` alone) changed nothing,
  although 0017 does raise `stalls` 4 -> 44.
- Not a polling timeout: the firmware's host-port polls
  (`dsp_record_loader`, `dsp_bootstrap_feed`) are unbounded and boot-only.
- `docs/experiments/0018-io-split-budget-compat.patch` (NOT applied): keep
  0017's speed but charge a capped TB the uncapped TB's length, like stock.
  Removes the deterministic every-8th drop but leaves sporadic ones
  ([2] / [48, 77] / [] over 3 runs) — not equivalent to stock accounting.
  A real fix probably means understanding the DSP-side race (why every 8th
  trig; 8 voices? an 8-entry ring?) rather than tuning counts.
- **The race exists upstream too, just rarely.** 8 runs x 84 beats each, 4
  runs in parallel (tests/trig8-repro.sh; runs with a lost mute tap — median
  body far from ~2281, or != 84 beats — excluded):

  | build | drops |
  |---|---|
  | stock baseline (before any perf work) | 3 / 672 beats (3 of 8 runs) |
  | pre-merge PGO + idle skip + pacing | 2 / 588 (2 of 7) |
  | current `linux` (merge minus 0017) | 1 / 672 (1 of 8) |
  | with 0017 | every 8th trig, every run |

  So the perf work did not introduce it, and 0017 does not create it — it
  turns a ~0.3% sporadic drop into a deterministic one. The fix wanted is for
  the underlying race, which would also make 0017 safe to bring back.
- Shipped on `linux`: no 0017, PGO+LTO.

**Symptom** (found by the user): a FLEX track playing a single-cycle slice,
trig every quarter note — every **8th** trigger is silent. Deterministic.

**Repro, headless, no audio device**: user's saved project, walk = boot, wait
for LOADING FILES to clear, FUNC+T1/T2/T3/T5-T8 to mute the other tracks,
PLAY, 40 s; `--recording` the output; score the sustained-body RMS 75-175 ms
after each beat and flag < 50% of median. Drops at beats 9, 17, 25, 33, ...

**Bisect** (same card, same walk):

| build | drops |
|---|---|
| baseline (pre-LTO, ATA fix only) | none |
| PGO+LTO+idle skip+pacing (pre-merge) | none |
| merged, LTO, with 0017 | every 8th |
| merged, LTO, without 0017 (0015 + dsp 0012 kept) | none |
| merged + `--interleave 480` / `448` | every 8th — not a simple ratio shift |
| merged + `OCTA_NO_IDLE_SKIP=1` | every 8th — not the idle skip |

**Working theory**: ending the TB at the MMIO access lets the budget hook
(DSP slices, eDMA gate poll, ATA progress) run between guest accesses that
used to execute back-to-back inside one TB, and something on the trigger
path (the frame ISR's arms / host command sequence?) depends on that
atomicity. Unconfirmed. 0017 is worth ~8% here (LTO 3670 -> 3370
Mcycles/emu-s), so a version that keeps the hook from firing between the
split prefix and the access (or only splits where no hook-visible state is
mid-update) would be worth having — but it needs this trig test, not just
emac-diff / audio / keys-72, which all pass WITH the bug.

Linux now: PGO+LTO + stepPair, without 0017, ~3480 Mcycles/emu-s (1.000x).

## Open / next

- **DSP is ~56% of the cost** (JIT 31%, native peripherals/DMA/HDI08 26%).
  TRIED AND REJECTED: swapping in the gearmulator MD/MM fork of dsp56300
  (amorgan101010/dsp56300-md-mm @ b6ee02ca) with octemu's 11 patches rebased
  (0001/0002 already fixed there, 0005/0007/0008 hand-ported, plus your 0012).
  Functionally fine (test-dsp, metro, test-emu, audio 3/3), but **~10% SLOWER**
  (LTO A/B: 3336 -> 3685 Mcycles/emu-s): its reworked HDI08 (host-command
  arbitration, RX rate limit) and pending-DMA modelling cost more on the
  Octatrack's traffic than its JIT gains save. Rebase kept in
  ~/Documents/octemu/work/dsp-fork (Linux box) if individual commits are worth
  cherry-picking later. Note: that fork has the same latent
  RingBuffer::emplace_back(count) bug your dsp 0012 fixes (no callers there).
- Remaining hot native symbols: `DmaChannel::execTransfer` 5%,
  `HDI08::readRX/exec/writeRX` ~10%, TCG tb lookup/restore ~8%, EMAC helpers 6%.
- **Pre-existing flake**: `--gdb` runs stall after boot ~50% of the time on
  every build incl. stock (breaks `build-fixture.sh`'s readback and USB tests
  intermittently). Not investigated yet.
- Not yet measured: QEMU `-O3`, and dropping meson's `-D_GLIBCXX_ASSERTIONS`
  (debug buildtype) from the two C++ shim files.

## macOS (M1 MacBook Air, 8 GB, fanless; macOS 27, Apple clang)

Measured as wall-clock fraction of real time, 60 s headless, boot + PLAY, on a
read-only card. Every comparison is INTERLEAVED (`scripts/ab-wall.sh`): this
Air thermal-throttles ~10% after a minute, so back-to-back runs of one build
then the other are worthless. `BENCH_ARGS=--unthrottled` gives raw capacity,
which separates small wins better than the paced rate near the ceiling.

| build (cumulative) | paced | unthrottled |
|---|---|---|
| stock | ~70% | — |
| + clang LTO | ~71% | — |
| + clang PGO | ~76% | — |
| + inline fractional EMAC (qemu 0015) | ~81% | — |
| + catch-up pacing (same idea as "repays lateness") | ~92% | ~95% |
| + JIT execute-toggle cache (qemu 0016) | ~93% | ~97% |
| + HDI08 bulk writeRX (dsp 0012) | — | ~100% |
| + I/O split cap (qemu 0017) | ~99% | ~116% |
| + your idle skip (qemu 0014) | holds real time | **~129%** |

Idle skip on top of everything, 3 interleaved rounds unthrottled: 114.1 /
116.3 / 116.0% -> 126.3 / 129.6 / 129.6%. Audio 3/3, keys-72 clean.

### What changed, and whether it touches Linux

- **Inline fractional EMAC** (`patches/qemu/0015`) — portable. ~99% of the
  firmware's MAC traffic is fractional (FI=1): 67% OMC, 12% OMC+RT, 11%
  non-saturating, 8% SU. MACSR's mode bits are already in the TB flags, so
  macmulf, mac_accum + mac_set_flags (fused) and get_macf are emitted inline
  per mode; integer modes keep the helpers. This is the "EMAC helpers 6%"
  line above. Checked bit-exact against the helper build by
  `tests/emac-diff.py --ref OLD_QEMU` (random MAC sequences in all 16 modes,
  latched PAV, extremes, MAC-with-load, every accumulator read in-mode and
  raw); it catches a planted tie-rounding bug and a planted sticky-PAV bug.
- **I/O split cap** (`patches/qemu/0017`) — portable, and probably the
  "TCG tb lookup/restore ~8%" line above. MMIO that is not the last insn of
  its TB goes through cpu_io_recompile (longjmp, host-pc tree lookup, unwind,
  a 1-insn TB) and the original TB stays cached, so polling loops paid it on
  every pass: **~880k/s** here. Now the split point is remembered per TB pc,
  the TB is invalidated and retranslated ending at the access. IRQ behaviour
  is unchanged (nothing between prefix and access in either version). Side
  effect: the 0012 budget now counts exactly (the split path used to charge
  the whole TB for the prefix, the suffix again, and not the access).
  core `stalls` (host writes into a full RX FIFO, drained by stepping the
  DSP) rose 4 -> 44 per minute; nothing dropped.
- **HDI08 bulk writeRX** (`patches/dsp56300/0012`) — portable. One batched
  ring push per call instead of three atomics per word. **Also fixes a latent
  dsp56300 bug**: `RingBuffer::emplace_back(count, fn)` wrote every entry of
  the batch into ONE slot (the write count only advances after the loop).
  Nothing called it before. Worth checking whether the MD/MM fork has it too.
- **JIT execute-toggle cache** (`patches/qemu/0016`) — macOS only
  (`__APPLE__`). cpu_tb_exec calls pthread_jit_write_protect_np on every entry;
  with the budget returning to the loop constantly it was ~4% of the vCPU.
  asmjit toggles the same per-thread state but always ends in execute, so
  only QEMU's write path clears the cache.
- **Catch-up pacing**: found independently here: the vCPU was ASLEEP 18% of
  the time at 81% of real time. Your "repays lateness" (150 ms) covers it;
  100 ms measured best here (20 ms 89%, 100 ms 91.6%, 500 ms 90.9%).
- **Tools**: `tests/walks/keys-72.jsonl` (72 verified trig taps while
  playing) + `src/script.c` now prints `re-tap (key lost)` whenever a verified
  tap has to be redone — a direct measure of key delivery, which the
  throttle comment says must be re-tested whenever pacing loosens.
  `scripts/ab-wall.sh` is the interleaved wall-clock A/B (uses OCTEMU_QEMU).

Verified on every step: `make test-emac` 18/18, `tests/emac-diff.py` bit-exact,
`make test-emu-audio` 3/3 (440.0 Hz, no dropouts), keys-72 with no lost trig
taps, `make fixtures` walks succeed.

### Notes for the Linux side

- clang PGO: `-fprofile-generate=DIR` / `llvm-profdata merge` /
  `-fprofile-use=FILE` on both QEMU (`--extra-cflags`/`--extra-ldflags`, each
  flag its own option: QEMU_EXTRA_CONFIGURE is word-split) and dsp56300
  (`CMAKE_{C,CXX}_FLAGS`). QEMU's git build is -Werror, so add
  `-Wno-profile-instr-{unprofiled,out-of-date,missing}`.
- Overwriting a signed binary in place gets it SIGKILLed on macOS: `rm` before
  `cp` when swapping QEMU builds (moot with OCTEMU_QEMU).

### macOS on the merged `linux` branch (9217770 + mouse fix)

Built with clang LTO + PGO retrained on this tree; every check rerun:

- **ATA on guest progress: saving works on macOS.** `make fixtures` from
  scratch (project creation, sample load, trig walk, all saved to the card)
  succeeds with no "write INTRQ forced" / poll-cap messages.
- `make test-emac` 18/18, `tests/emac-diff.py` bit-exact vs the pre-work
  build, `make test-emu-audio` 3/3 (440.0 Hz, no dropouts), keys-72: 72/72
  trig taps first time (the usual one post-boot PLAY re-tap).
- Paced: 98.5-98.9% of real time over 60 s *including boot* — i.e. pinned at
  the pace. Unthrottled vs my previous best build (same patches, older tree):
  127.6-134.7% vs 127.7-129.5%.
- Also on this branch: `frontend: map mouse clicks correctly on Retina with
  sdl2-compat` — Homebrew's sdl2 is now sdl2-compat, which reports mouse
  events in pixels on Retina, so every click landed at 2x (not perf, but
  every current Mac build needs it).

### macOS: trig8 repro confirmed (964d226)

`tests/trig8-repro.sh` on the M1 Air, 2 runs each, same fixture:

| build | drops |
|---|---|
| current `linux` (no 0017), clang PGO+LTO | `[]`, `[]` (84/84 beats) |
| with 0017 | `[9, 17, 25, 33, 41, 49, 57, 65, 73, 81]` both runs |

Identical beat numbers and median body (2281) to Linux, so the emulation is
deterministic across hosts and the Mac can validate a race fix too.
`tests/trig8-body.py` needs numpy: `brew install numpy` on macOS (Homebrew's
python refuses a plain pip install).

### macOS: lockstep fix (74930ca) validated; 0017 loading data point

On the M1 Air, clang PGO+LTO rebuilt on 74930ca:

- `tests/trig8-repro.sh` x3: `[]` every run (0 / 252 beats). test-emu-audio
  3/3, keys-72: 72/72 and **no** post-boot PLAY re-tap this time. Paced cost
  ~1 point (interleaved: 95.0-97.1% vs 96.4-97.3%, boot included).
- lockstep + 0017 (test binary, not shipped): trig8 `[]`, `[]` — agrees with
  your 0 / 672.
- **0017's project-load slowdown is much smaller here, and zero in wall time.**
  trig8 fixture, walk = mark at PTCH, wait 3 s, wait_gone L0ADING, mark
  (so the 3 s wait is inside both numbers):

  | build | PTCH -> loaded, guest | wall |
  |---|---|---|
  | lockstep | 3.27 s, 3.27 s | 5.35 s, 5.20 s |
  | lockstep + 0017 | 5.01 s, 4.00 s | 5.11 s, 4.42 s |

  i.e. +0.7-1.7 s of guest time and no wall-clock cost, vs ~5x guest on the
  5600G. **CORRECTED below**: that walk's fixed 3 s wait hid most of it; over
  the read window the Mac is ~2.5x too. Guest time running further per wall second during the card reads
  suggests the difference is in how the load's MMIO/ATA polling interacts
  with pacing (catch-up / idle skip) on each host, not in the reads
  themselves. Happy to run any candidate on the Mac.

### macOS: your load diagnostics (1aa58ec) on the M1 Air

trig8 fixture, PGO+LTO, `OCTA_ATA_LOG=1 OCTA_PCHIST=...`, `scripts/diag/`:

| | no 0017 | 0017 |
|---|---|---|
| load window (first..last read), guest | 2.00 s | 4.94 s (**2.5x**; you: 3x) |
| PTCH -> loaded, wall | 4.8-5.2 s | 5.0-5.4 s (no wall cost here) |
| speed during load | 0.63-0.68x | 0.94-0.98x |
| per-sector insns (counted) | 3621 | 2152 |
| quanta per audio block | 221.4 | 125.8 |
| idle_loop / ata_isr share | 13.5% / 9.6% | 24.9% / 1.5% |

Same 19326 read commands / 23406 sectors in both.

- 3621 / 2152 = 1.68: that IS the old split-path overcount, measured on the
  PIO read loop. Without 0017 the loader's whole timing (and the calibrated
  ratio) sits on inflated counts; 0017 makes them exact.
- Hypothesis for your side (your ATA-on-progress code, so I haven't touched
  it): completions are delivered at the next quantum boundary, and with
  exact counts (and TBs now ending at the MMIO write) that boundary can fall
  a handful of insns after the command write, before the driver is parked to
  take the IRQ. If that wakeup is lost, the loader idles until the next frame
  ISR, which is your "frame ISR ends 51% of idle runs". A cheap test: a
  minimum completion latency (N retired insns / N blocks after the command)
  and see whether 0017's load returns to ~2 s.

### macOS: guest-time timers (c1e842d) validated

M1 Air, PGO+LTO retrained, interleaved against the previous build (1aa58ec):

- Gates: trig8 x3 `[]` (0 / 252), test-emu-audio 3/3, keys-72 72/72 (the
  usual post-boot PLAY re-tap). Paced throughput unchanged (96.7-97.3% vs
  96.2-97.2%).
- User-visible timing, trig8 card, walk = mark at start / PTCH, wait 3 s,
  wait_gone L0ADING, mark (the 3 s wait is inside PTCH->loaded):

  | | boot -> PTCH, wall | PTCH -> loaded, guest | wall |
  |---|---|---|---|
  | 1aa58ec (host timers) | 3.28, 3.26 s | 3.28, 3.26 s | 4.78, 4.86 s |
  | c1e842d (guest timers) | 3.59, 3.44 s | 4.57, 4.54 s | 5.68, 6.00 s |

- So on the Mac boot costs +0.2-0.3 s wall (not 1.5 s), but the load gets
  ~1 s SLOWER in wall time, the opposite of your 5600G. That fits your model
  from the other side: the Mac ran the load at ~0.63x, so wall-clock ticks
  came ~1.6x too often per guest second and sped up a loader that waits on
  ticks. Faithful ticks remove that bonus. Loads are now identical run to
  run and match across hosts, which makes the loader's tick/HREQ waiting
  (your "DSP freezes during project load") the one thing to fix everywhere.

### macOS: 0017 + 0018 (INTC) and everything since, validated

M1 Air, clang PGO+LTO on 7a01779:

- **`scripts/gates.sh` / `make gates`** (new, please use it on Linux too):
  13/13 PASS: test-dsp, -metro, -emu, test-emac, emac-diff, audio x3,
  trig8 x3, keys-72, save-reload. A known-bad build (0017 without the
  lockstep + INTC fixes) fails trig8 at once (then hangs in save-reload until
  its 2400 s walk timeout).
- 0017 + 0018 vs the guest-timer build, interleaved: **paced 99.3% on every
  run** (was 97.1-97.8%, boot included); **unthrottled 151-157%** (was
  129-136%). Load 6.95 s guest (~7 s like the real unit), 6.9 s wall.
- **Your startup-warble fix (2cc0f12):** PLAY right after LOADING, ratio
  0.9968, no starving, 0 rebuffers. BUT don't trust SDL's **dummy** audio
  driver for pitch on macOS: measured, it consumes **44763 frames/s = 1.0150x**
  (standalone SDL test, 20 s). The monitor then drains its cushion to ~50 ms
  and sits on the 0.97 clamp: that's the fake device running fast, not the
  monitor. Real CoreAudio runs at its true rate.
- **Fixed: SIGBUS with `SDL_VIDEODRIVER=offscreen` on macOS** (your silent-test
  recipe). skin_render ignored SDL_LockTexture's result; the lock fails there
  and the LCD expansion wrote through an uninitialised pointer (skin.c:804).
  Now a failed lock falls back to SDL_UpdateTexture (commit c6a7704).
  Offscreen + dummy now boots, loads and plays.
