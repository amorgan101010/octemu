# octemu

**octemu** is an Octatrack emulator for macOS, built around [QEMU][QEMU]
(ColdFire MCF54455), [dsp56300][dsp56300], and [SDL2][SDL2] with the goal of
providing a development environment for firmware customizations, including MIDI
and audio over USB.

![octemu running the emulated front panel](assets/demo.gif)

## `octemu` & `octdsp`

### Build

```sh
make doctor   # what your computer is missing, with the formula for each
make setup    # vendor toolchain: dsp56300, elektron-firmware-tool
make os       # fetch + unpack YOUR copy of the OS -> out/os/main.bin
make qemu     # the patched QEMU (~10-15 min)
make          # both programs, the panel raster, a blank CF card
```

### Run

```sh
./octemu                                                  # the Octatrack, windowed
./octdsp --in-a sin:440 --out-main out/x.wav --timeout 2  # its DSP cores, no QEMU
```

`octemu` boots from the CF card image and battery file `make` leaves in
`out/state/`.

### Demo

This will create a CF card with a simple set and project, where track 1 is
configured with a STATIC machine and a sine wave assigned to its first slot:

```sh
make out/fx2
./octemu --cf-card out/fx2/card.img --nvram out/fx2/nvram.bin
```

## Firmware customizations

Customizations are applied to a stock Elektron-supplied firmware image you
provide, using [elektron-firmware-tool][elektron-firmware-tool].

> [!CAUTION]
> I take no responsibility for any damage to your Octatrack, data loss to your
> CF card, etc., from attempting to use these firmware customizations. Be
> careful.

### RECEIVE machine

This is a generalization of the NEIGHBOR machine. Instead of only receiving the
preceding track as input, you can choose up to 6 tracks to receive as input,
p-lock how much each track contributes to the mix, and place trigs.

**You can create feedback loops this way.** Be careful.

```sh
make fw-receive
./octemu --os out/OCTATRACK_OS1.40C_receive_<build>.os
```

![Configuring a RECEIVE machine in octemu](assets/receive.gif)

### USB-MIDI

Reverse-engineering revealed partial support for USB-MIDI already implemented in
the Octatrack firmware. This customization completes it. I've done some basic
validation, and it seems to work. It is a mirror of the DIN ports.

```sh
make fw-usb-midi
./octemu --os out/OCTATRACK_OS1.40C_usb-midi_<build>.os --midi
```

### USB-Audio

Adds a UAC1 interface: the Octatrack appears as a 44.1 kHz stereo USB audio
input. The source is the summed track bus, tapped pre-fader.

```sh
make fw-usb-audio
./octemu --os out/OCTATRACK_OS1.40C_usb-audio_<build>.os
```

This one does not fit in the firmware image, so a unit needs both halves: the
flashed image, and a payload on the CF card root under that exact name.

```sh
cp out/USBAUDIO.BIN /Volumes/OCTATRACK/
```

If you have problems, you can recover by

- holding "NO" at boot (skips loading USBAUDIO.BIN).
- deleting USBAUDIO.BIN from the CF card.

## Licensing & Legal

Elektron, Octatrack and Octatrack MKII are trademarks of Elektron Music
Machines MAV AB. This project is unaffiliated with and unendorsed by them.
**Nothing Elektron-owned is in this repository.**

The `octemu` and `octdsp` binaries combine sources under incompatible licenses.
You can build them on your own computer and use them, but **you may not
distribute them.** This project's own sources are MIT-licensed.

See [LICENSE.md][LICENSE] for more details.

## Acknowledgements

Shoutout to the following projects:

- **[mxldyn/octamax][octamax]** and **[sambanks/octabam][octabam]**: the
  projects that inspired this one and did the first reverse-engineering passes
  (that I am aware of) on the Octatrack.
- **[mischa85/elektron-firmware-tool][elektron-firmware-tool]**: used for
  modifying Octatrack firmware images.
- **[The Usual Suspects][theusualsuspects]**: creators of the
  [dsp56300][dsp56300] library used for emulating the Octatrack's DSP cores.

This project was built essentially entirely with Claude Code. I have de-slopped
it where I could.

[QEMU]: https://www.qemu.org
[dsp56300]: https://github.com/dsp56300/dsp56300
[SDL2]: https://www.libsdl.org
[LICENSE]: LICENSE.md
[octamax]: https://github.com/mxldyn/octamax
[octabam]: https://github.com/sambanks/octabam
[theusualsuspects]: https://theusualsuspects.io
[elektron-firmware-tool]: https://github.com/mischa85/elektron-firmware-tool
