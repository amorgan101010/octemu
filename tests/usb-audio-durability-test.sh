#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Does the USB-audio stream SURVIVE A PROJECT RELOAD?
#
# This is the gate a heap-allocated payload could never pass: flex_heap_init
# memsets the whole sample heap and re-seeds the free-page stack on every
# project load, erasing the payload and leaving its hooks pointing into freed
# memory. The --heap-reserve build takes the lowest pages out of BOTH the
# memset and the free list at runtime, and loads the payload into them.
#
# ☠ The assertion is AUDIBLE, not structural: the iso stream must carry audio
# AFTER the reload, not merely exist. A payload whose bytes survive but whose
# frame_isr hook no longer runs would deliver perfect silence forever.
set -eu
OSIMG=${OSIMG:-$(ls -t out/OCTATRACK_OS*_usb-audio_*.os 2>/dev/null | head -1)}
[ -n "$OSIMG" ] || { echo "no usb-audio image — run: make fw-usb-audio" >&2; exit 1; }
PAY=${1:-out/USBAUDIO.BIN}
IMG=${2:-$OSIMG}
D=out/usb-audio-durability; rm -rf "$D"; mkdir -p "$D"
# the shipping payload IS the default output, so the copy can be a no-op
[ "$PAY" -ef out/USBAUDIO.BIN ] || cp "$PAY" out/USBAUDIO.BIN
tests/usb-audio-card.sh out/usb-audio-card >/dev/null 2>&1
cp out/usb-audio-card/card.img out/usb-audio-card/nvram.bin "$D/"
SOCK=$(mktemp -u /tmp/octa-dur.XXXXXX)
GDB=${GDB:-3481}
./octemu --headless --os "$IMG" --cf-card "$D/card.img" \
    --nvram "$D/nvram.bin" --usb-host "$SOCK" --hw-faithful --gdb "$GDB" \
    --script tests/walks/usb-audio-reload.jsonl --timeout 900 > "$D/walk.log" 2>&1 &
EMU=$!
trap 'kill -TERM $EMU 2>/dev/null || true' EXIT INT TERM
wait_mark() {
    local m=$1 t=0
    while [ $t -lt 500 ]; do
        grep -q "\[mark\] .* $m\$" "$D/walk.log" 2>/dev/null && return 0
        kill -0 $EMU 2>/dev/null || { echo "emulator died before $m"; return 1; }
        sleep 5; t=$((t+5))
    done
    echo "deadline waiting for $m"; return 1
}

# ☠ Two SHORT captures aligned to the marks, not one long one: the walk fires
# TRIG9 before and after the reload, and a single window spanning both cannot
# say which side the audio was on.
wait_mark before
echo "capturing BEFORE the reload"
timeout 60 python3 tests/usb-host.py "$SOCK" audio-stream "$D/before.pcm" 9000 \
    > "$D/before.log" 2>&1 || true

wait_mark after
echo "capturing AFTER the reload"
timeout 60 python3 tests/usb-host.py "$SOCK" audio-stream "$D/after.pcm" 9000 \
    > "$D/after.log" 2>&1 || true

wait_mark survived || true

# ☠ Structural checks the audio assertion alone cannot make. On hardware the
# payload was overwritten by FLEX sample data loaded from the BOTTOM of the
# free stack -- pages the constants patch only excluded from FUTURE re-seeds.
# Audio happened to keep working in the emulator because its fixture never
# allocated those pages, so the audible gate passed while a unit crashed with
# VEC:04 at IPL 5. These two reads catch it directly.
python3 - "$GDB" <<'PYCHK'
import sys, importlib.util as iu
sp=iu.spec_from_file_location("c","tests/canary.py"); c=iu.module_from_spec(sp); sp.loader.exec_module(c)
r=c.Rsp(int(sys.argv[1])); r.send("?"); r.recv(10)
base=int.from_bytes(r.read(0x8000691c,4),"big")
magic=r.read(0x40a955e0,4)
print(f"  flex_heap_free_base = {base} (must be >= the reserved page count)")
print(f"  payload header at 0x40a955e0 = {magic!r} (must still be b'OTPL')")
bad = (base == 0) or (magic != b"OTPL")
r.cmd("c")
sys.exit(1 if bad else 0)
PYCHK
CHK=$?
[ $CHK -eq 0 ] && echo "  structural checks OK" || echo "  ☠ STRUCTURAL CHECK FAILED: the payload's pages were handed out"
kill -TERM $EMU 2>/dev/null || true; wait $EMU 2>/dev/null || true
trap - EXIT INT TERM
[ "${CHK:-0}" -eq 0 ] || exit 1
grep -q "\[mark\] .* survived\$" "$D/walk.log" && echo "walk completed through the reload" \
    || echo "☠ walk did NOT reach 'survived'"
python3 - "$D/before.pcm" "$D/after.pcm" <<'PY'
import sys, struct, os
def loud(p):
    if not os.path.exists(p): return None, 0
    b=open(p,"rb").read()
    s=struct.unpack("<%dh"%(len(b)//2), b); L=s[0::2]
    return len(L), sum(1 for v in L if abs(v)>64)
nb, lb = loud(sys.argv[1])
na, la = loud(sys.argv[2])
print(f"  before reload: {nb} frames, {lb} loud")
print(f"  after  reload: {na} frames, {la} loud")
if not nb:  print("  FAIL: no stream before the reload"); sys.exit(1)
if not lb:  print("  FAIL: no audio before the reload (fixture problem)"); sys.exit(1)
if not na:  print("  FAIL: stream STOPPED across the project reload"); sys.exit(1)
if not la:  print("  FAIL: stream alive but SILENT after the reload (hooks dead)"); sys.exit(1)
print("  DURABILITY OK: audio present before AND after the project reload")
PY
