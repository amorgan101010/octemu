#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -uo pipefail
cd "$(dirname "$0")/.."
here=$PWD
RUNS=${1:-3}; SECS=${2:-20}; LABEL=${3:-$(date +%H%M)}
BLOCKS=$(python3 -c "print(int($SECS*44100/16))")
[ -f out/fx2/card.img ] || { echo "needs out/fx2 (make fixtures)" >&2; exit 1; }
[ -n "${QEMU:-}" ] && export OCTEMU_QEMU="$QEMU"
W=$(mktemp -d "$here/out/bench.XXXX"); trap 'rm -rf "$W"' EXIT
printf '%s\n' '{"wait_text":"PTCH","timeout_ms":180000}' '{"wait_guest_ms":4000}' \
  '{"tap":"PLAY"}' '{"wait_guest_ms":2000}' '{"mark":"a"}' \
  "{\"wait_blocks\":$BLOCKS}" '{"mark":"b"}' > "$W/walk.jsonl"
for r in $(seq 1 "$RUNS"); do
  cp out/fx2/card.img out/fx2/nvram.bin "$W/"
  ${TASKSET:-} ./octemu --headless --cf-card "$W/card.img" --nvram "$W/nvram.bin" \
      --script "$W/walk.jsonl" --timeout 600 > "$W/log" 2>&1 &
  emu=$!
  until grep -aq '\[mark\].* a$' "$W/log" 2>/dev/null || ! kill -0 $emu 2>/dev/null; do sleep 0.05; done
  pid=$(pgrep -f "[-]M octatrack,nvram=$W/nvram" | head -1)
  [ -n "$pid" ] || { echo "run $r: emulator died" >&2; cat "$W/log" | tail -5 >&2; continue; }
  perf stat -x, -e cycles:u,instructions:u -p "$pid" -o "$W/perf" &
  ps_=$!
  until grep -aq '\[mark\].* b$' "$W/log" 2>/dev/null || ! kill -0 $emu 2>/dev/null; do sleep 0.02; done
  kill -INT $ps_; wait $ps_ 2>/dev/null; wait $emu 2>/dev/null
  python3 - "$W/log" "$W/perf" "$SECS" "$LABEL" "$r" "${QEMU:-default}" "$here/out/bench-results.tsv" <<'PY'
import re,sys
log,perf,secs,label,run,qemu,out=sys.argv[1:]
m=dict((n,(int(b),int(t))) for b,t,n in re.findall(r'\[mark\] blk=(\d+) (\d+) (\w+)',open(log,errors='replace').read()))
v={}
for l in open(perf):
    p=l.strip().split(',')
    if len(p)>2 and p[0].replace('.','').isdigit(): v[p[2].split(':')[0]]=float(p[0])
(b1,t1),(b2,t2)=m['a'],m['b']; es=(b2-b1)*16/44100; wall=((t2-t1)%1000000)/1000  # mark time wraps at 1e6 ms
mc=v['cycles']/es/1e6; mi=v['instructions']/es/1e6
line=f"{label}\t{run}\t{mc:.0f}\t{mi:.0f}\t{es/wall:.3f}\t{qemu}"
print(f"run {run}: {mc:.0f} Mcycles/emu-s  {mi:.0f} Minstr/emu-s  {es/wall:.3f}x realtime")
open(out,'a').write(line+"\n")
PY
done
