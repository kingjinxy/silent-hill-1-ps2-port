#!/usr/bin/env python3
"""Warp the title-screen attract demo into every map and check the game survives.

For each map: boot the disc in DuckStation at unlimited speed, attach gdb, redirect the first map
load to that map, and wait for the next map load (the following demo). Reaching it means the
warped map ran and the game got back to the title. On timeout, report where the CPU is.

    python3 tools/port/gdb/warp_sweep.py [--cue build/SLUS_007.07.cue] [--maps 0,5,map7_s03]

Temporarily edits DuckStation's settings.ini (unlimited speed, GDB server on, no confirm/resume
state on exit) and restores it afterwards.
"""
import argparse
import os
import shutil
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, HERE)
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
SETTINGS = os.path.expanduser("~/.local/share/duckstation/settings.ini")
OVERRIDES = {
    # Native resolution, nearest texture filtering (as the PS1), bilinear-sharp output scaling: the
    # same comparison settings as tools/port/pcsx2_run.py.
    "ResolutionScale": "1",
    "TextureFilter": "Nearest",
    "SpriteTextureFilter": "Nearest",
    "Scaling": "BilinearSharp",
    "EmulationSpeed": "0",
    "ConfirmPowerOff": "false",
    "SaveStateOnExit": "false",
    "PauseOnFocusLoss": "false",
    "EnableGDBServer": "true",
}


def patch_settings():
    backup = SETTINGS + ".warp_sweep.bak"
    if os.path.exists(backup):
        # A previous run was killed before it could restore the settings: the backup is the original.
        shutil.copy2(backup, SETTINGS)
    shutil.copy2(SETTINGS, backup)
    lines = []
    for line in open(SETTINGS):
        key = line.split("=", 1)[0].strip()
        if key in OVERRIDES:
            line = "%s = %s\n" % (key, OVERRIDES[key])
        lines.append(line)
    open(SETTINGS, "w").writelines(lines)
    return backup


def wait_port(port, timeout):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 0.5).close()
            return True
        except OSError:
            time.sleep(0.5)
    return False


def run_map(args, idx, log):
    ds = subprocess.Popen([args.duckstation, "-batch", "-fastboot", "-nofullscreen", "--", args.cue],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL,
                          start_new_session=True)  # Own process group: the AppImage launcher doesn't forward signals.
    try:
        if not wait_port(2345, 30):
            return "ERROR", "GDB server didn't start"
        time.sleep(1)
        env = dict(os.environ, SH1_SWEEP="1")
        gdb = subprocess.Popen(
            ["gdb-multiarch", "-q", "-batch", "-x", os.path.join(HERE, "sh1.py"),
             "-ex", "target remote localhost:2345", "-ex", "sh-skip-intro on", "-ex", "warp %d" % idx, "-ex", "continue",
             "-ex", "p/x $pc", "-ex", "info symbol $pc", "-ex", "bt 6", "-ex", "kill"],
            cwd=REPO, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            out, _ = gdb.communicate(timeout=args.timeout)
            timed_out = False
        except subprocess.TimeoutExpired:
            gdb.send_signal(signal.SIGINT)  # Halt the target; the remaining -ex commands then report where.
            try:
                out, _ = gdb.communicate(timeout=20)
            except subprocess.TimeoutExpired:
                gdb.kill()
                out, _ = gdb.communicate()
            timed_out = True
        log.write("==== %s\n%s\n" % (MAPS[idx], out))
        log.flush()
        if "warp:" not in out:
            return "ERROR", "warp never triggered" + (" (DuckStation closed)" if "Remote connection closed" in out else "")
        if not timed_out and "next map load reached" in out:
            return "PASS", ""
        if "Remote connection closed" in out:
            return "FAIL", "DuckStation closed before the next map load (closed by hand, or emulator exit)"
        where = [l for l in out.splitlines() if " in section " in l or l.startswith("$1")]
        return "FAIL", "timed out at " + ("; ".join(where) or "unknown PC")
    finally:
        # Close DuckStation whatever happened, including a crashed game or an emulator error dialog.
        for sig in (signal.SIGTERM, signal.SIGKILL):
            try:
                os.killpg(ds.pid, sig)
            except ProcessLookupError:
                break
            try:
                ds.wait(5)
                break
            except subprocess.TimeoutExpired:
                pass
        time.sleep(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cue", default=os.path.join(REPO, "build", "SLUS_007.07.cue"))
    ap.add_argument("--duckstation", default=os.path.expanduser("~/Downloads/DuckStation-x64.AppImage"))
    ap.add_argument("--maps", default="all")
    ap.add_argument("--timeout", type=float, default=120, help="seconds (wall clock) per map")
    ap.add_argument("--log", default="warp_sweep.log")
    args = ap.parse_args()
    maps = range(len(MAPS)) if args.maps == "all" else [
        int(m) if m.isdigit() else MAPS.index(m) for m in args.maps.split(",")]

    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))  # Run the finally block (restores settings).
    backup = patch_settings()
    results = []
    try:
        with open(args.log, "w") as log:
            for idx in maps:
                start = time.time()
                status, info = run_map(args, idx, log)
                results.append((MAPS[idx], status, info))
                print("%-9s %-5s %5.0fs %s" % (MAPS[idx], status, time.time() - start, info), flush=True)
    finally:
        shutil.copy2(backup, SETTINGS)
        os.remove(backup)
    bad = [r for r in results if r[1] != "PASS"]
    print("\n%d/%d passed" % (len(results) - len(bad), len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
