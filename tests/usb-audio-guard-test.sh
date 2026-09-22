#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# usb-audio-guard-test.sh — the hook guard's positive control, with its own
# negative control.
#
# Every hook this feature installs is a jmp into payload memory. If that memory
# ever stops being our code — a project load wiping the heap, an allocation
# handed to a FLEX sample, any placement mistake — the machine executes
# whatever is there. On a unit that is an illegal instruction at IPL 5 with the
# PC somewhere in the sample heap, and it is unrecoverable without pulling the
# card. The guard checks the payload's 'OTPL' header before jumping, so a
# missing payload becomes silence plus a dismissible "USBAUD LOST" popup.
#
# ☠ The simulated loss must be RANDOM BYTES, not zeros: zeros decode as a valid
# ColdFire instruction (ori.b #0,d0) and an unguarded build survives them. And
# the failure signal must be k_exception_hook_sp, not process liveness: an
# exception leaves the emulator alive painting the EXCEPTION screen. This test
# passed for the wrong reason three times before both were fixed.
set -eu
OSIMG=${OSIMG:-$(ls -t out/OCTATRACK_OS*_usb-audio_*.os 2>/dev/null | head -1)}
[ -n "$OSIMG" ] || { echo "no usb-audio image — run: make fw-usb-audio" >&2; exit 1; }
PORT=${PORT:-3491}
BASE=${BASE:-0x40a955e0}
# ☠ Every build here uses the SHIPPING placement flags (the Makefile passes
# USBAUDIO_FLAGS; the default below matches it). Without --load-base the
# allocator places the payload at the top of the heap, the random bytes
# written at BASE hit nothing, and BOTH cases "survive" — the unguarded one
# fails this gate and the guarded one passes it for the wrong reason. And
# the restore at the end must produce the shipping payload, or every later
# gate runs a build with no heap reserve and no guard.
FLAGS=${USBAUDIO_FLAGS:---heap-reserve 9 --load-base 0x40a955e0 --dma-at 0x4ec94a00}

run_case() {          # run_case <name> <image> <payload> ; returns 1 on exception
    local name=$1 img=$2 pay=$3
    local d=out/usb-audio-guard/$name
    rm -rf "$d"; mkdir -p "$d"
    [ "$pay" -ef out/USBAUDIO.BIN ] || cp "$pay" out/USBAUDIO.BIN
    tests/usb-audio-card.sh out/usb-audio-card >/dev/null 2>&1
    cp out/usb-audio-card/card.img out/usb-audio-card/nvram.bin "$d/"
    ./octemu --headless --os "$img" --cf-card "$d/card.img" --nvram "$d/nvram.bin" \
        --hw-faithful --gdb "$PORT" --script tests/walks/usb-audio-reload.jsonl \
        --timeout 900 > "$d/walk.log" 2>&1 &
    local emu=$!
    local t=0
    while [ $t -lt 400 ]; do
        grep -q "\[mark\] .* before$" "$d/walk.log" 2>/dev/null && break
        kill -0 $emu 2>/dev/null || { echo "  $name: emulator died before install"; return 2; }
        sleep 5; t=$((t+5))
    done
    python3 - "$PORT" "$BASE" "$d" <<'PY'
import sys, time, importlib.util as iu
sp=iu.spec_from_file_location("c","tests/canary.py"); c=iu.module_from_spec(sp); sp.loader.exec_module(c)
r=c.Rsp(int(sys.argv[1])); r.send("?"); r.recv(10)
open(sys.argv[3]+"/exc_before","w").write(r.read(0x460ba974,4).hex())
base=int(sys.argv[2],16)
# ☠ Corrupt only while the CPU is OUTSIDE the payload. The '?' that halts the
# guest lands wherever it lands, and the frame ISR spends ~5% of its time in
# the producer: halted mid-payload, the CPU resumes into the random bytes and
# takes an exception in BOTH cases — a test artifact that failed the guarded
# case one run in twenty. A real wipe (memset from task context) clears the
# header first, so the guard sees it gone before any hook can enter.
for attempt in range(40):
    regs=r.cmd("g")                       # d0-d7, a0-a7, sr, pc: 8 hex each
    pc=int(regs[17*8:18*8],16)
    if not (base <= pc < base+0x10000):
        break
    r.send("c"); time.sleep(0.02); r.s.sendall(b"\x03"); r.recv(10)
else:
    sys.exit("could not halt the CPU outside the payload")
print(f"    corrupting with PC {pc:#010x}, outside the payload (after {attempt} re-halts)")
import random; random.seed(1234)
r.write(base, bytes(random.randrange(256) for _ in range(0x8000)))
r.cmd("c")
PY
    sleep 60
    local rc=0
    python3 - "$PORT" "$d" <<'PY' || rc=1
import sys, importlib.util as iu
sp=iu.spec_from_file_location("c","tests/canary.py"); c=iu.module_from_spec(sp); sp.loader.exec_module(c)
r=c.Rsp(int(sys.argv[1])); r.send("?"); r.recv(10)
now=r.read(0x460ba974,4).hex(); was=open(sys.argv[2]+"/exc_before").read()
r.cmd("c")
print(f"    k_exception_hook_sp {was} -> {now}")
sys.exit(1 if now != was else 0)
PY
    kill -TERM $emu 2>/dev/null || true; wait $emu 2>/dev/null || true
    return $rc
}

# ☠ Build the shipping payload first rather than trusting what a previous run
# left in out/. An aborted run leaves the NO-GUARD payload there, and the
# guarded case then fails for a reason unrelated to the guard.
MIDIIMG=${MIDIIMG:-$(ls -t out/OCTATRACK_OS*_usb-midi_*.os 2>/dev/null | head -1)}
python3 custom/usb-audio.py --in "$MIDIIMG" --out "$OSIMG" $FLAGS >/dev/null 2>&1

echo "USB-audio hook-guard control:"
if run_case guarded $OSIMG out/USBAUDIO.BIN; then
    echo "  guarded: ok — payload destroyed, machine still running"
else
    echo "  guarded: FAIL — the guard did not protect the hooks"; exit 1
fi

python3 custom/usb-audio.py --no-guard $FLAGS --in "$MIDIIMG" \
    --out out/usb-audio-noguard.os \
    --payload out/usb-audio-noguard.payload >/dev/null 2>&1
PORT=$((PORT+2))
if run_case unguarded out/usb-audio-noguard.os out/usb-audio-noguard.payload; then
    echo "  unguarded: FAIL — survived without the guard, so this gate proves nothing"
    python3 custom/usb-audio.py --in "$MIDIIMG" --out "$OSIMG" $FLAGS >/dev/null 2>&1
    exit 1
else
    echo "  unguarded: ok — without the guard the machine takes an exception"
fi
python3 custom/usb-audio.py --in "$MIDIIMG" --out "$OSIMG" $FLAGS >/dev/null 2>&1   # restore the shipping payload
echo "GUARD OK (verified against its own negative control)"
