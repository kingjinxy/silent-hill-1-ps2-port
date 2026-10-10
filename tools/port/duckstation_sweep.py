#!/usr/bin/env python3
"""Plays every Demo-menu cutscene on the unmodified PS1 game in DuckStation (one run of
tools/port/duckstation_cutscene.py each), and tabulates how each ended and its frame rate: frames the
game drew per second of emulated time, from the event's start until the player has control again. The
PS1 side of tools/port/demo_sweep.py (PCSX2).

    python3 tools/port/duckstation_sweep.py [--first N] [--only 3,7] [--timeout 300] [--out build/port/ps1_sweep.txt]
"""
import argparse
import os
import re
import signal  # noqa: F401
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import cutscene_list  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--only", help="comma-separated list numbers")
    ap.add_argument("--timeout", type=float, default=300, help="wall-clock seconds per cutscene")
    ap.add_argument("--out", default=os.path.join(REPO, "build", "port", "ps1_sweep.txt"))
    args = ap.parse_args()
    names = [cutscene_list.label(c) for c in cutscene_list.cutscenes()]
    order = [int(x) for x in args.only.split(",")] if args.only else range(args.first, len(names))
    out = open(args.out, "a")
    for i in order:
        log = os.path.join(REPO, "build", "port", "ps1_sweep_%02d.log" % i)
        # duckstation_cutscene.py stops DuckStation itself at --timeout; the outer limit is a backstop
        # that takes its whole process group (DuckStation included) with it.
        p = subprocess.Popen([sys.executable, os.path.join(HERE, "duckstation_cutscene.py"), names[i],
                              "--timeout", str(args.timeout), "--log", log],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)
        try:
            text = p.communicate(timeout=args.timeout + 120)[0]
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, 9)
            text = p.communicate()[0]
        m = re.search(r"fps: (\d+) frames in (\d+) vertical blanks = ([\d.]+) fps", text)
        presses = len(re.findall(r"Cross pressed", text))
        if m:
            result = "done  %6s fps  %5d frames  %6.1f s  %d presses" % (m.group(3), int(m.group(1)), int(m.group(2)) / 59.94, presses)
        elif "started" in text:
            result = "no return to gameplay within %g s (%d presses)" % (args.timeout, presses)
        else:
            result = "didn't start"
        line = "#%d %-40s %s" % (i, names[i], result)
        print(line, flush=True)
        out.write(line + "\n")
        out.flush()


if __name__ == "__main__":
    main()
