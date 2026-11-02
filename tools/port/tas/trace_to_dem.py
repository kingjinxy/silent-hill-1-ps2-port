#!/usr/bin/env python3
"""Turns the reference trace into a play recorder file: one step per game loop of game state 11 (in game),
from the first map load on, with the options and savegame from dump_start.lua.

    python3 tools/port/tas/trace_to_dem.py [out.dem]
"""
import lzma
import os
import struct
import sys

STATE_SIZE, FRAME_SIZE = 2048, 16
START_FRAME = 2109
STEP_STATES = {"11", "12", "13", "14", "15", "18"}  # Demo_StateGet's step states


def loop_rows():
    """One row per game loop: g_ActiveBufferIdx flips once a loop. PAD_OFFSET picks the row the step's pad,
    interval and seed come from, relative to the flip."""
    k = int(os.environ.get("PAD_OFFSET", "0"))
    rows = [l.split() for l in lzma.open("tas/reference/trace.log.xz", "rt") if l[0].isdigit()]
    rows = [r for r in rows if int(r[0]) >= START_FRAME and len(r) >= 14]
    steps = []
    for i in range(1, len(rows) - 1):
        if rows[i][13] != rows[i - 1][13] and rows[i][2] in STEP_STATES:
            steps.append(rows[max(i + k, 0)])
    return steps


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/port/rec/tas.dem"
    steps = loop_rows()
    config, save = [bytes.fromhex(l) for l in open(os.path.expanduser("~/emu/sh-tas/start.hex")).read().split()]
    head = bytearray(STATE_SIZE)
    head[0:len(config)] = config
    head[0x38] = ord("T")  # Port_RecTas: a step on every in-game loop
    head[0x100:0x100 + len(save)] = save
    struct.pack_into("<IH", head, 0x7F8, len(steps), int(steps[0][6], 16) & 0xFFFF)
    with open(out, "wb") as f:
        f.write(head)
        for r in steps:
            pad = bytes.fromhex(r[9]) if r[9] != "-" else bytes.fromhex("0073ffff80808080")
            interval = min(max(int(r[4]), 1), 4)
            f.write(pad + struct.pack("<bBbBI", int(r[2]), interval, 0, 0, int(r[6], 16)))
    print(len(steps), "steps to", out, os.path.getsize(out), "bytes")


if __name__ == "__main__":
    main()
