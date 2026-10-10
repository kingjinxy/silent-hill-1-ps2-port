#!/usr/bin/env python3
"""Plays every Demo-menu cutscene in PCSX2, in list order, and reports crashes and hangs.

The game does the sweep itself (src/port/demo_menu.c, from host:demo.txt): it requests each cutscene,
waits for "demo: <name> done" (gameplay reached) or a timeout, and goes on. This script runs PCSX2
(tools/port/pcsx2_run.py) until "demo sweep: done", a crash (CRASH lines) or a stall (no new frame);
after a crash or stall it starts PCSX2 again from the next cutscene.

    python3 tools/port/demo_sweep.py [--first N] [--last N] [--timeout 300] [--realtime] [--iso build/port/sh1_ps2.iso]
"""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


def names():
    out = []
    for line in open(os.path.join(REPO, "build", "port", "demo_cutscenes.c")):
        m = re.match(r'\s*\{ "(.*)", (\d+), (\d+) \},', line)
        if m:
            out.append(m.group(1))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--last", type=int, default=9999)
    ap.add_argument("--realtime", action="store_true", help="PCSX2 at normal speed (default: unlimited)")
    ap.add_argument("--timeout", type=int, default=300, help="emulated seconds per cutscene to reach gameplay")
    ap.add_argument("--iso", default=os.path.join(REPO, "build", "port", "sh1_ps2.iso"))
    ap.add_argument("--log", default=os.path.join(REPO, "build", "port", "demo_sweep.log"))
    args = ap.parse_args()
    cs = names()
    control = os.path.join(os.path.dirname(os.path.abspath(args.iso)), "demo.txt")  # host:demo.txt
    results = {}
    first = args.first
    log = open(args.log, "w")
    try:
        while first < len(cs) and first <= args.last:
            open(control, "w").write("sweep %d %d %d\n" % (first, args.timeout, args.last))
            print("demo_sweep: PCSX2 from #%d" % first, flush=True)
            out = subprocess.run([sys.executable, os.path.join(HERE, "pcsx2_run.py"), args.iso, "--seconds", "14400",
                                  "--until", "demo sweep: done", "--stall", "30"] + (["--realtime"] if args.realtime else []),
                                 capture_output=True, text=True, errors="replace").stdout
            log.write(out)
            log.flush()
            current = None
            for line in out.splitlines():
                m = re.search(r"demo: (.+?) (requested|started|done|timeout)", line)
                if m and m.group(1) in cs:
                    i = cs.index(m.group(1))
                    current = i if m.group(2) in ("requested", "started") else current
                    if m.group(2) in ("done", "timeout"):
                        results[i] = m.group(2)
                        print("  #%d %-40s %s" % (i, cs[i], m.group(2)), flush=True)
                        current = None
            if "demo sweep: done" in out:
                break
            crash = [l for l in out.splitlines() if "CRASH:" in l][:2]
            why = " | ".join(l.strip()[:150] for l in crash) or "stalled (no new frame for 30 emulated seconds) or ended"
            if current is None:
                print("demo_sweep: PCSX2 stopped between cutscenes: %s" % why, flush=True)
                current = max(results) if results else first - 1
                if current + 1 <= first - 1:
                    break
            else:
                results[current] = "FAILED: " + why
                print("  #%d %-40s FAILED: %s" % (current, cs[current], why), flush=True)
            first = current + 1
    finally:
        if os.path.exists(control):
            os.remove(control)
    print("demo_sweep: summary (log: %s)" % args.log)
    for i in sorted(results):
        print("  #%d %-40s %s" % (i, cs[i], results[i]))
    bad = [i for i in results if results[i] != "done"]
    print("demo_sweep: %d of %d reached gameplay; %d didn't" % (len(results) - len(bad), len(results), len(bad)))


if __name__ == "__main__":
    main()
