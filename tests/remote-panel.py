#!/usr/bin/env python3
"""The remote panel (src/remote.c), end to end, from a stand-in browser.

Boots octemu headless with --remote, then over plain sockets:
  - GET / , /panel.svg and /elements.json answer 200
  - the WebSocket handshake answers 101 with the RFC 6455 accept key
  - the first message is the 'L' layout, naming every lamp and key
  - the state frame has the documented length, and the screen gets drawn
  - 'b btn-func 1' shows FUNC (key 45) held in the state's key bytes
  - dropping the connection lets go of it: never a key stuck down
  - a crossfader move comes back in the state
  - a page that goes silent with its socket open (an iPad put to sleep) is
    dropped, and lets go of its keys

Run from the repo root: python3 tests/remote-panel.py [--port N]
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import time

GUID = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
NLAMPS = 53
STATE_BYTES = 1 + 1024 + 8 + 1 + 1 + NLAMPS * 3
FUNC = 45


def fail(msg):
    print('FAIL: ' + msg)
    sys.exit(1)


def http_get(port, path):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    s.sendall(('GET %s HTTP/1.1\r\nHost: x\r\n\r\n' % path).encode())
    data = b''
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    s.close()
    head, _, body = data.partition(b'\r\n\r\n')
    return head.split(b'\r\n')[0].decode(), body


class Page:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(('GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n'
                        'Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n'
                        'Sec-WebSocket-Version: 13\r\n\r\n' % key).encode())
        head = b''
        while b'\r\n\r\n' not in head:
            head += self.s.recv(1)
        lines = head.decode().split('\r\n')
        if not lines[0].startswith('HTTP/1.1 101'):
            fail('handshake answered %r' % lines[0])
        want = base64.b64encode(hashlib.sha1(key.encode() + GUID).digest()).decode()
        got = [l.split(':', 1)[1].strip() for l in lines
               if l.lower().startswith('sec-websocket-accept:')]
        if got != [want]:
            fail('accept key %r, want %r' % (got, want))

    def _recv(self, n):
        out = b''
        while len(out) < n:
            chunk = self.s.recv(n - len(out))
            if not chunk:
                raise EOFError
            out += chunk
        return out

    def recv(self):
        """-> (opcode, payload); answers pings on the way."""
        while True:
            b0, b1 = self._recv(2)
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack('!H', self._recv(2))[0]
            elif n == 127:
                n = struct.unpack('!Q', self._recv(8))[0]
            payload = self._recv(n)
            op = b0 & 0x0F
            if op == 0x9:
                self.send(payload, 0xA)
                continue
            return op, payload

    def send(self, data, op=0x1):
        if isinstance(data, str):
            data = data.encode()
        mask = os.urandom(4)
        head = bytes([0x80 | op])
        n = len(data)
        if n < 126:
            head += bytes([0x80 | n])
        else:
            head += bytes([0x80 | 126]) + struct.pack('!H', n)
        self.s.sendall(head + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(data)))

    def state_until(self, pred, secs, what):
        end = time.time() + secs
        while time.time() < end:
            op, p = self.recv()
            if op == 0x2:
                if len(p) != STATE_BYTES:
                    fail('state frame is %d bytes, want %d' % (len(p), STATE_BYTES))
                if pred(p):
                    return p
        fail('timed out waiting for ' + what)

    def close(self):
        self.s.close()


def key_held(state, n):
    return (state[1025 + (n >> 3)] >> (n & 7)) & 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=18798)
    args = ap.parse_args()
    env = dict(os.environ, SDL_AUDIODRIVER='dummy')
    emu = subprocess.Popen(['./octemu', '--headless', '--cf-card', 'none',
                            '--nvram', 'none', '--remote', '--remote-port',
                            str(args.port), '--timeout', '60'],
                           stderr=subprocess.PIPE, text=True, env=env)
    try:
        port = None
        end = time.time() + 20
        while time.time() < end and port is None:
            line = emu.stderr.readline()
            if not line:
                break
            if 'remote panel at' in line:
                port = int(line.rsplit(':', 1)[1].strip().rstrip('/'))
        if port is None:
            fail('octemu never said where the remote panel is')
        print('remote panel on port %d' % port)

        for path, needle in (('/', b'Octatrack remote'), ('/panel.svg', b'fader-handle'),
                             ('/elements.json', b'knob-level'), ('/ping', b'ok')):
            status, body = http_get(port, path)
            if '200' not in status or needle not in body:
                fail('GET %s -> %s' % (path, status))
        if '404' not in http_get(port, '/nope')[0]:
            fail('GET /nope is not a 404')
        print('ok: page, drawing and geometry served')

        # Firefox's HTTPS-First opens with a TLS hello: it must be refused at
        # once, or every https:// try costs the request deadline (5 s).
        t = time.time()
        s = socket.create_connection(('127.0.0.1', port), timeout=5)
        s.sendall(b'\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03' + os.urandom(32))
        try:
            gone = s.recv(1) == b''
        except ConnectionResetError:
            gone = True
        s.close()
        if not gone or time.time() - t > 1:
            fail('a TLS hello was not refused at once (%.1f s)' % (time.time() - t))
        print('ok: a TLS hello is refused at once (%.3f s)' % (time.time() - t))

        page = Page(port)
        op, p = page.recv()
        if op != 0x1 or not p.startswith(b'L '):
            fail('first message is not the layout: %r' % p[:40])
        lay = json.loads(p[2:])
        if len(lay['lamps']) != NLAMPS or lay['keys'].get('btn-func') != FUNC \
                or lay['keys'].get('trig-16') != 15 or lay['keys'].get('btn-t8') != 23:
            fail('layout is wrong: %r' % lay)
        print('ok: layout names %d lamps and %d keys' % (len(lay['lamps']), len(lay['keys'])))

        page.state_until(lambda s: any(s[1:1025]), 30, 'the screen to be drawn')
        print('ok: the screen is drawn')

        page.send('b btn-func 1')
        page.state_until(lambda s: key_held(s, FUNC), 3, 'FUNC to show held')
        print('ok: FUNC held from the page')
        page.close()                       # no release sent: the server must

        page = Page(port)
        page.state_until(lambda s: not key_held(s, FUNC), 3,
                         'FUNC to be let go after the page left')
        print('ok: a page that leaves lets go of its keys')

        page.send('x 40')
        page.state_until(lambda s: s[1033] == 40, 3, 'the crossfader to move')
        print('ok: crossfader')
        page.send('b trig-1 1')
        page.state_until(lambda s: key_held(s, 0), 3, 'TRIG1 held')
        page.send('r')
        page.state_until(lambda s: not key_held(s, 0), 3, "'r' to let go")
        print("ok: 'r' lets go")

        # An iPad put to sleep: the socket stays open and nothing answers.
        sleeper = Page(port)
        sleeper.send('b btn-func 1')
        page.state_until(lambda s: key_held(s, FUNC), 3, 'the sleeper to hold FUNC')
        page.state_until(lambda s: not key_held(s, FUNC), 20,
                         'the silent page to be dropped and FUNC let go')
        print('ok: a page that stops answering is dropped and lets go')
        sleeper.close()
        page.close()
    finally:
        emu.terminate()
        try:
            emu.wait(10)
        except subprocess.TimeoutExpired:
            emu.kill()
    print('PASS')


if __name__ == '__main__':
    main()
