#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Canary stamp/check over the QEMU gdbstub — how a region of guest memory is
shown to be free for something else to live in.

Stamps a deterministic pattern (word at A = A ^ 0xC3A5965A, so verification
needs no stored copy) over slices of guest memory, records exactly what was
stamped in a manifest, and later re-reads every stamped byte and names each
slice INTACT or CLOBBERED (with the first differing offset and a count).

  tests/canary.py PORT stamp --manifest M.json SPEC...
  tests/canary.py PORT check --manifest M.json [--label WHEN]

SPEC is LO:HI:STEP:LEN (a LEN-byte slice every STEP from LO) or LO:HI:dense
(every byte). All ops run in ONE gdbstub connection: the guest is halted for
the whole call and resumes when the socket closes, so guest-time waits in a
running walk barely advance while a stamp/check is in flight.

☠ Every write is verified by readback at stamp time — a stamp that did not
land would turn into a false CLOBBERED at check time. ☠ Do not stamp live
structures the firmware or the emulated controllers walk between stamp and
check unless the clobber is the measurement (the FAT RAM copy flushes to the
card image: stamping it corrupts the filesystem, not just the canary).
"""
import json, socket, sys

MAGIC = 0xC3A5965A
# gdbstub advertises PacketSize=0x20020, so an m/M packet carries ~64 KB of
# payload; 0x8000 keeps a safe margin (hex-doubled + header) and cuts a dense
# 108 MB stamp from ~440k round trips to ~3.3k. ☠ Bumped 2026-09-07 when a
# 256 B chunk made the dense preflight stamp take >10 min of Python CPU.
CHUNK = 0x8000


def csum(b):
    return sum(b) % 256


def is_stop(pkt):
    return pkt and pkt[0] in "ST" and (":" in pkt or len(pkt) == 3)


class Rsp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=20)
        self.buf = b""

    def send(self, p):
        self.s.sendall(b"$" + p.encode() + b"#" + b"%02x" % csum(p.encode()))

    def recv(self, timeout=15):
        self.s.settimeout(timeout)
        try:
            while True:
                i = self.buf.find(b"$")
                if i >= 0:
                    j = self.buf.find(b"#", i)
                    if j >= 0 and len(self.buf) >= j + 3:
                        pkt = self.buf[i + 1:j].decode()
                        self.buf = self.buf[j + 3:]
                        self.s.sendall(b"+")
                        return pkt
                c = self.s.recv(4096)
                if not c:
                    return None
                self.buf += c
        except socket.timeout:
            return None

    def cmd(self, p, timeout=15):
        self.send(p)
        while True:
            rep = self.recv(timeout)
            if not is_stop(rep):
                return rep

    def read(self, addr, n):
        out = b""
        while n:
            k = min(n, CHUNK)
            rep = self.cmd(f"m{addr:x},{k:x}")
            if not rep or not all(c in "0123456789abcdefABCDEF" for c in rep):
                raise RuntimeError(f"read failed at {addr:#x}: {rep!r}")
            out += bytes.fromhex(rep)
            addr += k
            n -= k
        return out

    def write(self, addr, data):
        off = 0
        while off < len(data):
            k = min(len(data) - off, CHUNK)
            rep = self.cmd(f"M{addr + off:x},{k:x}:{data[off:off + k].hex()}")
            if rep != "OK":
                raise RuntimeError(f"write failed at {addr + off:#x}: {rep!r}")
            off += k


def pattern(addr, n):
    out = bytearray()
    a = addr & ~3
    while len(out) < n + 8:
        w = (a ^ MAGIC) & 0xFFFFFFFF
        out += w.to_bytes(4, "big")
        a += 4
    lead = addr - (addr & ~3)
    return bytes(out[lead:lead + n])


def parse_spec(spec):
    parts = spec.split(":")
    lo, hi = int(parts[0], 16), int(parts[1], 16)
    slices = []
    if parts[2] == "dense":
        slices.append((lo, hi - lo))
    else:
        step, ln = int(parts[2], 0), int(parts[3], 0)
        a = lo
        while a < hi:
            slices.append((a, min(ln, hi - a)))
            a += step
    return slices


def main():
    port = int(sys.argv[1])
    mode = sys.argv[2]
    args = sys.argv[3:]
    manifest = None
    label = ""
    specs = []
    i = 0
    while i < len(args):
        if args[i] == "--manifest":
            manifest = args[i + 1]
            i += 2
        elif args[i] == "--label":
            label = args[i + 1]
            i += 2
        else:
            specs.append(args[i])
            i += 1
    if not manifest:
        sys.exit("need --manifest")

    r = Rsp(port)
    r.send("?")
    r.recv(10)

    if mode == "stamp":
        slices = [s for spec in specs for s in parse_spec(spec)]
        for addr, ln in slices:
            r.write(addr, pattern(addr, ln))
            back = r.read(addr, ln)
            if back != pattern(addr, ln):
                sys.exit(f"stamp verify FAILED at {addr:#x} — refusing to "
                         f"record a canary that never landed")
        json.dump({"slices": [[a, n] for a, n in slices]}, open(manifest, "w"))
        print(f"stamped {len(slices)} slices, "
              f"{sum(n for _, n in slices)} bytes, manifest {manifest}")
    elif mode == "check":
        slices = json.load(open(manifest))["slices"]
        clob = 0
        for addr, ln in slices:
            back = r.read(addr, ln)
            want = pattern(addr, ln)
            if back == want:
                continue
            clob += 1
            first = next(k for k in range(ln) if back[k] != want[k])
            ndiff = sum(1 for k in range(ln) if back[k] != want[k])
            print(f"CLOBBERED {addr:#010x}+{ln:#x} first@+{first:#x} "
                  f"({addr + first:#010x}) {ndiff}/{ln} bytes differ "
                  f"[{label}]")
        print(f"checked {len(slices)} slices: {len(slices) - clob} intact, "
              f"{clob} clobbered [{label}]")
    else:
        sys.exit(f"unknown mode {mode}")
    r.send("c")  # resume via detach-equivalent; socket close also resumes
    r.s.close()


if __name__ == "__main__":
    main()
