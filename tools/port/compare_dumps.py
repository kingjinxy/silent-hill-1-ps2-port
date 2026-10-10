#!/usr/bin/env python3
"""Compares two directories of frame dumps (tools/port/bench_frames.sh) pixel by pixel."""
import glob
import os
import sys


def ppm(p):
    d = open(p, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3][:w * h * 3]


a, b = sys.argv[1], sys.argv[2]
fa = sorted(glob.glob(os.path.join(a, "*.ppm")))
fb = sorted(glob.glob(os.path.join(b, "*.ppm")))
bad = 0
for x, y in zip(fa, fb):
    wa, ha, da = ppm(x)
    wb, hb, db = ppm(y)
    diff = sum(1 for i in range(0, min(len(da), len(db)), 3) if da[i:i + 3] != db[i:i + 3])
    print("%s: %s" % (os.path.basename(x), "identical" if diff == 0 and (wa, ha) == (wb, hb) else "%d pixels differ" % diff))
    bad += diff != 0
print("%d of %d dumps differ%s" % (bad, min(len(fa), len(fb)), "" if len(fa) == len(fb) else " (counts %d vs %d)" % (len(fa), len(fb))))
sys.exit(1 if bad or len(fa) != len(fb) else 0)
