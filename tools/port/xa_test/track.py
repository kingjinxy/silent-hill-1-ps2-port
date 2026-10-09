#!/usr/bin/env python3
"""Tracks a reference voice line through a recording in 20 ms pieces: for each piece, the offset
(ms, relative to the first strong match) and correlation of its best match nearby. Steady offsets
with high correlation mean clean playback; jumps show skipped or repeated audio.

    python3 tools/port/xa_test/track.py REF_48K.wav RECORDING.wav [FROM_S TO_S]
"""
import sys
import wave

import numpy as np


def load(p):
    w = wave.open(p)
    return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float).reshape(-1, w.getnchannels())[:, 0]


ref, rec = load(sys.argv[1]), load(sys.argv[2])
a0, a1 = (float(sys.argv[3]), float(sys.argv[4])) if len(sys.argv) > 4 else (1.5, 3.5)
# Anchor: the loudest 100 ms of the reference range, found anywhere in the recording.
seg = [int(a0 * 48000) + k * 4800 for k in range(int((a1 - a0) * 10))]
anchor = max(seg, key=lambda a: np.abs(ref[a:a + 4800]).sum())
p = ref[anchor:anchor + 4800]
n = 1 << int(np.ceil(np.log2(len(rec) + len(p))))
c = np.fft.irfft(np.fft.rfft(rec, n) * np.conj(np.fft.rfft(p, n)), n)[:len(rec)]
base = int(np.argmax(c)) - anchor
print("anchor at ref %.2f s -> recording %.3f s" % (anchor / 48000, (anchor + base) / 48000))
out = []
for a in range(int(a0 * 48000), int(a1 * 48000), 960):
    q = ref[a:a + 960]
    if np.abs(q).max() < 300:
        out.append("     .     ")
        continue
    lo = a + base - 2400
    reg = rec[lo:lo + 960 + 4800]
    cc = np.correlate(reg, q, "valid")
    j = int(np.argmax(cc))
    norm = cc[j] / np.sqrt((q ** 2).sum() * (reg[j:j + 960] ** 2).sum() + 1)
    out.append("%+5.1f/%.2f" % ((j - 2400) / 48, norm))
for i in range(0, len(out), 7):
    print("ref %.2f s: " % (a0 + i * 0.02) + "  ".join(out[i:i + 7]))
