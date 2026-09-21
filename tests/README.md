# tests/

Everything here is a **regression gate**: something that fails loudly when a
change breaks a property the Octatrack is supposed to have. The exploratory
tooling this project was mapped with is gone; what is left is the set that has
caught real defects and is expected to catch them again.

Two kinds of file: the **walks** in `tests/walks/` — scripted sessions that
drive the Octatrack through its own panel, one JSON object per line,
interpreted by `src/script.c` — and the **harnesses** that run them and judge
what came out.

The exceptions to "everything here is a gate" are `walks/demo.jsonl` and
`walks/receive.jsonl`, which assert nothing: they are the sessions the
README's `assets/demo.gif` and `assets/receive.gif` are films of, recorded by
`make demo-gif` and `make receive-gif`. They still run against a fixture and
still gate every gesture on the screen, so a walk that finishes is a walk the
firmware understood.

```sh
make test         # the fast deterministic set, ~25 s, no fixtures needed
make fixtures     # the card fixtures — one time, under 3 min, then cached
make test-audio   # the TRIG9 audio walk (needs the fixtures)
make test-usb-midi test-usb-audio    # the two USB suites (need the fixtures)
```

`make test` is `test-dsp test-dsp-metro test-emac test-emu`. Every harness
below is reached from one of the targets above (a few by way of another
harness) — nothing here is a manual-only tool.

☠ `make test-audio` fails in clusters on an unchanged binary (measured: four
silent runs, then five clean ones). Re-run it at least three times before
believing either result; the target prints what to read in `out/trig/walk.log`.

## The card fixtures — what `make fixtures` is

`make fixtures` produces two directories, each a `card.img` (a CF card image)
plus the `nvram.bin` that holds the Octatrack's saved state:

- **`out/fx`** — a card carrying a set and a project, with a 1 s 440 Hz sine in
  the set's AUDIO pool, and the NVRAM holding that project open.
- **`out/fx2`** — that project with FX1 and FX2 set to NONE, the sine loaded
  into STATIC slot 1 on track 1, and a trig on step 1, saved back to the card.
  This is the state every audio and USB assertion starts from: boot it and
  TRIG9 fires a known tone with no effects in the path.

**How the cards themselves are made.** `scripts/card.py` writes the MBR itself
and uses mtools (`mformat`, `mcopy`, `mdir`) for the filesystem — no `hdiutil`,
no mount, no root, and the same on Linux. 64 MiB, one FAT32-LBA partition at
LBA 2048, one sector per cluster = 127,006 clusters; the firmware's own FAT
ceiling is what that number is for.

**The FX-NONE gate.** The 40 UP taps in `walks/fixture-trig.jsonl` have no
screen to gate them, so stage 2 finishes by booting the built fixture
`--read-only` with `--gdb` and reading `published_fx_ids` (`0x80000ec4`, indexed
`[sel*8 + track]`, sel 0 = FX1, sel 1 = FX2). Track 1 must read `00` in both
halves. A fresh project reads `04` (FLTR) and `08` (DELAY) — which is why those
taps exist at all — so a fixture built with the taps lost fails the build
instead of silently detuning every audio assertion made on it afterwards.

**Why they are walked, not written.** Only the firmware can lay out its own
project and bank data, and a project it did not create is not a project it will
load. So `tests/build-fixture.sh` builds them the long way round: it boots the
emulator and drives the firmware's own UI through
`walks/fixture-project.jsonl` and then `walks/fixture-trig.jsonl` — creating the
set, naming the project, selecting machines, loading the sample, placing the
trig, saving — and every gesture is gated on what is actually on screen (read
by the OCR in `src/ocr.c`), so a firmware that puts up a different dialog fails
the build instead of producing a subtly wrong card.

**Why it costs what it costs.** The Octatrack runs at real time, so a walk
costs roughly what it asks the guest to live through. Measured in guest time:
the project walk consumes 76,521 audio blocks = 28 s, the trig walk 331,008
blocks = 120 s. The rest is boot, the firmware's bank reload after a project is
created, and stage 2's second short boot to read the FX ids back. Measured wall
time: stage 1 about 32 s, stage 2 about 2.5 min, so under three minutes for
both. (An earlier comment in this repo claimed twenty; it was never measured.)

**Why they are cached.** `build-fixture.sh` skips a stage whose outputs already
exist, so the cost is paid once per checkout and every later run boots into the
interesting state in seconds. Delete `out/fx2` (or `out/fx`) to rebuild. No
gate can corrupt them: the audio and stream walks copy the card first, and the
USB gates that boot `out/fx2` directly do it `--read-only`.

## The walks

| walk | driven by | what it does |
|---|---|---|
| `fixture-project.jsonl` | `build-fixture.sh` (`make fixtures`) | builds `out/fx`: creates the set and project, leaving the sine in the AUDIO pool |
| `fixture-trig.jsonl` | `build-fixture.sh trig` (`make fixtures`) | builds `out/fx2`: FX1/FX2 to NONE, the sine into STATIC slot 1, a trig on step 1, saved |
| `trig-one.jsonl` | `make test-emu-audio` | three TRIG9 presses on a copy of `out/fx2`, recorded to a WAV |
| `boot-nocard.jsonl` | `make test-emu` | boots headless with no card and reaches the no-card dialog |
| `boot-hold.jsonl` | `usb-bench-test.sh`, `usb-midi-conform.sh`, `usb-midi-enum.sh`, `usb-audio-test.sh`, `usb-audio-safety-test.sh` | boots the fixture and holds on the main page, so a host or probe can attach |
| `usb-midi-tx.jsonl` | `usb-midi-demo.sh` | presses PLAY, so the sequencer emits clock and transport over USB-MIDI |
| `midi-rx-note.jsonl` | `usb-midi-demo.sh` | holds while the host injects notes, so RX can be judged on the audio |
| `usb-midi-stress.jsonl` | `usb-midi-stress.sh`, `usb-midi-imgcheck.sh` | a sustained MIDI TX stream to drain against |
| `usb-diskmode.jsonl` | `usb-midi-coexist.sh` | enters and leaves USB DISK MODE with MIDI flowing |
| `usb-audio-trig.jsonl` | `usb-audio-stream-test.sh` (and `usb-audio-test.sh` via `WALK=`) | TRIG9 bursts while the host pulls the isochronous stream |

☠ Waits inside a walk are in **guest** time (`wait_guest_ms`) — the Octatrack's
own clock, not the host's. That is deliberate and must stay: a walk whose waits
were wall-clock lands its gestures in an Octatrack that has not got there yet.

## The harnesses

One line each: what stops being caught if the file goes away.

| file | what breaks silently without it |
|---|---|
| `build-fixture.sh` | nothing can produce `out/fx`/`out/fx2` — every audio and USB gate below loses its card and can only be skipped |
| `audio-quality.py` | a note chopped into fragments still "records": the periodic-mute bug shattered each burst into ~20 pieces, which no dropout count and no whole-file compare sees. `--expect-bursts` is what catches it |
| `click-quality.py` | the metronome test passes on silence — and silence is exactly how the MPYI immediate-sign defect presents, with the envelope still running |
| `emac-conform.py` + `emac.toml` | QEMU's ColdFire EMAC defects come back unnoticed. Every expectation is quoted from the MCF54455RM, never observed, so it cannot re-derive the emulator's own bugs |
| `emac-macload.py` | MAC-with-load — the exact form the STATIC voice's resampler inner loop is built from, and the one `emac-conform.py` does not reach — silently returns the post-load operand |
| `usb-host.py` | every USB gate disappears: it is the scripted host all of them are written against |
| `rsp-io.py` | the USB gates lose their only view of the firmware's own counters (TX drops, DISK MODE flag) and their only way to arm the sync-send gates — the assertions decay into "the emulator did not crash" |
| `usb-bench-test.sh` | the device stops enumerating as mass storage, or stops answering INQUIRY / TEST UNIT READY, and nothing notices |
| `usb-midi-demo.sh` | TX or RX breaks end to end while every structural test still passes — this is the only gate that watches a host note-on turn into sound |
| `usb-midi-conform.sh` | the CIN table and the RX decoder drift apart, so a message class enqueues the wrong byte count — checked at both USB bus speeds, full and high |
| `usb-midi-enum.sh` | descriptor topology that a real host would reject, because our own host is more forgiving than USB-MIDI 1.0 |
| `usb-midi-stress.sh` | messages are dropped under load without being counted (the single-dTD TX lost the 0xFA behind the 0xF8 clock), and overflow becomes silent again |
| `usb-midi-coexist.sh` | MSC and MIDI on one composite device start stepping on each other, or MIDI dies across a DISK MODE mount handoff |
| `usb-midi-imgcheck.sh` | something else in the firmware writes the in-image free zone the patch lives in — corruption that a functional test would only show as a mystery on hardware |
| `usb-audio-test.sh` | the UAC1 composite stops enumerating, the alt-setting stops bringing the iso endpoint up and down, or the 44.1 kHz cadence goes wrong (packet size, frames per 10) |
| `usb-audio-stream-test.sh` + `usb-audio-verify.py` | the iso stream keeps flowing while carrying silence, noise, or a capture with dropped and duplicated frames. A positive control runs every time, so the test is known to be able to fail |
| `usb-audio-safety-test.sh` | the recovery property: an absent, truncated or corrupt `/USBAUDIO.BIN` must still leave the Octatrack booting as the stock composite. This is the gate that governs whether the image may be flashed at all |
| `usb-audio-durability-test.sh` | the stream dies on a project reload and nothing notices: `flex_heap_init` wipes the sample heap on every load, and a payload that does not survive it leaves its hooks pointing into freed memory. The assertion is audible, not structural |
| `usb-audio-guard-test.sh` | the hook guard stops being checked, so a payload that has been wiped or overwritten is jumped into anyway — on a unit an illegal instruction at IPL 5 with the PC in the sample heap, unrecoverable without pulling the card. Carries its own negative control, because the guard has to be seen to fire |
| `usb-audio-qh-test.sh` | the emulator's "UNINITIALIZED dQH" detector stops firing. That defect crashed a real Octatrack twice by letting the USB controller write through stale buffer pointers, so the gate runs the detector both ways — a deliberately broken payload that must trip it, and the shipping one that must not |
| `canary.py` | the durability and guard gates lose the tool that says whether a region of guest memory was actually left alone: it stamps a verifiable pattern over slices, then re-reads every byte and names each slice INTACT or CLOBBERED |
| `lint-gates.py` | the 8-minute USB-audio suite goes back to being used as a syntax checker — an unset shell variable under `set -u`, or a build script invoked without a required argument, stays invisible until the line runs |
| `usb-audio-card.sh` | the USB-audio gates have no card carrying the payload; a stale one silently degrades the Octatrack to the stock composite and every audio assertion then fails for the wrong reason |
