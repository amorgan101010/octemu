#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""usb-audio.py — the first copy-to-scratch feature: a UAC1 audio-streaming
IN endpoint (the emulated MAIN output, 44.1 kHz stereo 16-bit) added to the
usb-midi composite.

Unlike usb-midi (which revived dormant firmware), there is NO audio-streaming
code in the OS, and the new code (four grown config descriptors + the EP3 iso
servicing) is far larger than the ~408 B of image free space usb-midi leaves.
So the payload lives on the CF card as /USBAUDIO.BIN, is copied at runtime into
the free SDRAM scratch region (0x48001000), and runs there.

Two pieces are assembled:

- custom/coldfire/usb-audio.s  (payload, linked at 0x48001000): stage2 runtime installer,
  the SET/GET_INTERFACE + usb_isr + descriptor-clamp shims, the iso packet
  builder, and the four grown UAC1 configs (this script generates them as an
  assembly include). Written to the card as /USBAUDIO.BIN and to
  out/usb-audio-payload.bin.
- custom/coldfire/usb-audio-tramp.s  (trampoline, linked into that image slack):
  hooks fs_card_detect_poll, and once the card is mounted loads the payload
  into scratch, invalidates the I-cache, and jsr's stage2.

Only TWO in-image writes land here, both expect()-guarded: the trampoline blob
in the free slack, and the 6-byte jmp into fs_card_detect_poll. Everything
else the payload needs is installed AT RUNTIME by stage2 (into image code, but
only after the card blob is confirmed loaded — so an image without the card
file is exactly the stock usb-midi composite).

Builds on out/usb-midi.bin by default -> out/usb-audio.bin.

Usage: custom/usb-audio.py [--in out/usb-midi.bin] [--out out/usb-audio.bin]
"""
import argparse
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x40000400
SCRATCH = 0x48001000                    # payload load address in SDRAM scratch
# What usb-midi leaves of the in-image free zone: it occupies up to 0x400d2b44,
# so 408 bytes remain, to 0x400d2cdc.
ZONE_TRAMP = 0x400D2B44
ZONE_END = 0x400D2CDC
ASM = os.path.join(ROOT, "custom/coldfire/usb-audio.s")
ASM_TRAMP = os.path.join(ROOT, "custom/coldfire/usb-audio-tramp.s")

# fs_card_detect_poll hook: 8 displaced bytes (two instructions), replaced by a
# 6-byte jmp (the trailing 2 bytes become dead — control jumps away).
FSCDP = 0x4003F174
FSCDP_BYTES = "2f022239460d1cbc"

SAMPLE_RATE = 44100
ISO_MAXPKT = 45 * 4                      # 45 frames * 4 bytes = 180 (stereo s16)


def config_descriptor(hs, other_speed=False):
    """The usb-midi MSC+AC+MS composite (byte-identical to usb-midi.py) grown
    with a fourth interface: UAC1 AudioStreaming (alt 0 zero-bandwidth + alt 1
    with an iso IN EP), Format Type I PCM 44100/16/2. The AC header now links
    both the MIDIStreaming (2) and AudioStreaming (3) interfaces."""
    bulk = 512 if hs else 64

    def ep(addr, pkt):
        return bytes([7, 5, addr, 2]) + struct.pack("<H", pkt) + bytes([0])

    def ep_midi(addr, pkt):
        return bytes([9, 5, addr, 2]) + struct.pack("<H", pkt) + bytes(3)

    ms_class = (bytes([7, 0x24, 1, 0, 1, 37, 0]) +
                bytes([6, 0x24, 2, 1, 1, 0]) +
                bytes([6, 0x24, 2, 2, 2, 0]) +
                bytes([9, 0x24, 3, 1, 3, 1, 2, 1, 0]) +
                bytes([9, 0x24, 3, 2, 4, 1, 1, 1, 0]))
    # AudioStreaming interface 3: alt 0 (zero bandwidth) then alt 1 (iso EP).
    as_iface = (
        bytes([9, 4, 3, 0, 0, 1, 2, 0, 0]) +                # IF3 alt0 AS
        bytes([9, 4, 3, 1, 1, 1, 2, 0, 0]) +                # IF3 alt1 AS
        bytes([7, 0x24, 1, 3, 0]) + struct.pack("<H", 1) +  # AS_GENERAL, PCM
        bytes([11, 0x24, 2, 1, 2, 2, 16, 1]) +              # FORMAT_TYPE_I
        struct.pack("<I", SAMPLE_RATE)[:3] +                # tSamFreq (3 B LE)
        bytes([9, 5, 0x83, 0x05]) +                         # iso IN EP, async
        struct.pack("<H", ISO_MAXPKT) + bytes([1, 0, 0]) +  # maxpkt, bInterval 1
        bytes([7, 0x25, 1, 0, 0]) + struct.pack("<H", 0))   # CS iso EP
    # AC header: bInCollection 2, baInterfaceNr [2, 3] (length 10).
    ac_header = bytes([10, 0x24, 1, 0, 1, 10, 0, 2, 2, 3])
    body = (bytes([9, 4, 0, 0, 2, 8, 6, 0x50, 0]) +
            ep(0x81, bulk) + ep(0x01, bulk) +
            bytes([9, 4, 1, 0, 0, 1, 1, 0, 0]) +
            ac_header +
            bytes([9, 4, 2, 0, 2, 1, 3, 0, 0]) +
            ms_class +
            ep_midi(0x02, bulk) + bytes([5, 0x25, 1, 1, 1]) +
            ep_midi(0x82, bulk) + bytes([5, 0x25, 1, 1, 3]) +
            as_iface)
    total = 9 + len(body)
    dt = 7 if other_speed else 2
    hdr = bytes([9, dt]) + struct.pack("<H", total) + bytes([4, 1, 0, 0xC0, 3])
    return hdr + body


def emit_cfg_include(path):
    """Write an assembly include placing the four configs, each 256-aligned so
    none crosses a 4 KB page (the EP0 responder sends from buffer page 0 only —
    0x4001d498), with symbols the payload links against."""
    fs = config_descriptor(False)
    hs = config_descriptor(True)
    os_fs = config_descriptor(False, other_speed=True)
    os_hs = config_descriptor(True, other_speed=True)
    assert len(fs) == len(hs) == len(os_fs) == len(os_hs), "configs must match"
    lines = ["| generated by custom/usb-audio.py — do not edit",
             "    .balign 4096", "    .global usbaudio_configs", "usbaudio_configs:"]
    for name, blob in [("cfg_fs", fs), ("cfg_hs", hs),
                       ("cfg_os_fs", os_fs), ("cfg_os_hs", os_hs)]:
        lines.append("    .balign 256")
        lines.append("    .global %s" % name)
        lines.append("%s:" % name)
        lines.append("    .byte " + ",".join("0x%02x" % b for b in blob))
    open(path, "w").write("\n".join(lines) + "\n")
    return len(fs)


def assemble(src, link_addr, out_blob, defsyms, incdir):
    obj = out_blob + ".o"
    elf = out_blob + ".elf"
    cmd_as = ["m68k-elf-as", "-mcpu=54454", "-I", incdir, "-o", obj, src]
    cmd_ld = ["m68k-elf-ld", "-Ttext=%#x" % link_addr, "-e", "0", "-o", elf, obj]
    for name, val in defsyms.items():
        cmd_ld += ["--defsym", "%s=%#x" % (name, val)]
    cmd_oc = ["m68k-elf-objcopy", "-O", "binary", elf, out_blob]
    for cmd in (cmd_as, cmd_ld, cmd_oc):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("failed: %s\n%s" % (" ".join(cmd), r.stderr))
    syms = {}
    nm = subprocess.run(["m68k-elf-nm", elf], capture_output=True, text=True)
    for ln in nm.stdout.splitlines():
        p = ln.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    return open(out_blob, "rb").read(), syms


def read_sym(path):
    d = {}
    for ln in open(path):
        if "=" in ln:
            k, v = ln.strip().split("=")
            d[k] = int(v, 16)
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp",
                    default=os.path.join(ROOT, "out/usb-midi.bin"))
    ap.add_argument("--out", default=os.path.join(ROOT, "out/usb-audio.bin"))
    ap.add_argument("--payload",
                    default=os.path.join(ROOT, "out/usb-audio-payload.bin"))
    a = ap.parse_args()
    img = bytearray(open(a.inp, "rb").read())
    midi = read_sym(os.path.splitext(a.inp)[0] + ".sym")

    def off(va):
        return va - BASE

    def expect(va, want, what):
        got = bytes(img[off(va):off(va) + len(want)])
        if got != want:
            sys.exit("refusing: %s at %#x is %s, expected %s"
                     % (what, va, got.hex(), want.hex()))

    tmp = "/tmp"
    cfg_len = emit_cfg_include(os.path.join(tmp, "usb-audio-cfg.s"))

    # 1. payload -> scratch (0x48001000). Chains into usb-midi's isr shim; the
    # config-clamp shim needs the grown length; the pea-repoint expect() bytes
    # are the usb-midi config addresses the responder currently holds.
    # The readback arena carries 32-bit samples; this is the right-shift that
    # lands them in int16. CALIBRATED against --recording by
    # tests/usb-audio-stream-test.sh, not assumed.
    AUDIO_SHIFT = 16
    defs = {"AUDIO_SHIFT": AUDIO_SHIFT,
            "USBMIDI_ISR_SHIM": midi["usbmidi_isr_shim"], "CFG_LEN": cfg_len,
            "MIDI_CFG_FS": midi["cfg_fs"], "MIDI_CFG_HS": midi["cfg_hs"],
            "MIDI_CFG_OS_FS": midi["cfg_os_fs"], "MIDI_CFG_OS_HS": midi["cfg_os_hs"]}
    payload, psyms = assemble(ASM, SCRATCH, os.path.join(tmp, "usbaudio.bin"),
                              defs, tmp)

    # 2. trampoline -> image slack. Knows the payload's stage2 entry, load
    # address, sector count, and prologue magic (validates the blob landed).
    # Pad to a longword so the trampoline's checksum covers the whole file and
    # the card copy's size is an exact, checkable number.
    payload += b"\x00" * (-len(payload) % 4)
    magic = struct.unpack(">I", payload[:4])[0]
    # the length and longword sum the trampoline gates the card file on before
    # it jsr's into it (see custom/coldfire/usb-audio-tramp.s)
    psum = sum(struct.unpack(">%dI" % (len(payload) // 4), payload)) & 0xFFFFFFFF
    tdefs = {"STAGE2": psyms["usbaudio_stage2"], "PAYLOAD_BASE": SCRATCH,
             "PAYLOAD_SECTORS": (len(payload) + 511) // 512, "STAGE2_MAGIC": magic,
             "PAYLOAD_LEN": len(payload), "PAYLOAD_WORDS": len(payload) // 4,
             "PAYLOAD_SUM": psum}
    tramp, _ = assemble(ASM_TRAMP, ZONE_TRAMP, os.path.join(tmp, "usbatramp.bin"),
                        tdefs, tmp)
    if ZONE_TRAMP + len(tramp) > ZONE_END:
        sys.exit("trampoline is %d bytes, slack is %d"
                 % (len(tramp), ZONE_END - ZONE_TRAMP))

    # 3. plant the two in-image writes.
    expect(FSCDP, bytes.fromhex(FSCDP_BYTES), "fs_card_detect_poll")
    img[off(FSCDP):off(FSCDP) + 6] = b"\x4e\xf9" + struct.pack(">I", ZONE_TRAMP)
    expect(ZONE_TRAMP, bytes(len(tramp)), "trampoline slack")
    img[off(ZONE_TRAMP):off(ZONE_TRAMP) + len(tramp)] = tramp

    open(a.out, "wb").write(img)
    open(a.payload, "wb").write(payload)
    sym_path = os.path.splitext(a.out)[0] + ".sym"
    with open(sym_path, "w") as f:
        for n, v in sorted(psyms.items()):
            f.write("%s=0x%08x\n" % (n, v))
    print("usb-audio: %s + %s -> %s" % (a.inp, a.payload, a.out))
    print("  payload %d B at %#x (card /USBAUDIO.BIN), config length %d"
          % (len(payload), SCRATCH, cfg_len))
    print("  trampoline %d B at %#x (slack %d), stage2 %#x"
          % (len(tramp), ZONE_TRAMP, ZONE_END - ZONE_TRAMP - len(tramp),
             psyms["usbaudio_stage2"]))


main()
