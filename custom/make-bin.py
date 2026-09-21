#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Wrap an ELEK container into the ELUP `.bin` the CF-card OS UPGRADE path reads.

Flashing over MIDI SysEx takes several minutes. The manual's fast alternative
(8.5.2 OS UPGRADE) is a `.bin` in the ROOT of the Compact Flash card, then
PROJECT -> OS UPGRADE -> [YES]. This writes that file.

The format is read out of the firmware's own reader at `FUN_4007f748`, which is
the loader the Octatrack runs on the card file:

    word 0          "ELUP"  (0x454c5550)
    word 1          the feedback seed, plain
    words 2..n-2    the enciphered payload
    word n-1        the enciphered 32-bit additive sum of the PLAIN payload

and the payload, once deciphered, is nothing more than

    [4-byte big-endian container length][ELEK container]

— the same container `elektron-firmware-tool --emit-container` writes, so there is
no second container format to build here.

The cipher is a one-word feedback chain: each word is combined with the
PREVIOUS CIPHER word (the seed standing in for word -1), and bit 23 of that
previous cipher word selects between two variants — a different additive-ish
constant, a different byte permutation, and a different output mask. Both
permutations are involutions, so deciphering is the same chain read backwards
and needs no inverse tables.

Neither direction is taken on faith. `--verify` runs the FORWARD encoder over
the container inside Elektron's own `OCTATRACK_OS1.40C.bin` and requires the
whole file back byte for byte, seed and checksum included; the `make fw-*`
targets run it before they write anything, and every write here round-trips
through the decoder before the file is claimed to be good.

    elektron-firmware-tool -i stock.syx -c 3 mainos.bin -V OE-abc123 \\
        --emit-container elek.bin -o out.syx
    python3 custom/make-bin.py elek.bin -o OCTATRACK_OE-abc123.bin
    python3 custom/make-bin.py --verify downloads/extracted/OCTATRACK_OS1.40C.bin
"""
import argparse
import struct
import sys
from pathlib import Path

WORD = 0xFFFFFFFF
ELUP = 0x454C5550
STOCK_SEED = 0x2F1349D2          # the seed Elektron's own 1.40C file carries
SELECT = 1 << 23                 # the bit of the previous cipher word that picks a variant


def _swap_halves(w):
    """Exchange the two 16-bit halves of a 32-bit word (an involution)."""
    return ((w & 0xFFFF) << 16) | (w >> 16)


def _reverse_bytes(w):
    """Reverse the four bytes of a 32-bit word (an involution)."""
    return int.from_bytes(w.to_bytes(4, "big"), "little")


# (feedback constant, output mask, permutation), indexed by SELECT of the
# previous cipher word: 0 -> halves, 1 -> bytes.
VARIANT = (
    (0x360FA955, 0x9E3B16A2, _swap_halves),
    (0xEF4A9AB6, 0x764E28CA, _reverse_bytes),
)


def _variant(prev):
    return VARIANT[1 if prev & SELECT else 0]


def _encipher(prev, plain):
    mix, mask, permute = _variant(prev)
    return permute((prev ^ mix ^ plain) & WORD) ^ mask


def _decipher(prev, cipher):
    mix, mask, permute = _variant(prev)
    return (prev ^ mix ^ permute(cipher ^ mask)) & WORD


def encode(payload, seed):
    """Payload bytes (word-aligned) -> the full ELUP file, checksum appended."""
    words = struct.unpack(f">{len(payload) // 4}I", payload)
    chain, total, out = seed, 0, []
    for plain in words:
        chain = _encipher(chain, plain)
        out.append(chain)
        total = (total + plain) & WORD
    out.append(_encipher(chain, total))          # the sum rides the chain like a word
    return struct.pack(f">II{len(out)}I", ELUP, seed, *out), total


def decode(raw):
    """A whole ELUP file -> (seed, payload bytes, stored checksum, running sum)."""
    if len(raw) < 12 or len(raw) % 4 or struct.unpack(">I", raw[:4])[0] != ELUP:
        raise ValueError("not an ELUP file (magic, length, or alignment)")
    seed, = struct.unpack(">I", raw[4:8])
    cipher = struct.unpack(f">{(len(raw) - 8) // 4}I", raw[8:])

    chain, total, plain = seed, 0, []
    for word in cipher[:-1]:
        plain.append(_decipher(chain, word))
        total = (total + plain[-1]) & WORD
        chain = word
    return seed, struct.pack(f">{len(plain)}I", *plain), _decipher(chain, cipher[-1]), total


def wrap(container):
    """The ELUP payload for a container: its big-endian length, then the bytes."""
    payload = struct.pack(">I", len(container)) + container
    return payload + b"\x00" * (-len(payload) % 4)


def unwrap(payload):
    length, = struct.unpack(">I", payload[:4])
    return payload[4:4 + length]


def verify(official):
    """Re-derive an official .bin from the container inside it.

    ☠ This is the whole warrant for the files this script writes. The cipher is
    read out of the firmware, and the only way to know the FORWARD direction is
    right is to run it on Elektron's own artifact and demand the bytes back.
    """
    raw = Path(official).read_bytes()
    try:
        seed, payload, stored, total = decode(raw)
    except ValueError as e:
        print(f"{official}: {e}")
        return 1
    container = unwrap(payload)
    print(f"official  : {official} ({len(raw):,} bytes, seed {seed:#010x})")
    print(f"container : {len(container):,} bytes  "
          f"({container[:18].decode('ascii', 'replace')})")
    print(f"checksum  : {stored:#010x} stored, {total:#010x} summed"
          f"{'' if stored == total else '  DISAGREE'}")

    rebuilt, _ = encode(wrap(container), seed)
    if rebuilt == raw:
        print(f"\nIDENTICAL — all {len(raw):,} bytes regenerated from that container "
              f"alone, so the forward encoder is right")
        return 0
    differ = sum(a != b for a, b in zip(rebuilt, raw))
    print(f"\nMISMATCH — {len(rebuilt):,} bytes against {len(raw):,}, "
          f"{differ} differing bytes in the overlap")
    return 1


def build(container_path, out_path, seed):
    container = Path(container_path).read_bytes()
    if container[:4] != b"ELEK":
        sys.exit(f"{container_path} does not start with 'ELEK': {container[:8]!r}")

    payload = wrap(container)
    pad = len(payload) - 4 - len(container)
    print(f"container : {len(container):,} bytes  "
          f"({container[:18].decode('ascii', 'replace')})")
    print(f"payload   : {len(payload):,} bytes"
          + (f"  (+{pad} pad bytes to a word boundary)" if pad else ""))

    raw, total = encode(payload, seed)
    print(f"checksum  : {total:#010x}")

    # Read it back with the decoder before claiming anything: an encoder that
    # writes a file no reader accepts is the one failure mode that matters, and
    # it costs microseconds to exclude.
    _, back, stored, summed = decode(raw)
    if back != payload or stored != summed != total:
        sys.exit("refusing to write a file that does not decode back to its input")

    Path(out_path).write_bytes(raw)
    print(f"round-trip: payload ok, checksum ok")
    print(f"\nwrote {out_path} ({len(raw):,} bytes)")
    print("Copy it to the ROOT of the CF card, then PROJECT -> OS UPGRADE -> [YES].")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("container", help="ELEK container (elektron-firmware-tool -e), "
                                     "or an official .bin with --verify")
    ap.add_argument("-o", "--out", help="output .bin")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=STOCK_SEED,
                    help="feedback seed (default: the stock 1.40C seed)")
    ap.add_argument("--verify", action="store_true",
                    help="treat the argument as an OFFICIAL .bin: take its container "
                         "out, re-encode it here, and require the result to match "
                         "the official file byte for byte")
    args = ap.parse_args()

    if args.verify:
        sys.exit(verify(args.container))
    if not args.out:
        ap.error("-o/--out is required unless --verify is given")
    build(args.container, args.out, args.seed)


if __name__ == "__main__":
    main()
