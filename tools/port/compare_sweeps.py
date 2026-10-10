#!/usr/bin/env python3
"""Compares the Demo cutscene sweeps: the port in PCSX2 (or on the PS2: any log with the port's output,
e.g. build/port/demo_sweep.log or build/port/ps2_log.out) against the PS1 game in DuckStation
(build/port/ps1_sweep.txt, tools/port/duckstation_sweep.py).

Per cutscene: how it ended on each, and frames drawn per second from the event's start until the
player has control again (the port's "demo: <name> fps: ..." line, src/port/demo_menu.c; the PS1's
"fps" line, tools/port/gdb/cutscene_ps1.py).

    python3 tools/port/compare_sweeps.py [--ps2 build/port/demo_sweep.log] [--ps1 build/port/ps1_sweep.txt]
"""
import argparse
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


def ps2_results(path):
    res = {}
    cur, beats, status = None, [], None
    for line in open(path, errors="replace"):
        m = re.search(r"demo: (.+?) started", line)
        if m:
            cur, beats, status = m.group(1), [], "started"
            continue
        if cur is None:
            continue
        m = re.search(r"demo: (.+?) fps: \d+ frames in \d+ vertical blanks = ([\d.]+) fps", line)
        if m and m.group(1) == cur:
            beats = [float(m.group(2))]
        m = re.search(r"demo: (.+?) (done|timeout)", line)
        if m and m.group(1) == cur:
            fps = beats[0] if len(beats) == 1 and isinstance(beats[0], float) else None
            res[cur] = (m.group(2), fps)
            cur = None
        if "CRASH:" in line and cur:
            res[cur] = ("CRASH", None)
            cur = None
    return res


def ps1_results(path):
    res = {}
    for line in open(path):
        m = re.match(r"#\d+ (.+?)\s{2,}(done|no return|didn't)(?:\s+([\d.]+) fps)?", line)
        if m:
            res[m.group(1).strip()] = (m.group(2), float(m.group(3)) if m.group(3) else None)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ps2", default=os.path.join(REPO, "build", "port", "demo_sweep.log"))
    ap.add_argument("--ps1", default=os.path.join(REPO, "build", "port", "ps1_sweep.txt"))
    args = ap.parse_args()
    p2, p1 = ps2_results(args.ps2), ps1_results(args.ps1)
    names = list(dict.fromkeys(list(p1) + list(p2)))
    ratios = []
    print("%-40s %-12s %8s   %-12s %8s   %s" % ("cutscene", "PS1", "fps", "port", "fps", "port/PS1"))
    for n in names:
        s1, f1 = p1.get(n, ("-", None))
        s2, f2 = p2.get(n, ("-", None))
        r = f2 / f1 if f1 and f2 else None
        if r:
            ratios.append(r)
        print("%-40s %-12s %8s   %-12s %8s   %s" % (n, s1, "%.1f" % f1 if f1 else "", s2, "%.1f" % f2 if f2 else "",
                                                   "%.2fx" % r if r else ""))
    if ratios:
        ratios.sort()
        print("\n%d cutscenes measured on both: port/PS1 frame rate median %.2fx, range %.2fx-%.2fx" % (
            len(ratios), ratios[len(ratios) // 2], ratios[0], ratios[-1]))


if __name__ == "__main__":
    main()
