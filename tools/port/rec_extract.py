#!/usr/bin/env python3
"""Rebuilds play recordings (src/port/demo_rec.c) from a console log (build/port/ps2.log, tools/port/
ps2_log.py) or a PCSX2 log: the `recdata NAME OFFSET HEX` lines, the last run of each name. Writes
build/port/rec/NAME.dem, with the step count set from the bytes received; a recording cut off by a
crash ends at its last complete step. Lost lines (the log drops output when its buffer is full) are
reported: the recording then ends before the first gap.

    python3 tools/port/rec_extract.py [log] [--name NAME]
"""
import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
HEADER, FRAME = 2048, 16


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log", nargs="?", default=os.path.join(REPO, "build", "port", "ps2.log"))
    ap.add_argument("--name")
    args = ap.parse_args()
    recs, ended = {}, {}
    for line in open(args.log, errors="replace"):
        m = re.search(r"recdata (\S+) (\d+) ([0-9A-F]+)$", line.rstrip())
        if m:
            name, off = m.group(1), int(m.group(2))
            if off == 0:
                recs[name], ended[name] = {}, None  # a new run of this name
            if name in recs:
                recs[name][off] = bytes.fromhex(m.group(3))
            continue
        m = re.search(r"recend (\S+) (\d+)", line)
        if m and m.group(1) in recs:
            ended[m.group(1)] = int(m.group(2))
    if not recs:
        sys.exit("rec_extract: no recordings in %s" % args.log)
    for name, chunks in recs.items():
        if args.name and name != args.name:
            continue
        data, gaps = bytearray(), 0
        for off in sorted(chunks):
            if off != len(data):
                gaps += 1
                break
            data += chunks[off]
        if len(data) < HEADER:
            print("rec_extract: %s: header incomplete, skipped" % name)
            continue
        steps = (len(data) - HEADER) // FRAME
        data = data[:HEADER + steps * FRAME]
        data[0x7F8:0x7FC] = struct.pack("<I", steps)
        out = os.path.join(REPO, "build", "port", "rec", name + ".dem")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        open(out, "wb").write(data)
        how = "complete" if ended[name] == HEADER + steps * FRAME and not gaps else (
            "lines lost: cut at the first gap" if gaps else "no end (crash or still recording)")
        print("rec_extract: %s: %d steps (%.1f s at 60 fps), %s -> %s" % (name, steps, steps / 60, how, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
