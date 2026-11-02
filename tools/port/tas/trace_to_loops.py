#!/usr/bin/env python3
"""Finds the game loops in the per-frame reference trace: during play a loop's end shows as a change of the
in-game timer, and g_VBlanks says how many video frames the next loop takes, which carries the walk over
frames where the timer is stopped. Prints how well the two agree, by game state.

    python3 tools/port/tas/trace_to_loops.py [tas/reference/trace.log.xz]
"""
import lzma
import sys
from collections import Counter


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "tas/reference/trace.log.xz"
    rows = [l.split() for l in lzma.open(src, "rt") if l[0].isdigit()]
    igt_changes = {i for i in range(1, len(rows)) if rows[i][8] != rows[i - 1][8] and rows[i][2] == "11"}
    walk, i = [], 0
    while i < len(rows):
        walk.append(i)
        i += max(int(rows[i][4]), 1)
    loops = set(walk)
    hit = len(igt_changes & loops)
    near = sum(1 for c in igt_changes if {c - 1, c, c + 1} & loops)
    print("loops walked %d, timer changes %d, on a walked loop %d, within one frame %d"
          % (len(walk), len(igt_changes), hit, near))
    print("loops by game state", Counter(rows[i][2] for i in walk).most_common(6))


if __name__ == "__main__":
    main()
