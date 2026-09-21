#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check a USB-audio card payload before you copy it.

☠ Reading "it booted" as "it worked" is the single mistake that cost the most
on this feature. This checks the payload the way the trampoline does, so a
file that passes here is one a flashed unit will accept — and a file that
fails here would otherwise leave USB audio silently absent on a machine that
looks perfectly healthy.

Header v2 (all big-endian longwords):
  +0  magic 'OTPL'
  +4  total length (header + blob + relocation table)
  +8  checksum: sum of every longword, minus this field, equals this field
  +12 entry OFFSET from the load base (0 = load but never execute)
  +16 relocation table offset from the load base
  +20 relocation entry count
  +24 link base, the address the code was linked at

  tests/usb-audio-preflight.py [PAYLOAD]      (default out/USBAUDIO.BIN)
"""
import struct
import sys

MAGIC = 0x4F54504C
HDR = 28
MAX = 0x10000

path = sys.argv[1] if len(sys.argv) > 1 else "out/USBAUDIO.BIN"
b = open(path, "rb").read()
print(f"payload : {path} ({len(b)} B)")

ok = True


def check(cond, good, bad):
    global ok
    print("  " + ("ok: " + good if cond else "FAIL: " + bad))
    if not cond:
        ok = False


check(len(b) >= HDR, "large enough for the header",
      f"{len(b)} B is shorter than the {HDR}-byte header")
if len(b) >= HDR:
    mag, total, ck, entry, roff, rcnt, link = struct.unpack(">7I", b[:HDR])
    check(mag == MAGIC, "magic 'OTPL'", f"magic 0x{mag:08x} is not 'OTPL'")
    check(len(b) % 4 == 0, "a whole number of longwords",
          f"{len(b)} B is not a multiple of 4")
    check(total == len(b), f"declared length {total} == actual size",
          f"declared {total} != actual {len(b)} (TRUNCATED?)")
    check(len(b) <= MAX, f"within the {MAX} B limit",
          f"{len(b)} B exceeds the {MAX} B the trampoline will load")
    if len(b) % 4 == 0 and total == len(b):
        acc = sum(struct.unpack(">%dI" % (len(b) // 4), b)) & 0xFFFFFFFF
        check(((acc - ck) & 0xFFFFFFFF) == ck,
              f"checksum 0x{ck:08x} verifies",
              f"checksum 0x{ck:08x} does not verify (CORRUPT?)")
    check(entry == 0 or HDR <= entry < len(b),
          ("entry 0 — loads but never executes (a probe)" if entry == 0
           else f"entry offset +0x{entry:x} lies inside the blob"),
          f"entry offset +0x{entry:x} is outside the blob")
    # the relocation table the loader will walk
    check(HDR <= roff <= len(b) and roff + 4 * rcnt <= len(b),
          f"{rcnt} relocations at +0x{roff:x}, inside the blob",
          f"relocation table at +0x{roff:x} x{rcnt} runs past the blob")
    if HDR <= roff and roff + 4 * rcnt <= len(b):
        bad = [i for i in range(rcnt)
               if not (HDR <= struct.unpack_from(">I", b, roff + 4 * i)[0] <= len(b) - 4)]
        check(not bad, "every relocation offset lands inside the blob",
              f"{len(bad)} relocation offsets point outside the blob")
    check(link != 0, f"link base 0x{link:08x} recorded",
          "link base is 0 — the loader cannot compute the fixup delta")

print("PREFLIGHT OK — copy to the card root as /USBAUDIO.BIN" if ok
      else "PREFLIGHT FAILED — rebuild with custom/usb-audio.py")
sys.exit(0 if ok else 1)
