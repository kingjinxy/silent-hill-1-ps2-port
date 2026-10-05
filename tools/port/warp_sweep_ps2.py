#!/usr/bin/env python3
"""Warps the attract demo into each map on the PS2 build in PCSX2 and checks the game survives
(the PS2 counterpart of tools/port/gdb/warp_sweep.py).

For each map: writes build/port/warp.txt (read by src/port/ps2/warp_ps2.c), boots the disc image,
and waits for the map load after the warped one (the warped demo ran and the game got back to the
title). pcsx2_run.py's stall detection reports crashes. With --realtime, also reports the frame rate
while in the warped map (build with SH1_FPS=60 SH1_BENCH=1 to measure 60 fps headroom).

    python3 tools/port/warp_sweep_ps2.py [--maps map1_s02,5,...] [--realtime] [--timeout 240]
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
ISO = os.path.join(REPO, "build", "port", "sh1_ps2.iso")
WARP = os.path.join(REPO, "build", "port", "warp.txt")
MAPS = [
    "map0_s00", "map0_s01", "map0_s02",
    "map1_s00", "map1_s01", "map1_s02", "map1_s03", "map1_s04", "map1_s05", "map1_s06",
    "map2_s00", "map2_s01", "map2_s02", "map2_s03", "map2_s04",
    "map3_s00", "map3_s01", "map3_s02", "map3_s03", "map3_s04", "map3_s05", "map3_s06",
    "map4_s00", "map4_s01", "map4_s02", "map4_s03", "map4_s04", "map4_s05", "map4_s06",
    "map5_s00", "map5_s01", "map5_s02", "map5_s03",
    "map6_s00", "map6_s01", "map6_s02", "map6_s03", "map6_s04", "map6_s05",
    "map7_s00", "map7_s01", "map7_s02", "map7_s03",
]
DONE = "(next map load after the warp)"


def frame_rates(lines):
    """Frames per second for each 60-frame display line while in the warped map, in gameplay (the
    game's 320-wide 224-line mode), skipping the first one (still loading)."""
    rates, prev, inside = [], None, False
    for line in lines:
        if "(warp from" in line:
            inside = True
        elif DONE in line:
            break
        m = re.search(r"\[\s*([\d.]+)\] display: frame \d+, (\d+)x(\d+)", line)
        if m and inside:
            t = float(m.group(1))
            if prev is not None and m.group(3) == "224":
                rates.append(60 / (t - prev))
            prev = t
    return rates[1:]


def run_map(name, args, log):
    open(WARP, "w").write(name + "\n")
    cmd = [sys.executable, os.path.join(HERE, "pcsx2_run.py"), "--seconds", str(args.timeout), "--until", DONE, ISO]
    if args.realtime:
        cmd.insert(2, "--realtime")
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    log.write("==== %s\n%s\n" % (name, out))
    log.flush()
    lines = out.splitlines()
    if not any("(warp from" in l for l in lines):
        return "ERROR", "warp never triggered", []
    rates = frame_rates(lines)
    if any(DONE in l for l in lines):
        return "PASS", "", rates
    stall = [l for l in lines if l.startswith("pcsx2_run:")]
    faults = [l.split("] ", 1)[-1] for l in lines if "TLB Miss, pc=" in l or "Exception" in l]
    return "FAIL", (stall[0][11:] if stall else "timed out") + ("; " + faults[0] if faults else ""), rates


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--maps", help="comma-separated names or numbers (default: all)")
    ap.add_argument("--realtime", action="store_true", help="run at normal speed and report frame rates")
    ap.add_argument("--timeout", type=int, default=240, help="seconds per map")
    args = ap.parse_args()
    maps = MAPS
    if args.maps:
        maps = [MAPS[int(m)] if m.isdigit() else m for m in args.maps.split(",")]
    log = open(os.path.join(REPO, "build", "port", "warp_sweep_ps2.log"), "w")
    results = []
    try:
        for name in maps:
            status, why, rates = run_map(name, args, log)
            fps = " fps min %.0f avg %.1f" % (min(rates), sum(rates) / len(rates)) if rates and args.realtime else ""
            print("%-9s %-5s%s %s" % (name, status, fps, why), flush=True)
            results.append(status)
    finally:
        if os.path.exists(WARP):
            os.remove(WARP)
    print("%d/%d passed (log: build/port/warp_sweep_ps2.log)" % (results.count("PASS"), len(results)))
    return 0 if results.count("PASS") == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
