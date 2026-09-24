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
