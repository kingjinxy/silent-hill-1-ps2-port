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
    rows = [l.split() for l in lzma.open("tas/reference/trace.log.xz", "rt") if l[0].isdigit()]
    rows = [r for r in rows if int(r[0]) >= START_FRAME and len(r) >= 12]
    steps, wait, prev = [], 0, None
    for r in rows:
        if r[2] not in STEP_STATES:
            prev, wait = r, 0
            continue
        new = prev is None or prev[2] != r[2] or r[8] != prev[8] or wait <= 1
        if new:
            steps.append(r)
            wait = max(int(r[4]), 1)
        else:
            wait -= 1
        prev = r
    return steps


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/port/rec/tas.dem"
    steps = loop_rows()
    rows = steps
    config, save = [bytes.fromhex(l) for l in open(os.path.expanduser("~/emu/sh-tas/start.hex")).read().split()]
    head = bytearray(STATE_SIZE)
    head[0:len(config)] = config
    head[0x38] = ord("T")  # Port_RecTas: a step on every in-game loop
    head[0x100:0x100 + len(save)] = save
    struct.pack_into("<IH", head, 0x7F8, len(steps), int(rows[0][6], 16) & 0xFFFF)
    with open(out, "wb") as f:
        f.write(head)
        for r in steps:
            pad = bytes.fromhex(r[9]) if r[9] != "-" else bytes.fromhex("0073ffff80808080")
            interval = min(max(int(r[4]), 1), 4)
            f.write(pad + struct.pack("<bBbBI", int(r[2]), interval, 0, int(r[12]) % 255 + 1, int(r[6], 16)))
    print(len(steps), "steps to", out, os.path.getsize(out), "bytes")


if __name__ == "__main__":
    main()
