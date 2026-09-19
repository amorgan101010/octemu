#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""usb-midi.py — resurrect the firmware's dormant USB-MIDI half.

OS 1.40C ships a complete USB-MIDI TX implementation (encoder + EP2
primitives, usb_midi_* in re/coldfire.syms) that nothing reaches, and no RX
decoder at all. This patch wires it in:

- grown config descriptors (MSC + AudioControl + MIDIStreaming, EP2 bulk
  in/out, bNumInterfaces=3) served from the in-image free zone, with the
  responder's four pea sites repointed and its two hardcoded moveq #32
  length clamps widened;
- EP2 brought up at SET_CONFIGURATION (hook at 0x4001d9ca) via the dormant
  usb_midi_ep2_init, dQH max packet widened to 512 for high speed, and
  CLEAR_FEATURE(ENDPOINT_HALT) taught to answer for EP2 instead of stalling;
- EP2 completions dispatched from usb_isr (hook at 0x4001e606): IN
  completion frees the encoder's dTD, OUT completion runs the new RX
  decoder (custom/coldfire/usb-midi.s) which feeds the received USB-MIDI event packets
  into midi_rx_enqueue — the same byte path DIN MIDI uses — and re-primes;
- TX mirrored into the dormant encoder from both senders: midi_send
  (channel messages, hook at its entry) and the priority realtime byte
  sender 0x400108b0 (clock/transport), guarded by a single-dTD busy flag.

Builds on the receive-amp image by default, so ONE image carries RECEIVE+AMP and
USB-MIDI both.

Usage: custom/usb-midi.py [--in out/receive-amp.bin] [--out out/usb-midi.bin]
"""
import argparse
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x40000400
# ☠ Do not move this past image end (0x40110000): such an overlay LOADS but
# does not SURVIVE, because the firmware wipes/reuses the tail of SDRAM at
# boot. Everything lives in the in-image free zone instead — 2060 bytes at
# 0x400d24d0, zero and unreferenced, of which receive-amp takes the first 32
# for per-track state, hence the start below.
ZONE_ADDR = 0x400D24F0
ZONE_END = 0x400D2CDC                  # last long of the zone is live
ASM = os.path.join(ROOT, "custom/coldfire/usb-midi.s")

# hook sites: (vaddr, expected bytes, overlay symbol)
TRAMPS = [
    (0x4001D9CA, "4879400b9868", "usbmidi_setcfg_shim"),   # SET_CONFIG body
    (0x4001E606, "2039fc0b01ac", "usbmidi_isr_shim"),      # usb_isr UI path
    (0x40010BC8, "4fefffec48d7", "usbmidi_send_shim"),     # midi_send entry
    (0x400108B0, "2f02122f000b", "usbmidi_prio_shim"),     # priority sender
    (0x4001DAEC, "303946c8ce0c", "usbmidi_clrfeat_shim"),  # CLEAR_FEATURE(halt)
]
# GET_DESCRIPTOR(CONFIG) pea sites: (pea vaddr, old target, which new cfg)
PEAS = [
    (0x4001D880, 0x400E201C, "cfg_fs"),
    (0x4001D888, 0x400E203C, "cfg_hs"),
    (0x4001D8BE, 0x400E207C, "cfg_os_hs"),
    (0x4001D8C6, 0x400E205C, "cfg_os_fs"),
]
# the hardcoded moveq #32 clamps (two per responder)
CLAMPS = [(0x4001D858, 0x70), (0x4001D862, 0x72),
          (0x4001D896, 0x70), (0x4001D8A0, 0x72)]


def config_descriptor(hs):
    """MSC + AC + MS composite config, 124 bytes, mirroring the stock MSC
    part at 0x400e201c/203c byte for byte."""
    msc_pkt = 512 if hs else 64
    def ep(addr, pkt):
        return bytes([7, 5, addr, 2]) + struct.pack("<H", pkt) + bytes([0])
    def ep_midi(addr, pkt):
        return bytes([9, 5, addr, 2]) + struct.pack("<H", pkt) + bytes(3)
    ms_class = (bytes([7, 0x24, 1, 0, 1, 37, 0]) +          # MS header
                bytes([6, 0x24, 2, 1, 1, 0]) +              # IN jack emb 1
                bytes([6, 0x24, 2, 2, 2, 0]) +              # IN jack ext 2
                bytes([9, 0x24, 3, 1, 3, 1, 2, 1, 0]) +     # OUT jack emb 3
                bytes([9, 0x24, 3, 2, 4, 1, 1, 1, 0]))      # OUT jack ext 4
    body = (bytes([9, 4, 0, 0, 2, 8, 6, 0x50, 0]) +         # MSC interface
            ep(0x81, msc_pkt) + ep(0x01, msc_pkt) +
            bytes([9, 4, 1, 0, 0, 1, 1, 0, 0]) +            # AudioControl
            bytes([9, 0x24, 1, 0, 1, 9, 0, 1, 2]) +         # AC header -> MS 2
            bytes([9, 4, 2, 0, 2, 1, 3, 0, 0]) +            # MIDIStreaming
            ms_class +
            ep_midi(0x02, msc_pkt) + bytes([5, 0x25, 1, 1, 1]) +
            ep_midi(0x82, msc_pkt) + bytes([5, 0x25, 1, 1, 3]))
    total = 9 + len(body)
    hdr = bytes([9, 2]) + struct.pack("<H", total) + bytes([3, 1, 0, 0xC0, 3])
    return hdr + body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp",
                    default=os.path.join(ROOT, "out/receive-amp.bin"))
    ap.add_argument("--out", default=os.path.join(ROOT, "out/usb-midi.bin"))
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

    # 1. assemble + link the shims at their final address
    obj, elf, blob = "/tmp/usbmidi.o", "/tmp/usbmidi.elf", "/tmp/usbmidi.bin"
    for cmd in (["m68k-elf-as", "-mcpu=54454", "-o", obj, ASM],
                ["m68k-elf-ld", "-Ttext=%#x" % ZONE_ADDR, "-e", "0",
                 "-o", elf, obj],
                ["m68k-elf-objcopy", "-O", "binary", elf, blob]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("failed: %s\n%s" % (" ".join(cmd), r.stderr))
    code = open(blob, "rb").read()
    syms = {}
    nm = subprocess.run(["m68k-elf-nm", elf], capture_output=True, text=True)
    for ln in nm.stdout.splitlines():
        parts = ln.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)

    # 2. descriptors appended after the code, 4-aligned
    overlay = bytearray(code)
    while len(overlay) % 4:
        overlay.append(0)
    cfg_addrs = {}
    fs, hs = config_descriptor(False), config_descriptor(True)
    assert len(fs) == len(hs), "FS/HS configs must share one clamp length"
    clamp = len(fs)
    assert clamp <= 127, "config too big for the moveq clamps"
    for name, blob_ in [("cfg_fs", fs), ("cfg_hs", hs),
                        ("cfg_os_fs", bytes([9, 7]) + fs[2:]),
                        ("cfg_os_hs", bytes([9, 7]) + hs[2:])]:
        cfg_addrs[name] = ZONE_ADDR + len(overlay)
        overlay += blob_
    if ZONE_ADDR + len(overlay) > ZONE_END:
        sys.exit("shims+descriptors are %d bytes, zone room is %d"
                 % (len(overlay), ZONE_END - ZONE_ADDR))

    # 3. plant everything, expect()-guarded
    for va, want, sym in TRAMPS:
        expect(va, bytes.fromhex(want), "hook site " + sym)
        wr(va, b"\x4e\xf9" + struct.pack(">I", syms[sym]))
    for va, old, name in PEAS:
        expect(va, b"\x48\x79" + struct.pack(">I", old), "pea " + name)
        wr(va + 2, struct.pack(">I", cfg_addrs[name]))
    for va, opc in CLAMPS:
        expect(va, bytes([opc, 0x20]), "moveq #32 clamp")
        wr(va, bytes([opc, clamp]))

    # 4. place the blob in the free zone — expect all zeros there
    expect(ZONE_ADDR, bytes(len(overlay)), "free zone")
    wr(ZONE_ADDR, bytes(overlay))

    open(a.out, "wb").write(img)
    # Sidecar of resolved symbol addresses (name=0xADDR), so tests can read
    # the free-zone globals (usbmidi_tx_drops, etc.) that move each build.
    sym_path = os.path.splitext(a.out)[0] + ".sym"
    with open(sym_path, "w") as f:
        for n, v in sorted(syms.items()):
            f.write("%s=0x%08x\n" % (n, v))
        for name, addr in cfg_addrs.items():
            f.write("%s=0x%08x\n" % (name, addr))
    print("usb-midi: %s -> %s" % (a.inp, a.out))
    print("  %d bytes at %#x (code %d + descriptors %d), "
          "config length %d, zone slack %d" % (len(overlay), ZONE_ADDR,
                                len(code), len(overlay) - len(code), clamp,
                                ZONE_END - ZONE_ADDR - len(overlay)))
    for va, _, sym in TRAMPS:
        print("  hook %#x -> %s %#x" % (va, sym, syms[sym]))
    for name, addr in cfg_addrs.items():
        print("  %s at %#x" % (name, addr))


main()
