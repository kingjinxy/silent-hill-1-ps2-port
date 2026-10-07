#!/usr/bin/env python3
"""Warps the attract demo into each map on the PS2 build in PCSX2 and checks the game survives
(the PS2 counterpart of tools/port/gdb/warp_sweep.py).

Boots once for the whole list: build/port/warp.txt (read by src/port/ps2/warp_ps2.c) lists the maps;
each run of the first demo (the only one played, right after boot with no logos or title wait) warps to the next one, runs --seconds-per-map emulated seconds, then the port presses a
button to end it, and the next demo takes the next map. A map passes when the game gets back to a
map load after it. pcsx2_run.py's stall detection reports crashes; the sweep then boots again from
the following map. Also reports the game's frame rate while in
the warped map, in emulated time (frames per 60 vertical blanks, from the port's heartbeat), so it
doesn't depend on the emulator's speed: build with SH1_FPS=60 SH1_BENCH=1 to measure 60 fps headroom.

    python3 tools/port/warp_sweep_ps2.py [--maps map1_s02,5,...] [--seconds-per-map 40] [--realtime]
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
    """(frame rate, idle %) for each emulated second (heartbeat: one line per 60 vertical blanks) in
    a map's segment of the log, skipping the first five (loading and the fade-in, which waits an
    extra vertical blank per frame by design)."""
    rates, prev = [], None
    for line in lines:
        m = re.search(r"heartbeat: vblank (\d+) frame (\d+) idle (\d+)%", line)
        if m:
            vb, fr, idle = int(m.group(1)), int(m.group(2)), int(m.group(3))
            if prev is not None and vb > prev[0]:
                rates.append(((fr - prev[1]) * 60.0 / (vb - prev[0]), idle))
            prev = (vb, fr)
    return rates[5:]


def run_list(names, args, log):
    """One boot through `names`; returns [(name, status, why, rates)] for the maps it got to."""
    open(WARP, "w").write("seconds=%d\n%s\n" % (args.seconds_per_map, " ".join(names)))
    timeout = len(names) * (args.seconds_per_map + 60) + 60
    cmd = [sys.executable, os.path.join(HERE, "pcsx2_run.py"), "--heartbeat", "--seconds", str(timeout),
           "--until", "port: warp list done", "--progress", "port: map load",
           "--progress-seconds", str(args.seconds_per_map + 30), ISO]
    if args.realtime:
        cmd.insert(2, "--realtime")
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    log.write("==== %s\n%s\n" % (" ".join(names), out))
    log.flush()
    results, current, seg, measured = [], None, [], None
    for line in out.splitlines():
        m = re.search(r"port: map load (\S+) \(warp from", line)
        if m:
            current, seg, measured = m.group(1), [], None
            continue
        if current and DONE in line:
            results.append((current, "PASS", "", frame_rates(measured if measured is not None else seg)))
            current, measured = None, None
            continue
        if current and "port: ending demo" in line:
            measured = list(seg) # frame rates: the time in the map, not the way back to the title
            continue
        if current:
            seg.append(line)
    if current:
        stall = [l for l in out.splitlines() if l.startswith("pcsx2_run:")]
        faults = [l.split("] ", 1)[-1] for l in seg if "TLB Miss, pc=" in l or "Exception" in l]
        why = (stall[0][11:] if stall else "timed out") + ("; " + faults[0] if faults else "")
        results.append((current, "FAIL", why, frame_rates(seg)))
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--maps", help="comma-separated names or numbers (default: all)")
    ap.add_argument("--seconds-per-map", type=int, default=40, help="emulated seconds in each map")
    ap.add_argument("--realtime", action="store_true", help="run at normal speed (frame rates don't need it)")
    args = ap.parse_args()
    maps = MAPS
    if args.maps:
        maps = [MAPS[int(m)] if m.isdigit() else m for m in args.maps.split(",")]
    log = open(os.path.join(REPO, "build", "port", "warp_sweep_ps2.log"), "w")
    results, todo = [], list(maps)
    try:
        while todo:
            got = run_list(todo, args, log)
            if not got:
                for name in todo:
                    print("%-9s ERROR warp never triggered" % name, flush=True)
                    results.append("ERROR")
                break
            for name, status, why, rates in got:
                fps = ""
                if rates:
                    srt = sorted(r for r, _ in rates)
                    idle = sorted(i for _, i in rates)
                    fps = " fps avg %.1f, 10%% low %.0f (%d s); idle avg %d%%, min %d%%" % (
                        sum(srt) / len(srt), srt[len(srt) // 10], len(srt), sum(idle) // len(idle), idle[0])
                print("%-9s %-5s%s %s" % (name, status, fps, why), flush=True)
                results.append(status)
            todo = todo[len(got):]
    finally:
        if os.path.exists(WARP):
            os.remove(WARP)
    print("%d/%d passed (log: build/port/warp_sweep_ps2.log)" % (results.count("PASS"), len(results)))
    return 0 if results.count("PASS") == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
