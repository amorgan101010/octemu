#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""RECEIVE — turn the stock NEIGHBOR machine (id 3) into an internal mixer.

RECEIVE keeps NEIGHBOR's id, menu slot and dispatch, and replaces the handler
with one that sums up to six source tracks at independent levels
(custom/coldfire/receive.s), read from the per-track post-FX / pre-fader buffers
the DSP returns to the CPU. It runs on ANY track, where stock NEIGHBOR refuses
tracks 1 and 5, having no predecessor within a DSP core's group of four.

Every edit site is checked against the image before it is written
(machine_vtable_a=0x400d6434 A[3]=neighbor_stager, readback_buf=0x80003190,
staging_cursor=0x80001c80; see re/coldfire.syms), so this refuses to patch an
image it does not recognize rather than corrupting one.

Handler runs on the ColdFire (m68k-elf-as -mcpu=54454), 240 bytes, in the 240
freed by absorbing NEIGHBOR's prep. Params: page 1 LV1..LV6 (0..127), page 2
SRC1..SRC6 (0=OFF, 1..8=that track).

Usage: custom/receive.py [--amp] [--in out/os/main.bin] [--out out/receive.bin]
"""
import argparse
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x40000400
ASM = os.path.join(ROOT, "custom/coldfire/receive.s")

# ---- descriptor layout (offsets from the descriptor pointer) ----
P_NAME, P_NAME_STRIDE = 0x016, 6
P_DEFAULT = 0x05E
P_COUNT = 0x09A
P_FMT = 0x0CA
P_FN2 = 0x0FA
P_ENUM = 0x12A                     # NOT 0x12E — arrays tile every 0x30
P_ENABLE_HI = 0x18A
P_ENABLE_LO = 0x18E

NEIGHBOR_P = 0x400D34D2
FMT_LEVEL = 0                      # NULL formatter -> plain 0..127
FN2_LEVEL = 0x400479B4             # THRU's VOL entry
FN2_SELECT = 0x400467A4            # THRU's INAB/INCD selector entry
ENUM_SELECT = 0x400328E4           # page-class handler for selectors

TRACK_GUARDS = (0x400797DC, 0x4005A5D2)   # "NOT AVAILABLE HERE!" on track 1/5
# ☠ THE PREDECESSOR SILENCER. A post-pass over the control block at 0x40004eae
# walks the tracks, reads the NEXT track's published machine id, and when it is
# 3 writes zero into the slot word 32 bytes BEHIND the cursor — the previous
# track's mix level. That is the "silence the track before a NEIGHBOR" that no
# search for muting code turns up: it never names the predecessor, it just
# steps back 32 bytes. Two sites, both `moveq #3,%d2` before the compare;
# pointing them at a dead id leaves RECEIVE free to publish its own id 3.
SILENCER_CMPS = (0x40004EB4, 0x40004EC6)
NAME_LONG = 0x400B5413             # 'NEIGHBOR\0'
NAME_SHORT = 0x400B540A            # 'NHB\0'
VTAB_AUDIO3, VTAB_PREP3 = 0x400D6440, 0x400D6460
SLOT_START, SLOT_END = 0x4000463C, 0x4000472C   # stock NEIGHBOR prep + audio
# ---- --amp: borrow the stock DSP amp envelope (custom/coldfire/receive-amp.s) ----
# ☠ NEIGHBOR-class machines have their own prep, not the note-arming prep that
# STATIC, FLEX and PICKUP run (0x4000f450), which is why an armed NEIGHBOR track
# passes audio forever and its AMP page is inert. --amp is the fix: it gives
# RECEIVE a trig recorder of its own rather than borrowing that prep, whose
# sample lookup a RECEIVE track cannot satisfy. The hook replaces the record
# builder's id-3 word-30 marker block: other ids keep the stock bclr, id 3 gets
# the envelope driver.
AMP_ASM = os.path.join(ROOT, "custom/coldfire/receive-amp.s")
# the landing zone: 338 zero bytes no word in the image points into (the
# install expect()s them zero before writing).
CODE_ZONE, CODE_ROOM = 0x400C45B0, 338
HOOK_SITE, HOOK_LEN = 0x40004D4A, 28
HOOK_EXPECT = bytes.fromhex(
    "2045" "7110" "3211" "7403" "b480" "6608"
    "2001" "08c00009" "6006" "2001" "08800009" "3280")


def assemble(src, stem):
    """Assemble one .s to a flat .text blob; returns (bytes, {sym: offset})."""
    obj, blob = "/tmp/%s.o" % stem, "/tmp/%s.bin" % stem
    for cmd in (["m68k-elf-as", "-mcpu=54454", "-o", obj, src],
                ["m68k-elf-objcopy", "-O", "binary", "-j", ".text", obj, blob]):
        if subprocess.run(cmd).returncode != 0:
            sys.exit("failed: " + " ".join(cmd))
    nm = subprocess.run(["m68k-elf-nm", obj], capture_output=True, text=True)
    syms = {l.split()[2]: int(l.split()[0], 16)
            for l in nm.stdout.splitlines() if " T " in l}
    return open(blob, "rb").read(), syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", default=os.path.join(ROOT, "out/os/main.bin"))
    ap.add_argument("--out", default=os.path.join(ROOT, "out/receive.bin"))
    ap.add_argument("--amp", action="store_true",
                    help="make the AMP page WORK: hook the record builder's "
                         "id-3 marker block so a RECEIVE voice runs the "
                         "stock DSP amp envelope — silent at rest, a trig "
                         "opens it, ATK/HOLD/REL/VOL/BAL/XVOL shape it, and "
                         "REL=INF after one trig is NEIGHBOR's drone. "
                         "Installs custom/coldfire/receive-amp.s in the free zone and "
                         "points prep[3] at its trig recorder.")
    a = ap.parse_args()
    img = bytearray(open(a.inp, "rb").read())

    def off(va):
        return va - BASE

    def expect(va, want, what):
        got = bytes(img[off(va):off(va) + len(want)])
        if got != want:
            sys.exit("refusing: %s at %#x is %s, expected %s"
                     % (what, va, got.hex(), want.hex()))

    def wr(va, data):
        img[off(va):off(va) + len(data)] = data

    def wr32(va, v):
        wr(va, struct.pack(">I", v))

    # the handler first: its size has to fit the slot it replaces
    handler, _ = assemble(ASM, "receive")
    room = SLOT_END - SLOT_START
    if len(handler) > room:
        sys.exit("receive.s assembles to %d bytes, the NEIGHBOR slot holds %d "
                 "-- shrink the handler" % (len(handler), room))

    # displayed names, in place
    expect(NAME_LONG, b"NEIGHBOR\0", "long machine name")
    expect(NAME_SHORT, b"NHB\0", "short machine name")
    wr(NAME_LONG, b"RECEIVE\0")
    wr(NAME_SHORT, b"RCV\0")

    # descriptor: LV1..LV6 (page 1), SRC1..SRC6 (page 2)
    if struct.unpack(">I", img[off(NEIGHBOR_P + P_ENABLE_LO):][:4])[0] != 0:
        sys.exit("refusing: NEIGHBOR's descriptor already declares parameters")
    for i in range(12):
        expect(NEIGHBOR_P + P_NAME + i * P_NAME_STRIDE, b"---\0\0\0", "p%d name" % i)
        expect(NEIGHBOR_P + P_ENUM + i * 4, b"\0\0\0\0", "p%d handler3 slot" % i)
        level = i < 6
        name = ("LV%d" % (i + 1)) if level else ("SRC%d" % (i - 5))
        wr(NEIGHBOR_P + P_NAME + i * P_NAME_STRIDE, name.encode()[:5].ljust(6, b"\0"))
        wr32(NEIGHBOR_P + P_FMT + i * 4, FMT_LEVEL if level else 0)
        wr32(NEIGHBOR_P + P_FN2 + i * 4, FN2_LEVEL if level else FN2_SELECT)
        wr32(NEIGHBOR_P + P_ENUM + i * 4, 0 if level else ENUM_SELECT)
        wr32(NEIGHBOR_P + P_COUNT + i * 4, 128 if level else 9)   # OFF + T1..T8
        img[off(NEIGHBOR_P + P_DEFAULT + i)] = 0
    wr32(NEIGHBOR_P + P_ENABLE_LO, 0x11111111)
    wr32(NEIGHBOR_P + P_ENABLE_HI, 0x00001111)

    # point both silencer id tests at a dead id, so RECEIVE can publish id 3
    # as itself and still leave the track before it audible
    for site in SILENCER_CMPS:
        expect(site, b"\x74\x03", "predecessor-silencer id test")
        img[off(site) + 1] = 0x05

    # unblock the machine on tracks 1 and 5 (moveq #3 -> #5, a dead id)
    for site in TRACK_GUARDS:
        expect(site, b"\x76\x03", "track 1/5 guard")
        img[off(site) + 1] = 0x05

    # dispatch: prep[3], audio[3] -> the handler
    expect(VTAB_PREP3, struct.pack(">I", 0x4000463C), "prep vtable[3]")
    expect(VTAB_AUDIO3, struct.pack(">I", 0x4000466C), "audio vtable[3]")
    # RECEIVE absorbs the voice-marking into its audio handler, so prep[3] is
    # free. NULL leaves the track behaving like NEIGHBOR: armed once, sounding
    # forever, AMP page dead. --amp points it at the trig recorder instead, so a
    # trig opens the stock amp envelope and ATK/HOLD/REL/VOL shape it.
    if a.amp:
        amp, syms = assemble(AMP_ASM, "receive-amp")
        if len(amp) > CODE_ROOM:
            sys.exit("receive-amp is %d bytes, code zone is %d"
                     % (len(amp), CODE_ROOM))
        expect(CODE_ZONE, b"\0" * len(amp), "receive-amp landing zone")
        wr(CODE_ZONE, amp)
        # hook the builder's id-3 marker block: jsr hook + nops
        expect(HOOK_SITE, HOOK_EXPECT, "id-3 word-30 marker block")
        wr(HOOK_SITE, b"\x4e\xb9" + struct.pack(">I", CODE_ZONE + syms["hook"])
           + b"\x4e\x71" * ((HOOK_LEN - 6) // 2))
        wr32(VTAB_PREP3, CODE_ZONE + syms["prep"])
    else:
        wr32(VTAB_PREP3, 0)
    wr32(VTAB_AUDIO3, SLOT_START)

    # the handler itself, then clear the rest of the slot
    wr(SLOT_START, handler)
    for va in range(SLOT_START + len(handler), SLOT_END):
        img[off(va)] = 0

    out = a.out if os.path.isabs(a.out) else os.path.join(ROOT, a.out)
    open(out, "wb").write(bytes(img))
    changed = sum(1 for x, y in zip(open(a.inp, "rb").read(), img) if x != y)
    print("RECEIVE installed (NEIGHBOR id 3 -> internal mixer):")
    print("  handler    %d bytes in %d (%d free)" % (len(handler), room, room - len(handler)))
    print("  descriptor %#x  page1 LV1..LV6, page2 SRC1..SRC6" % NEIGHBOR_P)
    print("  names      RECEIVE / RCV; tracks 1/5 unblocked; "
          "id 3 as itself, silencer patched")
    print("  bytes changed vs stock: %d  -> %s" % (changed, out))


if __name__ == "__main__":
    main()
