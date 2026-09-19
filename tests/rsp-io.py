#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Sequential guest-memory reads and pokes over the QEMU gdbstub — raw RSP,
no gdb needed. Every USB gate observes the guest through this.

  octemu --gdb 3333 ... &
  tests/rsp-io.py 3333  r:80000034:4  w:8000005f=7f  r:80005460:0x100

Reads print `ADDR: hexbytes`; writes are verified with a readback and the
script exits nonzero if any verify fails. The target halts on attach and is
resumed before the socket closes, so a probe costs the guest ~a blink.

☠ The stub can emit an ASYNC stop packet (`T05...`) besides the `?` reply;
naively taking packets in order shifts every later answer by one. Anything
shaped like a stop reply is drained, not returned.
"""
import socket, sys

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

    def recv(self, timeout=10):
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

    def cmd(self, p, timeout=10):
        self.send(p)
        while True:
            rep = self.recv(timeout)
            if not is_stop(rep):
                return rep

def main():
    r = Rsp(int(sys.argv[1]))
    r.send("?")
    r.recv(10)
    ok = True
    for op in sys.argv[2:]:
        if op.startswith("r:"):
            _, a, ln = op.split(":")
            addr, n = int(a, 16), int(ln, 0)
            out = []
            while n:
                k = min(n, 256)
                out.append(r.cmd(f"m{addr:x},{k:x}") or "?")
                addr += k
                n -= k
            print(f"{a}: {''.join(out)}", flush=True)
        elif op.startswith("w:"):
            a, v = op[2:].split("=")
            addr = int(a, 16)
            if len(v) % 2:
                v = "0" + v
            k = len(v) // 2
            old = r.cmd(f"m{addr:x},{k:x}")
            rep = r.cmd(f"M{addr:x},{k:x}:{v}")
            new = r.cmd(f"m{addr:x},{k:x}")
            print(f"W {a}: {old} -> {new} (want {v}, {rep})", flush=True)
            if new != v:
                ok = False
        else:
            print(f"bad op {op}", file=sys.stderr)
            ok = False
    r.send("c")
    r.s.close()
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
