#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""EMAC differential fuzz: two qemu-system-m68k builds must agree bit for bit.

emac-conform.py checks EMAC against the manual, one rule per vector. This checks
a translator change against a reference build instead: thousands of random MAC
sequences in every MACSR mode, including latched saturation, extreme operands
and MAC-with-load, with every accumulator read back both through the current
mode (move.l %accN — the fractional store) and raw. Written for the inline
fractional EMAC (patches/qemu/0015), which must match the helpers it replaced.

Usage:
  emac-diff.py --ref PATH [--qemu PATH] [--cases N] [--seed S]
"""
from __future__ import annotations

import importlib.util
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location(
    "emac_conform", ROOT / "tests" / "emac-conform.py")
conform = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(conform)

EXTREMES = [0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x00008000, 0x00007FFF,
            0xFFFF8000, 0x40000000, 0xC0000000, 0x00010000, 0x7FFF0000]
TABLE_LONGS = 64


def val(rng: random.Random) -> int:
    return rng.choice(EXTREMES) if rng.random() < 0.3 else rng.getrandbits(32)


def mac_op(rng: random.Random) -> str:
    acc = f"%acc{rng.randrange(4)}"
    op = rng.choice(["mac", "msac"])
    scale = rng.choice(["", "", "<<,", ">>,"])
    if rng.random() < 0.5:
        rx, ry = rng.sample(["%d0", "%d1", "%a2"], 2)
        size = ".l"
    else:
        rx = rng.choice(["%d0", "%d1", "%a2"]) + rng.choice("ul")
        ry = rng.choice(["%d0", "%d1"]) + rng.choice("ul")
        size = ".w"
    if rng.random() < 0.25:
        load = f"{rng.choice(['%a1@+', '%a1@-', '%a1@'])},{rng.choice(['%d3', '%d4'])},"
    else:
        load = ""
    return f"    {op}{size} {rx},{ry},{scale}{load}{acc}"


def gen_case(i: int, rng: random.Random) -> str:
    mode = rng.randrange(16) << 4                 # OMC SU FI RT
    if rng.random() < 0.2:
        mode |= rng.randrange(16) << 8            # latched PAV0-3
    lines = [f"\n| ---- case {i} ----", "    move.l  #0,%d0", "    move.l  %d0,%macsr"]
    for n in range(4):
        lines += [f"    move.l  #{val(rng):#010x},%d0", f"    move.l  %d0,%acc{n}"]
    for ext in ("%accext01", "%accext23"):
        e = rng.choice([0, 0xFFFFFFFF, rng.getrandbits(32)])
        lines += [f"    move.l  #{e:#010x},%d0", f"    move.l  %d0,{ext}"]
    lines += [f"    move.l  #{mode:#06x},%d0", "    move.l  %d0,%macsr"]
    for reg in ("%d0", "%d1", "%d3", "%d4", "%a2"):
        lines.append(f"    move.l  #{val(rng):#010x},{reg}")
    lines += ["    lea     table,%a1", f"    add.l   #{TABLE_LONGS * 2},%a1",
              "    move.l  #0,%d5"]
    for _ in range(rng.randint(1, 6)):
        if rng.random() < 0.1:
            lines.append(f"    movclr.l %acc{rng.randrange(4)},%d5")
        else:
            lines.append(mac_op(rng))
    # Everything a MAC can touch: MACSR, accumulators through the current mode
    # (fractional store and saturation), then raw with the mode cleared.
    reads = ["%macsr"] + [f"%acc{n}" for n in range(4)]
    for r in reads:
        lines += [f"    move.l  {r},%d2", "    jsr     puthex8", "    jsr     sp"]
    for r in ("%d3", "%d4", "%d5", "%a1"):
        lines += [f"    move.l  {r},%d2", "    jsr     puthex8", "    jsr     sp"]
    lines += ["    move.l  #0,%d0", "    move.l  %d0,%macsr"]
    for r in [f"%acc{n}" for n in range(4)] + ["%accext01", "%accext23"]:
        lines += [f"    move.l  {r},%d2", "    jsr     puthex8", "    jsr     sp"]
    lines.append("    jsr     nl")
    return "\n".join(lines)


def build_asm(cases: int, seed: int) -> str:
    rng = random.Random(seed)
    body = [gen_case(i, rng) for i in range(cases)]
    table = ", ".join(f"{rng.getrandbits(32):#010x}" for _ in range(TABLE_LONGS * 2))
    return (conform.PROLOGUE + "\n".join(body)
            + "\n    lea     end_msg,%a2\n    jsr     putstr\n"
            + conform.EPILOGUE
            + "\nsp:\n    move.l  #32,%d6\n    bsr     putc\n    rts\n"
            + "\n    .section .rodata\nend_msg:\n    .asciz  \"END\"\n"
            + f"    .align 4\ntable:\n    .long {table}\n")


def assemble(asm: str, d: Path) -> Path:
    (d / "t.s").write_text(asm)
    (d / "t.ld").write_text("SECTIONS { . = %#x; .text : { *(.text) } "
                            ".rodata : { *(.rodata) } }\n" % conform.RAM_BASE)
    for cmd in (["m68k-elf-as", "-mcpu=54454", "-o", str(d / "t.o"), str(d / "t.s")],
                ["m68k-elf-ld", "-T", str(d / "t.ld"), "-o", str(d / "t.elf"),
                 str(d / "t.o")]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"{cmd[0]} failed:\n{r.stderr}")
    return d / "t.elf"


def run(qemu: Path, elf: Path) -> list[str]:
    """Run until the END marker: `halt` never exits QEMU, so stop it ourselves."""
    p = subprocess.Popen([str(qemu), "-M", "mcf5208evb", "-kernel", str(elf),
                          "-display", "none", "-monitor", "none",
                          "-serial", "stdio", "-no-reboot"],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    out = []
    try:
        for line in p.stdout:
            if line.startswith("END"):
                return out
            out.append(line.rstrip("\n"))
    finally:
        p.kill()
        p.wait()
    sys.exit(f"{qemu}: output ended without END after {len(out)} cases")


def main() -> int:
    argv = sys.argv[1:]
    opt = lambda k, d: argv[argv.index(k) + 1] if k in argv else d
    if "--ref" not in argv:
        sys.exit(__doc__)
    ref, qemu = Path(opt("--ref", "")), Path(opt("--qemu", conform.DEFAULT_QEMU))
    cases, seed = int(opt("--cases", 4000)), int(opt("--seed", 1))
    asm = build_asm(cases, seed)
    with tempfile.TemporaryDirectory() as td:
        elf = assemble(asm, Path(td))
        a, b = run(ref, elf), run(qemu, elf)
    if len(a) != cases or len(b) != cases:
        print(f"FAIL: case count ref={len(a)} new={len(b)} want={cases}")
        return 1
    bad = [i for i in range(cases) if a[i] != b[i]]
    names = "macsr acc0 acc1 acc2 acc3 d3 d4 d5 a1 raw0 raw1 raw2 raw3 ext01 ext23".split()
    for i in bad[:5]:
        print(f"MISMATCH case {i} (seed {seed}):")
        for n, x, y in zip(names, a[i].split(), b[i].split()):
            print(f"  {n:6} ref {x}  new {y}{'   <--' if x != y else ''}")
        blk = asm.split(f"| ---- case {i} ----")[1].split("| ---- case")[0]
        print("  program:" + blk.split("move.l  #0,%d5")[1].split("move.l  %macsr")[0])
    print(f"{cases - len(bad)}/{cases} cases identical (seed {seed})")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
