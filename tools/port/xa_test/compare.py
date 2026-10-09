#!/usr/bin/env python3
"""Compares the reference 48 kHz WAV with sh1spu's raw output (xa_host): first difference, how
many samples differ, largest difference; writes the host output as a WAV next to it."""
import struct
import sys
import wave

ref = wave.open(sys.argv[1])
r = struct.unpack("<%dh" % (ref.getnframes() * 2), ref.readframes(ref.getnframes()))
h = open(sys.argv[2], "rb").read()
h = struct.unpack("<%dh" % (len(h) // 2), h)
w = wave.open(sys.argv[2][:-4] + ".wav", "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000); w.writeframes(struct.pack("<%dh" % len(h), *h)); w.close()
n = min(len(r), len(h))
diffs = [i for i in range(n) if r[i] != h[i]]
print("reference %d frames, sh1spu %d frames; %d of %d samples differ" % (len(r) // 2, len(h) // 2, len(diffs), n))
if diffs:
    i = diffs[0]
    print("first difference at frame %d (%s): reference %d, sh1spu %d; largest %d" % (
        i // 2, "LR"[i % 2], r[i], h[i], max(abs(r[k] - h[k]) for k in diffs)))
