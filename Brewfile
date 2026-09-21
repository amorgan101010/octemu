# SPDX-License-Identifier: MIT
# Everything the build can use, and the single source `make doctor` reads.
#
#   brew bundle     # install them all
#   make doctor     # what is here, what is missing, what each one is for
#
# Each line carries the command doctor should look for and why it is wanted:
#
#   need:<cmd>   nothing builds without it
#   want:<cmd>   one named feature needs it; everything else works without
#
# Keep the annotations accurate — doctor has no other list.

# building the patched QEMU, the DSP core, and the two programs
brew "cmake"              # need:cmake       builds the DSP core and QEMU
brew "ninja"              # need:ninja       builds QEMU
brew "pkg-config"         # need:pkg-config  finds sdl2
brew "sdl2"               # need:sdl2-config the emulator's window and audio
brew "python@3.12"        # need:python3     the build and test scripts

# card images: FAT32 without root and without mounting anything
brew "mtools"             # need:mformat     creates and fills card images

# make panel: rasterizing the panel skin
brew "librsvg"            # need:rsvg-convert  the panel skin (make panel)
brew "imagemagick"        # need:magick        the panel skin (make panel)

# --recording out.mov: muxing video and audio
brew "ffmpeg"             # want:ffmpeg      --recording to .mov

# make fw-*: assembling the ColdFire sources of the custom firmware
brew "m68k-elf-binutils"  # want:m68k-elf-as   make fw-*: custom firmware
brew "m68k-elf-gcc"       # want:m68k-elf-ld   make fw-*: custom firmware
