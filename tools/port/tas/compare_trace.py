#!/usr/bin/env python3
"""Compares a port replay log's `rec step` lines with the steps trace_to_dem.py made from the reference trace.

    python3 tools/port/tas/compare_trace.py LOG [lines to print after the first difference]
"""
import lzma
import re
import struct
import sys
sys.path.insert(0, "tools/port/tas")
from trace_to_dem import START_FRAME, loop_rows


def main():
    steps = loop_rows()
    extra = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    bad = 0
    n = 0
    for l in open(sys.argv[1], errors="replace"):
        m = re.search(r"rec step (\d+): sys (-?\d+)/(-?\d+), Harry (-?\d+) (-?\d+) (-?\d+) hp (-?\d+), delta (-?\d+), seed (\w+)", l)
        if not m:
            continue
        n += 1
        s = int(m[1])
        if s >= len(steps):
            continue
        r = steps[s]
        pos = struct.unpack("<iii", bytes.fromhex(r[10]))
        ok = pos == (int(m[4]), int(m[5]), int(m[6]))
        if not ok:
            if bad < extra:
                print("step %d frame %s: port %s %s %s sys %s, trace %s sys %s map %s" % (s, r[0], m[4], m[5], m[6], m[2], pos, r[3], r[7]))
            bad += 1
    print("%d step lines, %d differ" % (n, bad))


if __name__ == "__main__":
    main()
