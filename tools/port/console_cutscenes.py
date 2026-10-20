#!/usr/bin/env python3
"""Plays a list of Demo-menu cutscenes on the real PS2 in each of several rendering modes, over the
network, and tabulates speed and problems from the console log.

For each mode, the Debug Options rendering settings are set remotely (ps2_ctl.py options: bit 0
Resolution x2, bit 1 Color Depth 32-bit), then each cutscene is requested (ps2_ctl.py demo N) and
followed in build/port/ps2.log (tools/port/ps2_log.py must be running; build-and-deploy starts it)
until the game reports gameplay ("demo: NAME done"), a crash, a restart, a stall or a timeout.

Per cutscene: the game's fps line (frames drawn per second from the event's start to player
control, FMVs included at their 15 fps), the in-engine fps and slowest one-second heartbeat window and
mean idle with the seconds around FMVs left out, texture readbacks per frame (GS
stats), stuck GS transfers. Results go to stdout and build/port/console_cutscenes.md.

    python3 tools/port/console_cutscenes.py [--cutscenes 36,50,1,...] [--modes 2,1,3] [--timeout 420]
"""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
LOG = os.path.join(REPO, "build", "port", "ps2.log")
OUT = os.environ.get("CONSOLE_CUTSCENES_OUT", os.path.join(REPO, "build", "port", "console_cutscenes.md"))
CTL = os.path.join(HERE, "ps2_ctl.py")

# Heaviest cutscenes of the 2026-10-17 sweep (PS2 Optimizations.md), the benchmark, and ones with screen
# effects (motion blur, lens flare); all reached gameplay in the sweeps.
DEFAULT = "36,50,1,43,0,13,4,32,48,8"
MODE_NAMES = {0: "1x 16-bit", 1: "2x 16-bit", 2: "1x 32-bit", 3: "2x 32-bit"}
STALL = 60  # wall-clock seconds without a heartbeat line: the game is frozen


def names():
    out = []
    for line in open(os.path.join(REPO, "build", "port", "demo_cutscenes.c")):
        m = re.match(r'\s*\{ "(.*)", (\d+), (\d+) \},', line)
        if m:
            out.append(m.group(1))
    return out


def ctl(*args):
    r = subprocess.run([sys.executable, CTL, *map(str, args)], capture_output=True, text=True, timeout=30)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


def read_from(pos):
    with open(LOG, errors="replace") as f:
        f.seek(pos)
        return f.read()


def play(idx, name, timeout):
    """Requests cutscene `idx` and follows the log; returns (status, log text)."""
    start = os.path.getsize(LOG)
    ok, msg = ctl("demo", idx)
    if not ok:
        return "no answer (%s)" % msg, ""
    t0 = last_beat = time.time()
    beats = 0
    while True:
        time.sleep(1)
        text = read_from(start)
        if "demo: %s done" % name in text:
            return "ok", text
        if "SH1 port: starting" in text:
            return "restarted", text
        if "CRASH" in text or "exception" in text.lower():
            return "crash", text
        n = text.count("heartbeat:")
        if n != beats:
            beats, last_beat = n, time.time()
        if time.time() - last_beat > STALL:
            return "stall (no heartbeat for %d s)" % STALL, text
        if time.time() - t0 > timeout:
            return "timeout", text


def measure(text, name):
    """Numbers from one cutscene's log text."""
    r = {"fps": "", "game": "", "worst": "", "idle": "", "readbacks": "", "movie": "",
         "stuck": text.count("VRAM download stuck")}
    m = re.search(r"demo: %s fps: .*= ([\d.]+) fps" % re.escape(name), text)
    if m:
        r["fps"] = m.group(1)
    # Heartbeats between the request and gameplay: per-second frames and idle, leaving out the seconds
    # around an FMV (shown at its own 15 fps; "movie: started" .. "movie: stopped") and the first
    # second after one.
    beats, movie, skip = [], False, 0
    for line in text.splitlines():
        if "movie: started" in line:
            movie = True
        elif "movie: stopped" in line:
            movie, skip = False, 2
        m = re.search(r"heartbeat: vblank (\d+) frame (\d+) idle (\d+)%", line)
        if m:
            beats.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), movie or skip > 0))
            skip = max(0, skip - 1)
    rates, idles, frames, vbs = [], [], 0, 0
    for (v0, f0, _, m0), (v1, f1, i1, m1) in zip(beats, beats[1:]):
        if v1 > v0 and f1 >= f0 and not (m0 or m1):
            rates.append((f1 - f0) * 59.94 / (v1 - v0))
            idles.append(i1)
            frames, vbs = frames + f1 - f0, vbs + v1 - v0
    if rates:
        r["worst"] = "%.1f" % min(rates)
        r["game"] = "%.1f" % (frames * 59.94 / vbs)
        r["idle"] = "%d%%" % round(sum(idles) / len(idles))
    r["movie"] = "yes" if "movie: started" in text else ""
    rb = [int(x) for x in re.findall(r"gs stats per frame:.* (\d+) readbacks", text)]
    if rb:
        r["readbacks"] = "%.1f / %d" % (sum(rb) / len(rb), max(rb))
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cutscenes", default=DEFAULT, help="Demo list numbers")
    ap.add_argument("--modes", default="2,1,3", help="option bits: 1 2x, 2 32-bit (0 = 1x 16-bit)")
    ap.add_argument("--timeout", type=float, default=420, help="wall-clock seconds per cutscene")
    args = ap.parse_args()
    if not os.path.exists(LOG):
        sys.exit("no %s: start tools/port/ps2_log.py (build-and-deploy does)" % LOG)
    ok, msg = ctl("ping")
    if not ok:
        sys.exit("the PS2 doesn't answer: %s" % msg)
    cs = names()
    idxs = [int(x) for x in args.cutscenes.split(",")]
    modes = [int(x) for x in args.modes.split(",")]
    rows = []
    lines = ["# Console cutscene test (%s)" % time.strftime("%Y-%m-%d %H:%M"), "",
             "| mode | # | cutscene | result | event fps | in-engine fps | slowest second | idle | readbacks a frame (mean / max) | FMV | stuck |",
             "|---|---|---|---|---|---|---|---|---|---|---|"]

    def flush():
        with open(OUT, "w") as f:
            f.write("\n".join(lines + rows) + "\n")

    for mode in modes:
        ok, msg = ctl("options", mode)
        print("== %s (options %d): %s" % (MODE_NAMES[mode], mode, msg), flush=True)
        if not ok:
            sys.exit("options not set")
        for idx in idxs:
            name = cs[idx]
            status, text = play(idx, name, args.timeout)
            m = measure(text, name)
            row = "| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s | %d |" % (
                MODE_NAMES[mode], idx, name, status, m["fps"], m["game"], m["worst"], m["idle"], m["readbacks"], m["movie"],
                m["stuck"])
            rows.append(row)
            print(row, flush=True)
            flush()
            if status in ("restarted", "crash") or status.startswith("stall") or status.startswith("no answer"):
                print("stopping: the PS2 needs attention (%s)" % status, flush=True)
                return
            time.sleep(3)
    print("results: %s" % OUT)


if __name__ == "__main__":
    main()
