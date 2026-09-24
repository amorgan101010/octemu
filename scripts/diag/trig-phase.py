"""phase.py WAV START_BLOCK: per-beat onset frame and its phase within the
16-frame block, plus whether the beat's sustained body dropped."""
import sys, wave
import numpy as np

w = wave.open(sys.argv[1]); sr = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1, 2).astype(float).mean(axis=1)
s = int(sys.argv[2]) * 16
thr = np.abs(x[s:]).max() * 0.05
first = s + int(np.argmax(np.abs(x[s:]) > thr))
q = sr // 2
bodies, onsets = [], []
i = 0
while first + i * q + int(0.2 * sr) < len(x):
    a = first + i * q - 400                      # search window around the beat
    seg = np.abs(x[a:a + 1200])
    on = a + int(np.argmax(seg > thr))          # first frame over threshold
    onsets.append(on)
    b = x[on + int(0.075 * sr):on + int(0.175 * sr)]
    bodies.append(np.sqrt((b ** 2).mean()))
    i += 1
med = np.median(bodies)
for k, (on, bd) in enumerate(zip(onsets, bodies)):
    tag = "DROP" if bd < 0.5 * med else ""
    if tag or k < 16:
        print(f"beat {k + 1:3d} onset frame {on:9d}  block {on // 16:7d}  phase {on % 16:2d}  body {bd:6.0f} {tag}")
drops = [k for k, b in enumerate(bodies) if b < 0.5 * med]
print("phases of drops:", sorted(set(onsets[k] % 16 for k in drops)))
print("phase histogram of all beats:", np.bincount([o % 16 for o in onsets], minlength=16).tolist())
