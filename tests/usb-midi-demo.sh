#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# usb-midi-demo.sh — the USB-MIDI end-to-end demo, both directions, on the
# patched image ($IMG) against the emulator's packet bench.
#
#   OT -> host (TX): PLAY with the sync-send gates on emits 0xFA + a 0xF8
#     clock stream, which arrive as USB-MIDI event packets on EP2 IN.
#   host -> OT (RX): a USB-MIDI note-on on channel 1 note 36 (AUDIO NOTE IN
#     STANDARD, track 1) fires track 1's SINE440 and it comes out of MAIN.
#
# Exits 0 only if BOTH directions are observed. Ships as the instrument the
# plan asks for, not a one-off probe.
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=${IMG:-$(ls -t out/OCTATRACK_OS*_usb-midi_*.os 2>/dev/null | head -1)}
[ -n "$IMG" ] || { echo "no usb-midi image — run: make fw-usb-midi" >&2; exit 1; }

# ---------- OT -> host (TX): clock/transport out EP2 IN ----------
TXSOCK=$(mktemp -u /tmp/octa-usbtx.XXXXXX)
TXLOG=$(mktemp /tmp/octa-usbtx-log.XXXXXX)
GDB=3417
./octemu --headless --os "$IMG" --cf-card out/fx2/card.img \
    --nvram out/fx2/nvram.bin --read-only --usb-host "$TXSOCK" --gdb $GDB \
    --script tests/walks/usb-midi-tx.jsonl --timeout 200 > "$TXLOG" 2>&1 &
EMU=$!
cleanup_tx() { kill $EMU 2>/dev/null || true; wait $EMU 2>/dev/null || true; }
trap cleanup_tx EXIT

for i in $(seq 1 60); do
    grep -q '\[mark\].*ready' "$TXLOG" && break
    kill -0 $EMU 2>/dev/null || { echo "TX emu died"; tail -5 "$TXLOG"; exit 1; }
    sleep 2
done
python3 tests/usb-host.py "$TXSOCK" enum >/dev/null
tests/rsp-io.py $GDB w:80000028=03 w:8000002a=01 >/dev/null
TXOUT=$(timeout 25 python3 tests/usb-host.py "$TXSOCK" midi-recv 18 2>&1 || true)
cleanup_tx; trap - EXIT

echo "$TXOUT" | grep -q '0ffa0000' || { echo "FAIL: no USB-MIDI Start (0xFA) on EP2 IN"; echo "$TXOUT" | sort | uniq -c; exit 1; }
echo "$TXOUT" | grep -q '0ff80000' || { echo "FAIL: no USB-MIDI clock (0xF8) on EP2 IN"; exit 1; }
CLK=$(echo "$TXOUT" | grep -c '0ff80000')
echo "TX ok: USB-MIDI Start + $CLK clock packets on EP2 IN"

# ---------- host -> OT (RX): USB note fires track 1's sample ----------
RXSOCK=$(mktemp -u /tmp/octa-usbrx.XXXXXX)
RXLOG=$(mktemp /tmp/octa-usbrx-log.XXXXXX)
RXWAV=$(mktemp /tmp/octa-usbrx.XXXXXX).wav
GDB=3418
rm -rf out/usbmidi/fx2demo && mkdir -p out/usbmidi/fx2demo
cp out/fx2/card.img out/fx2/nvram.bin out/usbmidi/fx2demo/
./octemu --headless --os "$IMG" \
    --cf-card out/usbmidi/fx2demo/card.img --nvram out/usbmidi/fx2demo/nvram.bin \
    --usb-host "$RXSOCK" --gdb $GDB --recording "$RXWAV" \
    --script tests/walks/midi-rx-note.jsonl --timeout 200 > "$RXLOG" 2>&1 &
EMU=$!
cleanup_rx() { kill $EMU 2>/dev/null || true; wait $EMU 2>/dev/null || true; }
trap cleanup_rx EXIT

for i in $(seq 1 60); do
    grep -q '\[mark\].*ready' "$RXLOG" && break
    kill -0 $EMU 2>/dev/null || { echo "RX emu died"; tail -5 "$RXLOG"; exit 1; }
    sleep 2
done
python3 tests/usb-host.py "$RXSOCK" enum >/dev/null
tests/rsp-io.py $GDB w:8000004b=01 w:80000012=01 >/dev/null
python3 tests/usb-host.py "$RXSOCK" midi-send 902464 >/dev/null
sleep 1
python3 tests/usb-host.py "$RXSOCK" midi-send 902400 >/dev/null
for i in $(seq 1 30); do
    grep -q '\[mark\].*done' "$RXLOG" && break
    kill -0 $EMU 2>/dev/null || break
    sleep 3
done
cleanup_rx; trap - EXIT

python3 - "$RXWAV" <<'EOF'
import wave, array, sys
w = wave.open(sys.argv[1]); a = array.array('h', w.readframes(w.getnframes()))
sr = w.getframerate() * w.getnchannels()
peak = max((max(abs(x) for x in a[s:s+sr]) for s in range(0, len(a), sr)),
           default=0)
print(f"RX peak {peak}")
sys.exit(0 if peak > 1000 else 1)
EOF
echo "RX ok: USB note-on fired track 1's sample out of MAIN"
echo "USB-MIDI demo PASSED both directions"
