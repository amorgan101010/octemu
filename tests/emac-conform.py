#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""EMAC conformance harness — emulator milestone M1.

Turns tests/emac.toml from prose into an executable acceptance test. Each
vector below cites the MCF54455RM rule it checks, so a failure names the rule it
violates rather than just a number.

WHY THIS EXISTS. QEMU's ColdFire EMAC has at least five defects. Three are
fixed by patches/qemu/0001-coldfire-emac-isa-c.patch; the rest are specified in
tests/emac.toml but not yet implemented. Measuring the emulator to find out what it
should do would only re-derive its bugs — so every expectation here comes from the
vendor's manual, never from observation.

HOW IT WORKS. Generates one bare-metal ColdFire program that runs every vector
and prints each result as hex over the mcf5208evb's UART0 (TX buffer at
0xfc06000c) to stdout. There is no other channel: QEMU's m68k cpu_dump_state does
not print EMAC registers.

  vectors -> m68k asm -> m68k-elf-as -mcpu=54454 -> ELF
          -> qemu-system-m68k -M mcf5208evb -kernel ... -nographic
          -> parse stdout -> compare against the RM

Usage:
  emac_conform.py                  build, run, compare
  emac_conform.py --asm            print the generated assembly and stop
  emac_conform.py --qemu PATH      use a specific qemu-system-m68k
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REF = Path(__file__).resolve().parent / "emac.toml"
DEFAULT_QEMU = ROOT / "vendor" / "qemu" / "build" / "qemu-system-m68k"

RAM_BASE = 0x40000000       # mcf5208evb SDRAM, same base as the Octatrack's
UART_TX = 0xFC06000C        # mcf5208evb UART0 transmit buffer

# Fractional constants, Q1.31
Q_ONE_MINUS = 0x7FFFFFFF    # ~ +1.0
Q_HALF = 0x40000000         # +0.5
Q_NEG_HALF = 0xC0000000     # -0.5


class Vector:
    """One conformance check.

    expect=None means OBSERVE ONLY -- the value is reported, not asserted,
    because the manual does not pin it down unambiguously. Being explicit about
    that is the point; a test that asserts a guess is worse than no test.
    """

    def __init__(self, name, macsr, acc, d0, d1, insn, expect, rule, ext=0,
                 read="acc", repeat=1, mask=0xFFFFFFFF, known_fail=None):
        self.name = name
        self.macsr = macsr
        self.acc = acc
        self.ext = ext
        self.d0 = d0
        self.d1 = d1
        self.insn = insn
        self.expect = expect
        self.rule = rule
        self.read = read          # "acc" | "ext" | "macsr"
        self.repeat = repeat
        self.mask = mask          # compare only these bits
        # A defect that is SPECIFIED but not yet implemented. Reported loudly and
        # separately from a regression, and does not fail the run -- but it must
        # never read as "verified".
        self.known_fail = known_fail


VECTORS = [
    # ---- the three defects the patch fixes -------------------------------
    Vector("int_mac", 0x00, 100, 3, 5, "mac.l  %d0,%d1,%acc0",
           100 + 3 * 5, "integer MAC accumulates: 100 + 3*5"),
    Vector("int_msac", 0x00, 100, 3, 5, "msac.l %d0,%d1,%acc0",
           100 - 3 * 5, "MSAC SUBTRACTS. QEMU executed it as MAC for every "
                        "form (translate.c tested the opword, not the ext word)"),
    Vector("frac_mac_scale", 0x20, 0, Q_ONE_MINUS, Q_ONE_MINUS,
           "mac.l  %d0,%d1,%acc0", 0x7FFFFFFE,
           "fractional 1.31x1.31 -> 2.62 has a redundant sign bit; the "
           "mandatory <<1 normalises to 1.63. Without it results are 6 dB low"),
    Vector("frac_signed", 0x20, 0, Q_NEG_HALF, Q_HALF,
           "mac.l  %d0,%d1,%acc0", 0xE0000000,
           "RM Table 5-3: fractional operands are ALWAYS SIGNED regardless of "
           "S/U. (-0.5)*(+0.5) = -0.25"),

    # ---- S/U is NOT a signedness control in fractional mode -------------
    Vector("frac_su_still_signed", 0x60, 0, Q_NEG_HALF, Q_HALF,
           "mac.l  %d0,%d1,%acc0", 0x0000E000,
           "RM Table 5-3: S/U=1 F/I=1 is STILL 'Signed, fractional' -- S/U "
           "selects round-to-16-bits on accumulator stores, not signedness. "
           "-0.25 -> 16-bit 0xE000 in the low word, upper word zero-filled "
           "(RM S/U description). 0xE000 as Q1.15 is -0.25, so the sign survives"),

    # ---- flags: RM Table 5-2 --------------------------------------------
    Vector("n_flag_negative", 0x20, 0, Q_NEG_HALF, Q_HALF,
           "mac.l  %d0,%d1,%acc0", 1 << 3, mask=1 << 3,
           rule="MACSR[N] bit 3 must be SET: 'Set if the msb of the result is "
                "set' (RM Table 5-2)", read="macsr"),
    Vector("z_flag_zero", 0x20, 0, 0, Q_HALF,
           "mac.l  %d0,%d1,%acc0", 1 << 2, mask=1 << 2,
           rule="MACSR[Z] bit 2 must be SET: result is zero (RM Table 5-2)",
           read="macsr"),

    # ---- the fifth defect, found by reading the RM ----------------------
    Vector("accext_neg_packing", 0x20, 0, Q_NEG_HALF, Q_HALF,
           "mac.l  %d0,%d1,%acc0", 0xFF000000, mask=0xFF000000,
           rule="RM Table 5-6: ACC0U is bits 31-24 of ACCext01, so a NEGATIVE "
                "acc0's sign-extension byte must appear there", read="ext",
           # Kept as a tripwire, not as an excuse. This vector PASSES with
           # 0005-emac-accext-packing.patch applied; if it starts reporting
           # known-unfixed again, that patch has gone missing from the build.
           known_fail="REGRESSION: stock QEMU returns 0x0000FF00 (ACC0's "
                      "extension 16 bits low, in ACC1U). Fixed by "
                      "patches/qemu/0004-emac-accext-packing.patch -- if you are "
                      "seeing this, that patch is NOT in your build"),

    # ---- saturation: RM Table 5-2 OMC -----------------------------------
    Vector("saturate_positive", 0xA0, Q_ONE_MINUS, Q_ONE_MINUS, Q_ONE_MINUS,
           "mac.l  %d0,%d1,%acc0", 0x7FFFFFFF,
           rule="OMC=1: on overflow a signed accumulator saturates to "
                "0x7FFFFFFF (RM Table 5-2)",
           known_fail="CONFIRMED: returns 0x807FFFFD -- does not saturate"),
    Vector("saturate_sticky", 0xA0, Q_ONE_MINUS, Q_ONE_MINUS, Q_ONE_MINUS,
           "mac.l  %d0,%d1,%acc0", 0x7FFFFFFF, repeat=3,
           rule="☠ RM Table 5-2 OMC: 'After saturation, the accumulator remains "
                "unaffected by any other MAC or MSAC instructions until the "
                "overflow bit is cleared or the accumulator is directly loaded.' "
                "Saturation LATCHES -- three ops must equal one",
           known_fail="CONFIRMED: returns 0x00000000 -- neither saturates nor "
                      "latches. Only shows up after the SECOND overflow"),
]


PROLOGUE = r"""
| EMAC conformance, generated by tests/emac-conform.py -- do not hand-edit.
| Runs on qemu-system-m68k -M mcf5208evb. Results go out UART0 as hex.
    .text
    .globl  _start
_start:
    move.l  #0x40800000,%sp             | stack well above the code

| Bring UART0 up enough to transmit. In QEMU the TX buffer write is immediate,
| so no status polling is required, but reset TX/RX for good order.
    lea     0xfc060000,%a1
    move.b  #0x30,%a1@(8)               | UCR: reset transmitter
    move.b  #0x20,%a1@(8)               | UCR: reset receiver
    move.b  #0x13,%a1@(0)               | UMR1: 8 bits, no parity
    move.b  #0x07,%a1@(0)               | UMR2: 1 stop bit
    move.b  #0x05,%a1@(8)               | UCR: enable TX + RX
"""

EPILOGUE = r"""
    bsr     nl
    halt

| ---- putc: %d6 low byte -> UART0 ------------------------------------------
putc:
    lea     0xfc06000c,%a0
    move.b  %d6,%a0@
    rts

nl:
    move.l  #10,%d6
    bsr     putc
    rts

| ---- puthex8: %d2 -> eight hex digits ------------------------------------
puthex8:
    move.l  #28,%d5
ph_loop:
    move.l  %d2,%d6
    lsr.l   %d5,%d6
    and.l   #15,%d6
    add.l   #48,%d6                     | '0'
    cmp.l   #57,%d6                     | '9'
    ble     ph_emit
    add.l   #7,%d6                      | -> 'A'..'F'
ph_emit:
    bsr     putc
    subq.l  #4,%d5
    bge     ph_loop
    rts

| ---- putstr: %a2 -> NUL-terminated string --------------------------------
putstr:
    clr.l   %d6
ps_loop:
    move.b  %a2@+,%d6
    beq     ps_done
    bsr     putc
    bra     ps_loop
ps_done:
    rts
"""


def gen_test(i: int, v: Vector) -> tuple[str, str]:
    """Return (code, rodata) for one vector."""
    label = f"t{i}"
    read_reg = {"acc": "%acc0", "ext": "%accext01", "macsr": "%macsr"}[v.read]
    body = [
        f"\n| ---- {v.name} ----",
        f"    lea     {label}_name,%a2",
        "    bsr     putstr",
        # Clear saturation state and any latched flags before each vector:
        # writing MACSR clears V, and loading the accumulator directly unlatches
        # saturation (RM Table 5-2, OMC).
        "    move.l  #0,%d0",
        "    move.l  %d0,%macsr",
        f"    move.l  #{v.ext:#010x},%d0",
        "    move.l  %d0,%accext01",
        f"    move.l  #{v.acc:#010x},%d0",
        "    move.l  %d0,%acc0",
        f"    move.l  #{v.macsr:#04x},%d0",
        "    move.l  %d0,%macsr",
        f"    move.l  #{v.d0:#010x},%d0",
        f"    move.l  #{v.d1:#010x},%d1",
    ]
    body += [f"    {v.insn}"] * v.repeat
    body += [
        f"    move.l  {read_reg},%d2",
        "    bsr     puthex8",
        "    bsr     nl",
    ]
    rodata = f'{label}_name:\n    .asciz  "{v.name}="\n'
    return "\n".join(body), rodata


def build_asm() -> str:
    code, rodata = [], []
    for i, v in enumerate(VECTORS):
        c, r = gen_test(i, v)
        code.append(c)
        rodata.append(r)
    return (
        PROLOGUE
        + "\n".join(code)
        + EPILOGUE
        + "\n    .section .rodata\n"
        + "".join(rodata)
    )


def run(qemu: Path) -> str:
    asm = build_asm()
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        (d / "t.s").write_text(asm)
        ld = d / "t.ld"
        ld.write_text(
            "SECTIONS { . = %#x; .text : { *(.text) } "
            ".rodata : { *(.rodata) } }\n" % RAM_BASE
        )
        for cmd in (
            ["m68k-elf-as", "-mcpu=54454", "-o", str(d / "t.o"), str(d / "t.s")],
            ["m68k-elf-ld", "-T", str(ld), "-o", str(d / "t.elf"), str(d / "t.o")],
        ):
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode:
                sys.exit(f"{cmd[0]} failed:\n{r.stderr}")
        # -nographic would multiplex the monitor onto stdio and fight the serial
        # line for it; take the serial port and disable the rest.
        cmd = [str(qemu), "-M", "mcf5208evb", "-kernel", str(d / "t.elf"),
               "-display", "none", "-monitor", "none", "-serial", "stdio",
               "-no-reboot"]
        # ColdFire `halt` stops the CPU but does NOT terminate QEMU, and this
        # board's reset controller only reloads PC from address 0 — so there is
        # no in-guest exit. The timeout IS the termination signal, and it is
        # sound because every result is written to the UART before the halt.
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            return r.stdout + r.stderr
        except subprocess.TimeoutExpired as exc:
            def dec(b):
                if b is None:
                    return ""
                return b.decode(errors="replace") if isinstance(b, bytes) else b
            return dec(exc.stdout) + dec(exc.stderr)


def main() -> int:
    argv = sys.argv[1:]
    if "--asm" in argv:
        print(build_asm())
        return 0

    qemu = DEFAULT_QEMU
    if "--qemu" in argv:
        qemu = Path(argv[argv.index("--qemu") + 1])
    if not qemu.exists():
        alt = shutil.which("qemu-system-m68k")
        if not alt:
            sys.exit(f"no qemu-system-m68k at {qemu} and none on PATH — "
                     "build vendor/qemu first: run 'make qemu'")
        qemu = Path(alt)

    spec = tomllib.loads(REF.read_text())["claim"]
    out = run(qemu)
    got = dict(re.findall(r"^(\w+)=([0-9A-F]{8})", out, re.M))

    w = 74
    print("┌" + "─" * w + "┐")
    print("│ EMAC conformance — M1".ljust(w + 1) + "│")
    print("├" + "─" * w + "┤")
    print(f"│ qemu {str(qemu):<{w - 7}}"[: w + 1] + "│")
    print(f"│ spec {'tests/emac.toml, from MCF54455RM ch.5':<{w - 7}}" + "│")
    print("└" + "─" * w + "┘\n")

    if not got:
        print("no results parsed. Raw output:\n")
        print(out[:2000])
        return 1

    npass = nregress = nknown = nobs = 0
    for v in VECTORS:
        val = got.get(v.name)
        if val is None:
            print(f"  ◯ {v.name:<24} NO RESULT")
            nregress += 1
            continue
        actual = int(val, 16)
        if v.expect is None:
            print(f"  ◆  {v.name:<24} 0x{actual:08X}   observe-only")
            print(f"     {v.rule}")
            nobs += 1
            continue
        want = v.expect & v.mask
        hit = (actual & v.mask) == want
        masknote = "" if v.mask == 0xFFFFFFFF else f" (mask {v.mask:#010x})"
        if hit:
            print(f"  ✓ {v.name:<24} 0x{actual:08X}{masknote}")
            npass += 1
        elif v.known_fail:
            print(f"  ⏸ {v.name:<24} 0x{actual:08X}  want "
                  f"0x{want:08X}{masknote}")
            print(f"     KNOWN-UNFIXED: {v.known_fail}")
            print(f"     RULE: {v.rule}")
            nknown += 1
        else:
            print(f"  ✗ {v.name:<24} 0x{actual:08X}  want "
                  f"0x{want:08X}{masknote}   REGRESSION")
            print(f"     RULE: {v.rule}")
            nregress += 1

    print(f"\n  {npass} pass · {nregress} regression · "
          f"{nknown} known-unfixed · {nobs} observe-only")
    if nknown:
        print(f"\n  ⏸ {nknown} defect(s) are SPECIFIED in tests/emac.toml and NOT")
        print("     yet implemented in patches/qemu/. This run must never be read")
        print("     as 'EMAC verified' while that count is non-zero. M1 is")
        print("     complete only at 0 regressions AND 0 known-unfixed.")
    return 1 if nregress else 0


if __name__ == "__main__":
    sys.exit(main())
