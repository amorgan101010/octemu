# Licence

```
MIT License

Copyright (c) 2026 Mark Roberts

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## What you build from this may not be redistributed

`make qemu` combines QEMU (GPL-2.0-only as a whole) with the `dsp56300` DSP
core (GPL-3.0-only). Those two cannot be satisfied at once in one work, so the
`qemu-system-m68k` it produces is **permanently undistributable** — there is no
licence you can pick for it. Building it on your own computer for your own use
is allowed; publishing it, in a release, an image or a CI artifact, is not.

| artifact | links | if you distribute it |
|---|---|---|
| `qemu-system-m68k` | QEMU (GPLv2-only) + dsp56300 (GPLv3-only) + `src/board/ot-*` | **never** |
| `octdsp` | `src/dsp-main.cc`, `src/board/ot-dsp56k.cc`, dsp56300, asmjit (Zlib) | GPL-3.0-only |
| `octemu` | `src/*.c` (MIT), SDL2 (Zlib), CoreMIDI | MIT — no GPL in it |

`src/board/ot-dsp56k.cc` compiles into both GPLv2 QEMU and GPLv3-linked
`octdsp`, so it must stay MIT. A GPL header there quietly breaks one of them.

## The parts that are not MIT

- **`src/board/ot-panel-uart.c`** is GPL-2.0-or-later: it is derived function
  for function from QEMU's `hw/char/mcf_uart.c` (Copyright (c) 2007
  CodeSourcery). Every other `src/board/ot-*` file is original work and MIT.
- **A patch takes the licence of the tree it applies to**, being a derivative
  of it: `patches/qemu/` is GPL-2.0-or-later and `patches/dsp56300/` is
  GPL-3.0-only. A patch that adds a whole new file carries that file's own
  header instead — `patches/qemu/0012` adds `ot-insn-budget.h`, original work,
  MIT.
- **`custom/`** is MIT. Those programs patch *your* OS image at build time and
  contain no Elektron code.

Everything the build fetches — QEMU, dsp56300, asmjit, SDL2,
elektron-firmware-tool, the OS image — comes from its own upstream onto your
computer, under its own licence. None of it is redistributed here.
