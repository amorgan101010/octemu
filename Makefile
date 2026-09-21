# SPDX-License-Identifier: MIT
# octemu — see README.md. `make doctor` checks your computer for what the
# build needs.
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc)
UNAME := $(shell uname)
IMG  := out/os/main.bin
QEMU := vendor/qemu/build/qemu-system-m68k
DSP  := vendor/dsp56300

# ☠ Defined at the top: make expands a rule's target AND prerequisite names
# when it parses the rule, so anything naming a file built from these must
# see them first. Defined lower down, every such name came out empty.
OS_VER  ?= 1.40C
# ☠ A sha in the filename must mean the image IS that commit. With anything
# uncommitted it would be a lie, so a dirty tree builds as SNAPSHOT instead.
BUILD   ?= $(shell git diff --quiet HEAD 2>/dev/null && \
                   git rev-parse --short=6 HEAD 2>/dev/null || echo SNAPSHOT)
# ☠ The ELEK version field holds TEN bytes, and the firmware tool silently
# leaves the STOCK version in place when it is handed more — a flashed unit
# then reports 1.40C with nothing to say the custom OS landed. "OE-" plus a
# 6-char sha is 9; the truncation is the backstop for any longer BUILD
# (SNAPSHOT, or an override passed on the command line).
VERSION ?= $(shell printf %.10s "OE-$(BUILD)")
STOCKSYX = downloads/extracted/OCTATRACK_OS$(OS_VER).syx
STOCKBIN = downloads/extracted/OCTATRACK_OS$(OS_VER).bin
EFT      = vendor/elektron-firmware-tool/elektron-firmware-tool

# ------------------------------------------------------------ custom images --
# Each one patches YOUR out/os/main.bin, adding a machine or a feature the stock
# OS does not have (README.md says what each one is). They assemble the ColdFire
# sources in custom/coldfire/, so they need the m68k cross-assembler.
ASM_PREFLIGHT = @command -v m68k-elf-as >/dev/null || \
    { echo "m68k-elf-as not found — brew install m68k-elf-binutils m68k-elf-gcc" >&2; exit 1; }

# Each firmware is emitted under ONE basename in three forms, differing only by
# suffix: .os is the raw patched MAIN OS section (what ./octemu --os takes and
# what the next patch in the chain builds on), .bin is the ELUP container for
# the CF-card upgrade path, .syx is the same container for MIDI DIN. Elektron's
# own OCTATRACK_OS<ver>.bin is a container too, so only the container may carry
# that name; the raw section gets .os so the format is never in doubt.
OSNAME   = out/OCTATRACK_OS$(OS_VER)_$(1)_$(BUILD)
RECEIVE  = $(call OSNAME,receive)
USBMIDI  = $(call OSNAME,usb-midi)
USBAUDIO = $(call OSNAME,usb-audio)
DSPA := $(DSP)/build/source/dsp56kEmu/libdsp56kEmu.a \
        $(DSP)/build/source/dsp56kBase/libdsp56kBase.a \
        $(DSP)/build/source/asmjit/libasmjit.a

CXXFLAGS := -std=c++17 -O2 -g -I$(DSP)/source -I$(DSP)/source/asmjit/src \
            -DDSP56300_DEBUGGER=0 -DASMJIT_STATIC
CFLAGS   := -std=c11 -O2 -g -Wall
# Lazy (=), not immediate (:=): on a box without sdl2 an immediate expansion
# runs pkg-config for EVERY target, so `make setup` — whose whole job is to get
# you to a working box — would open with two pkg-config errors it cannot help.
SDL_CFLAGS = $(shell pkg-config --cflags sdl2)
SDL_LIBS   = $(shell pkg-config --libs sdl2)

# One platform file, named after `uname`: an unported host then fails by naming
# the file somebody has to write, not with a pile of undefined symbols.
EMUSRC := src/main.c src/panel.c src/ocr.c src/audio.c src/skin.c src/script.c \
          src/platform/$(shell echo $(UNAME) | tr A-Z a-z).c

# The default build is what `./octemu` with no flags needs: both
# programs and the rasterized panel.
all: octdsp octemu panel card

.PHONY: all setup doctor os qemu panel panel-svg card fixtures demo-gif \
        receive-gif \
        test test-audio \
        test-dsp test-dsp-metro test-emac test-emu test-emu-audio \
        test-emu-usb test-usb-midi test-usb-audio \
        test-emu-usb-midi test-emu-usb-midi-conform test-emu-usb-midi-enum \
        test-emu-usb-midi-stress test-emu-usb-midi-coexist \
        test-emu-usb-midi-imgcheck \
        test-emu-usb-audio-enum test-emu-usb-audio-alt \
        test-emu-usb-audio-cadence test-emu-usb-audio-stream \
        test-emu-usb-audio-safety \
        fw-all fw-receive fw-usb-midi fw-usb-audio clean distclean

# ---------------------------------------------------------------- preflight --
# scripts/doctor.sh reads the Brewfile, so the dependency list lives in exactly
# one place.
doctor:
	@scripts/doctor.sh

setup:
	@scripts/setup.sh

# The three vendored archives. A real prerequisite, so a target that needs them
# says what to run instead of failing inside the compiler.
$(DSPA):
	@echo "missing $@ — run 'make setup'" >&2; exit 1

os $(IMG):
	@scripts/fetch-os.sh

# A real rule, not just the .PHONY alias: every test depends on $(QEMU), so
# skipping `make qemu` self-heals instead of failing in the child process.
qemu: $(QEMU)
$(QEMU): $(wildcard src/board/ot-*) $(wildcard patches/qemu/0*.patch) | $(DSPA)
	@scripts/build-qemu.sh

# A blank 64 MiB FAT32 card image for a zero-flag run. It is a build product, so
# `./octemu` with no arguments finds one instead of creating it at
# runtime; pass --cf-card to use your own.
card: out/state/card.img

out/state/card.img: scripts/mkcard.sh scripts/card.py
	@scripts/mkcard.sh out/state/card.img

panel: out/panel/panel.bin

panel-svg: assets/panel/octatrack.svg

# gen_svg.py writes octatrack.svg and octatrack-elements.json into the CWD, so
# it runs from its own directory. Both outputs are generated and gitignored.
assets/panel/octatrack.svg assets/panel/octatrack-elements.json: assets/panel/gen_svg.py
	cd assets/panel && python3 gen_svg.py
	@echo "ok: assets/panel/octatrack.svg + octatrack-elements.json"

out/panel/panel.bin: assets/panel/rasterize.py assets/panel/octatrack.svg \
                     assets/panel/octatrack-elements.json
	python3 assets/panel/rasterize.py

# ----------------------------------------------------------- the programs ---
# The DSP chip is shared with the QEMU shim, so this program exercises the
# same code the Octatrack runs — and answers a DSP question in four seconds.
octdsp: src/dsp-main.cc src/board/ot-dsp56k.cc src/board/ot-dsp56k.h src/wav.h $(DSPA)
	c++ $(CXXFLAGS) -o $@ src/dsp-main.cc src/board/ot-dsp56k.cc $(DSPA) -lpthread

# src/platform/darwin.c is the macOS port: app activation, the USB DISK MODE
# host mount and the --midi bridge.
octemu: $(EMUSRC) src/emu.h src/wav.h src/platform/platform.h \
                    src/board/ot-audio-shm.h src/board/ot-panel-wire.h
	cc $(CFLAGS) $(SDL_CFLAGS) -o $@ $(EMUSRC) $(SDL_LIBS) -lpthread -lm \
	    $(if $(filter Darwin,$(UNAME)),-lobjc -framework CoreMIDI -framework CoreFoundation)

# ---------------------------------------------------------------- fixtures --
# The card fixtures every audio and USB test starts from, built once and then
# cached (delete a directory to rebuild). They are made by driving the firmware's
# own UI through scripted walks, because only the firmware can lay out its own
# project data: out/fx is a set + project + a 440 Hz sine in the AUDIO pool,
# out/fx2 is that project with FX1/FX2 set to NONE, the sine in STATIC slot 1 and
# a trig on step 1, and stage 2 then reads the FX ids back to prove the walk
# landed. Under three minutes for both; tests/README.md has the detail.
fixtures: out/fx2/card.img

out/fx/card.img out/fx/nvram.bin: octemu $(QEMU) $(IMG) \
                                  tests/build-fixture.sh scripts/mkcard.sh \
                                  scripts/card.py tests/walks/fixture-project.jsonl
	@echo "== out/fx: project-creation walk, about 32 s =="
	@tests/build-fixture.sh

out/fx2/card.img out/fx2/nvram.bin: octemu $(QEMU) $(IMG) \
                                    out/fx/card.img tests/walks/fixture-trig.jsonl
	@echo "== out/fx2: FX-none + sample + trig walk, about 2.5 min =="
	@tests/build-fixture.sh trig

# --------------------------------------------------------------------- demo --
# assets/demo.gif, the picture at the top of the README. It is a recording of
# tests/walks/demo.jsonl, which starts from the out/fx fixture and leaves it
# alone, so this is repeatable: same card in, same walk, same film out.
#
# The GIF comes out at the face plate's own 1177 px, which is the default and
# also the cheap path: a 2x display's readback divides by exactly two, so the
# reduction is a box average in the emulator rather than a lanczos in ffmpeg.
demo-gif: octemu $(QEMU) $(IMG) out/panel/panel.bin \
          out/fx/card.img tests/walks/demo.jsonl
	@echo "== assets/demo.gif: the demo walk, about 30 s =="
	@rm -rf out/demo && mkdir -p out/demo
	@cp out/fx/card.img out/fx/nvram.bin out/demo/
	./octemu --cf-card out/demo/card.img --nvram out/demo/nvram.bin \
	    --script tests/walks/demo.jsonl --recording assets/demo.gif \
	    --timeout 900

# assets/receive.gif, the RECEIVE machine being set up and played. The -amp
# image, because without the stock DSP amp envelope a RECEIVE track passes
# audio forever and the trigs this walk places would shape nothing. Needs the
# m68k cross-assembler.
receive-gif: octemu $(QEMU) $(RECEIVE).os out/panel/panel.bin \
             out/fx/card.img tests/walks/receive.jsonl
	@echo "== assets/receive.gif: the RECEIVE walk, about 2 min =="
	@rm -rf out/receive && mkdir -p out/receive
	@cp out/fx/card.img out/fx/nvram.bin out/receive/
	./octemu --cf-card out/receive/card.img --nvram out/receive/nvram.bin \
	    --os $(RECEIVE).os --script tests/walks/receive.jsonl \
	    --recording assets/receive.gif --timeout 900

# -------------------------------------------------------------------- tests --
test-dsp: octdsp $(IMG)
	@echo "== test-dsp: expect ~4 s =="
	./octdsp --in-a sin:440 --out-main out/dsp-test.wav --timeout 2 \
	    --expect-tone 440

test-emu: octemu $(QEMU) $(IMG)
	@echo "== test-emu: expect ~4 s =="
	./octemu --headless --cf-card none --nvram none \
	    --script tests/walks/boot-nocard.jsonl --timeout 90

# The metronome is core 1's, and it is the only test that exercises the
# inter-core handoff through the shared window, core 1's render path and the
# ICC audio return. It also fails loudly on MPYI's immediate sign
# (patches/dsp56300/0010): with that bug the envelope still runs and the output
# is silent, which is why it is asserted on the sound and not on a counter.
test-dsp-metro: octdsp $(IMG)
	@echo "== test-dsp-metro: expect ~5 s =="
	@mkdir -p out/metro
	./octdsp --metro 1378 --metro-cue 32 --metro-main 32 \
	    --out-main out/metro/click.wav --timeout 3
	python3 tests/click-quality.py out/metro/click.wav \
	    --expect-clicks 5 --spacing 0.5

test-emac: $(QEMU)
	@echo "== test-emac: expect ~10 s =="
	python3 tests/emac-conform.py
	python3 tests/emac-macload.py

# Boot the trig fixture, press TRIG9, and require the reference sine back
# unbroken: one burst per trig, no dropouts, every zero-crossing period within
# a sample of ideal.
test-emu-audio: octemu $(QEMU) $(IMG) out/fx2/card.img
	@echo "== test-emu-audio: expect ~65 s =="
	@echo "   ☠ THIS TEST FAILS IN CLUSTERS on an unchanged binary — measured:"
	@echo "     four consecutive silent runs, then five clean ones. No small"
	@echo "     number of runs is evidence either way. Re-run at least 3x, and"
	@echo "     read ship_nz in out/trig/walk.log: cleanly 0 on a bad run and"
	@echo "     8270-8271 on a good one, which separates 'the DSP produced"
	@echo "     silence' from 'the transport lost it' in a single run."
	@rm -rf out/trig && mkdir -p out/trig
	cp out/fx2/card.img out/fx2/nvram.bin out/trig/
	./octemu --headless --cf-card out/trig/card.img \
	    --nvram out/trig/nvram.bin --script tests/walks/trig-one.jsonl \
	    --recording out/trig/trig.wav --timeout 1800 2> out/trig/walk.log
	python3 tests/audio-quality.py out/trig/trig.wav 440 --expect-bursts 3

# The USB packet bench gate: the stock firmware must enumerate as mass
# storage against the scripted host (--usb-host) and answer INQUIRY /
# TEST UNIT READY over bulk EP1.
test-emu-usb: octemu $(QEMU) $(IMG) out/fx2/card.img
	tests/usb-bench-test.sh msc

# The USB-MIDI end-to-end demo on the patched image: OT->host clock/transport
# out EP2 IN, and host->OT a USB note-on firing track 1's sample out of MAIN.
test-emu-usb-midi: octemu $(QEMU) $(USBMIDI).os out/fx2/card.img
	tests/usb-midi-demo.sh

# USB-MIDI RX message-type conformance: every MIDI message class sent in on
# EP2 OUT must decode to the right byte count in the DIN FIFO, at BOTH
# full and high speed.
test-emu-usb-midi-conform: octemu $(QEMU) $(USBMIDI).os out/fx2/card.img
	tests/usb-midi-conform.sh

# USB-MIDI descriptor + enumeration conformance: independent structural
# validation of the composite config against USB-MIDI 1.0, plus the standard
# requests a host issues incl CLEAR_FEATURE(halt) on EP2.
test-emu-usb-midi-enum: octemu $(QEMU) $(USBMIDI).os out/fx2/card.img
	tests/usb-midi-enum.sh

# USB-MIDI TX loss-free gate: sustained clock/transport out EP2 IN with the drop
# counter at zero under continuous drain, and overflow COUNTED (not silent)
# under deliberate starvation.
test-emu-usb-midi-stress: octemu $(QEMU) $(USBMIDI).os out/fx2/card.img
	tests/usb-midi-stress.sh

# USB-MIDI composite coexistence: MSC (EP1) and MIDI (EP2) interleaved stay
# correct with zero MIDI drops, and MIDI keeps flowing across a DISK MODE
# enter/exit cycle.
test-emu-usb-midi-coexist: octemu $(QEMU) $(USBMIDI).os out/fx2/card.img
	tests/usb-midi-coexist.sh

# Image safety: nothing else writes the patch's free-zone code region.
test-emu-usb-midi-imgcheck: octemu $(QEMU) $(USBMIDI).os out/fx2/card.img
	tests/usb-midi-imgcheck.sh

# The whole USB-MIDI regression suite, in one target. Sequential on purpose: each sub-gate boots its own emulator on its own gdb port, and
# running them back-to-back (not overlapping) avoids port/socket contention.
test-usb-midi: octemu $(QEMU) $(USBMIDI).os out/fx2/card.img
	tests/usb-bench-test.sh msc
	tests/usb-midi-demo.sh
	tests/usb-midi-conform.sh
	tests/usb-midi-enum.sh
	tests/usb-midi-stress.sh
	tests/usb-midi-coexist.sh
	tests/usb-midi-imgcheck.sh
	@echo "ALL USB-MIDI GATES PASSED"

# P1 — enumerate as a 4-interface UAC1 composite with the AudioStreaming
# interface + iso IN endpoint, MSC + MIDI descriptors still intact.
test-emu-usb-audio-enum: octemu $(QEMU) $(USBAUDIO).os out/fx2/card.img
	tests/usb-audio-test.sh audio-validate

# P2 — SET_INTERFACE(3, alt 1) brings the EP3 iso stream up (a packet flows),
# alt 0 tears it down (the endpoint goes idle).
test-emu-usb-audio-alt: octemu $(QEMU) $(USBAUDIO).os out/fx2/card.img
	tests/usb-audio-test.sh audio-alt

# P3 — the 44.1 kHz iso cadence: every packet 176/180 B, 441 frames per 10.
test-emu-usb-audio-cadence: octemu $(QEMU) $(USBAUDIO).os out/fx2/card.img
	tests/usb-audio-test.sh audio-cadence 200

# P4 — the stream is real, continuous, guest-produced audio over a TRIG9 burst:
# the fixture's tone, unbroken through the sustain, at a level that tracks the
# recording. It is the summed post-FX pre-fader track bus, not MAIN, so it is a
# related signal rather than an identical one — tests/usb-audio-verify.py says
# exactly what it asserts and why each assertion can fail.
test-emu-usb-audio-stream: octemu $(QEMU) $(USBAUDIO).os out/fx2/card.img
	tests/usb-audio-stream-test.sh

# P5 — the RECOVERY gate, and the one that governs whether this may be
# flashed at all: deleting /USBAUDIO.BIN from the card is the ONLY recovery
# this feature is allowed to need, so a card the Octatrack cannot use in full
# (absent, truncated, corrupt) must leave it booting as the stock usb-midi
# composite. Runs --hw-faithful, like every USB-audio gate.
test-emu-usb-audio-safety: octemu $(QEMU) $(USBAUDIO).os out/fx2/card.img
	tests/usb-audio-safety-test.sh

# The whole USB-audio regression suite. Sequential, like test-usb-midi.
test-usb-audio: octemu $(QEMU) $(USBAUDIO).os out/fx2/card.img
	@python3 tests/lint-gates.py
	tests/usb-audio-safety-test.sh
	tests/usb-audio-qh-test.sh
	tests/usb-audio-test.sh audio-validate
	tests/usb-audio-test.sh audio-alt
	tests/usb-audio-test.sh audio-cadence 200
	tests/usb-audio-stream-test.sh
	tests/usb-audio-durability-test.sh
	tests/usb-audio-guard-test.sh
	@echo "ALL USB-AUDIO GATES PASSED"

# `make test` is the fast, deterministic set — about 25 seconds, and a clean
# run means the Octatrack boots, both DSP cores run the real payload, the
# inter-core path works and the EMAC is right. It needs no card fixture.
#
# The audio walk lives in `make test-audio` because of the cluster flakiness
# (see the banner that target prints): it is the sharpest instrument here and
# the one no single run can be trusted from.
test: test-dsp test-dsp-metro test-emac test-emu
	@echo 'PASSED: the fast set. make test-audio runs the TRIG9 walk.'

test-audio: test-emu-audio
	@echo "PASSED: the audio walk — ☠ re-run at least 3x before believing a result."


# Pack $(1).os into the two things a real unit will take.
define pack
	@test -f $(STOCKSYX) || { echo "missing $(STOCKSYX) — run 'make os'"; exit 1; }
	@test -f $(STOCKBIN) || { echo "missing $(STOCKBIN) — run 'make os'"; exit 1; }
	@test -x $(EFT) || { echo "missing $(EFT) — run 'make setup'"; exit 1; }
	python3 custom/make-bin.py --verify $(STOCKBIN)
	$(EFT) -i $(STOCKSYX) -c 3 $(1).os -V $(VERSION) \
	    --emit-container out/elek_$(BUILD).bin -o $(1).syx
	python3 custom/make-bin.py out/elek_$(BUILD).bin -o $(1).bin
	@echo
	@echo "  raw OS:     $(1).os   (./octemu --os)"
	@echo "  card image: $(1).bin  (CF root, PROJECT -> OS UPGRADE)"
	@echo "  MIDI image: $(1).syx"
	@echo "  ☠ Back up the card first. Copy the .bin to the card ROOT, eject the"
	@echo "     card properly, then PROJECT -> OS UPGRADE. An unejected write can"
	@echo "     leave a truncated file. The .syx is the MIDI DIN recovery path."
endef

fw-all: fw-receive fw-usb-midi fw-usb-audio

fw-receive:   $(RECEIVE).os  ; $(call pack,$(RECEIVE))
fw-usb-midi:  $(USBMIDI).os  ; $(call pack,$(USBMIDI))
fw-usb-audio: $(USBAUDIO).os ; $(call pack,$(USBAUDIO))

$(RECEIVE).os: custom/receive.py custom/coldfire/receive.s \
               custom/coldfire/receive-amp.s $(IMG)
	$(ASM_PREFLIGHT)
	python3 custom/receive.py --out $@

# The USB-MIDI image: the RECEIVE build with the dormant USB-MIDI half
# resurrected — one image carries RECEIVE+AMP and USB-MIDI both.
$(USBMIDI).os: custom/usb-midi.py custom/coldfire/usb-midi.s $(RECEIVE).os
	$(ASM_PREFLIGHT)
	python3 custom/usb-midi.py --in $(RECEIVE).os --out $@

# The USB-audio image: the usb-midi build with a UAC1 AudioStreaming interface
# + iso IN endpoint added. The payload rides the CF card as /USBAUDIO.BIN and is
# copied into SDRAM scratch at runtime, so ONE image carries RECEIVE+AMP,
# USB-MIDI, and USB-audio.
$(USBAUDIO).os: custom/usb-audio.py custom/coldfire/usb-audio.s \
                custom/coldfire/usb-audio-tramp.s \
                custom/coldfire/usb-audio-alloc.s \
                custom/coldfire/usb-audio-report.s \
                custom/coldfire/usb-audio-guard.s $(USBMIDI).os
	$(ASM_PREFLIGHT)
	python3 custom/usb-audio.py --in $(USBMIDI).os --out $@

# ------------------------------------------------------------------ hardware --
# The fw-* targets above emit all three forms under one basename. BUILD is the
# git short SHA, so a filename maps back to a commit — or SNAPSHOT when the
# tree is dirty, since a SHA would then be a lie. Override it with
# `make fw-usb-audio BUILD=042` to stamp something else (<= 6 chars).
#
# ☠ The emulator cannot tell you an image is safe to flash: it models neither
# the flash writer nor the bootloader's container check. What IS checked here:
# custom/make-bin.py --verify regenerates Elektron's own OCTATRACK_OS1.40C.bin
# byte for byte from that file's own container, so the ELUP encoder is right.
# ☠ Flashing is not documented in README.md; the steps this target prints are
# the only guidance a user gets, so keep them accurate: back up the card, copy
# to the card ROOT, eject properly, PROJECT -> OS UPGRADE.

# ------------------------------------------------------------------- clean --
# `clean` takes out what a rebuild replaces in seconds: the two programs, any
# debug bundle beside them (including ones left by an older binary name), and
# the generated panel sources. It deliberately spares out/ — the card fixtures
# in there cost three minutes of emulated Octatrack to rebuild, and
# out/os/main.bin is unpacked from YOUR copy of the firmware.
clean:
	rm -f octdsp octemu src/*.o src/board/*.o
	rm -rf *.dSYM
	rm -f assets/panel/octatrack.svg assets/panel/octatrack-elements.json

# Back to a fresh checkout. `make setup qemu os` puts it all back, at the cost
# of re-cloning the ~1.2 GB of vendored sources and re-fetching the OS.
distclean: clean
	rm -rf out vendor downloads
