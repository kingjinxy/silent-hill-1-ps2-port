#!/usr/bin/env python3
"""Finds the reference voice line (xa_ref.py --out48) in a PCSX2 recording made with
SH1_XA_SOLO=1 (only the XA input audible), lines them up, and reports where they differ: the error
level per 5 ms window, the share of bad windows and the spacing between them.

    python3 tools/port/xa_test/compare_recording.py REF_48K.wav RECORDING.wav
"""
import sys
import wave

import numpy as np


def load(path):
    w = wave.open(path)
    a = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64).reshape(-1, w.getnchannels())
    return a[:, 0], w.getframerate()


ref, r1 = load(sys.argv[1])
rec, r2 = load(sys.argv[2])
assert r1 == r2 == 48000
probe = ref[:48000 * 2]
n = 1 << int(np.ceil(np.log2(len(rec) + len(probe))))
corr = np.fft.irfft(np.fft.rfft(rec, n) * np.conj(np.fft.rfft(probe, n)), n)[:len(rec)]
off = int(np.argmax(corr))
seg = rec[off:off + len(ref)]
ref = ref[:len(seg)]
gain = float(np.dot(seg, ref) / max(np.dot(ref, ref), 1))
err = seg - gain * ref
win = 240  # 5 ms
k = len(ref) // win
e = np.sqrt((err[:k * win].reshape(k, win) ** 2).mean(1))
s = np.sqrt(((gain * ref[:k * win]).reshape(k, win) ** 2).mean(1)) + 1
bad = np.where(e / s > 0.3)[0]
print("line found at %.3f s in the recording, gain %.3f" % (off / 48000, gain))
print("error/signal overall %.3f; %d of %d 5-ms windows bad (>0.3)" % (np.sqrt((err ** 2).mean()) / np.sqrt(((gain * ref) ** 2).mean()), len(bad), k))
if len(bad) > 1:
    gaps = np.diff(bad)
    print("first bad windows (ms):", (bad[:20] * 5).tolist())
    print("most common spacing between bad windows (ms):", (np.bincount(gaps[gaps > 1]).argmax() * 5) if (gaps > 1).any() else "-")
