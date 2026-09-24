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

Effective clock under load is ~3.9 GHz here, so ~3600 is only ~8% headroom.

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

- **DSP is now ~56% of the cost** (JIT 31%, native peripherals/DMA/HDI08 26%).
  In progress: rebasing octemu's 11 dsp56300 patches onto the faster
  gearmulator MD/MM fork of dsp56300 (amorgan101010/dsp56300-md-mm, b6ee02ca).
  0001/0002 are already fixed there; 0003/4/6/9/10/11 apply; 0005/0007/0008
  hand-ported. Not yet built or A/B'd.
- Remaining hot native symbols: `DmaChannel::execTransfer` 5%,
  `HDI08::readRX/exec/writeRX` ~10%, TCG tb lookup/restore ~8%, EMAC helpers 6%.
- **Pre-existing flake**: `--gdb` runs stall after boot ~50% of the time on
  every build incl. stock (breaks `build-fixture.sh`'s readback and USB tests
  intermittently). Not investigated yet.
- Not yet measured: QEMU `-O3`, and dropping meson's `-D_GLIBCXX_ASSERTIONS`
  (debug buildtype) from the two C++ shim files.
