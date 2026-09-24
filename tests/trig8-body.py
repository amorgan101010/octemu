"""body.py WAV START_BLOCK: per-beat body energy (75-175 ms after each beat); flags drops."""
import sys, wave
import numpy as np

w = wave.open(sys.argv[1]); sr = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1, 2).astype(float).mean(axis=1)
s = int(sys.argv[2]) * 16
seg = x[s:]
thr = np.abs(seg).max() * 0.05
first = int(np.argmax(np.abs(seg) > thr))
q = int(sr * 0.5)
bodies = []
i = 0
while first + i * q + int(0.175 * sr) <= len(seg):
    a = first + i * q
    b = seg[a + int(0.075 * sr):a + int(0.175 * sr)]
    bodies.append(np.sqrt((b ** 2).mean()))
    i += 1
bodies = np.array(bodies)
med = np.median(bodies)
drops = [k + 1 for k, v in enumerate(bodies) if v < 0.5 * med]
print(f"beats {len(bodies)}  median body {med:.0f}  drops at beats {drops}")
if len(drops) > 1:
    print(f"  spacing {np.diff(drops).tolist()}")
