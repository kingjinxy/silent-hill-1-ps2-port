#!/usr/bin/env python3
"""Starts the PS1 game in DuckStation (normal speed, for playing) with its GDB server, and logs the
world streaming state at every map load (tools/port/gdb/world_watch.py): PS1 RAM in build/port/watch/
and tools/port/world_diag.py's diagnosis in build/port/watch/world_watch.log. For comparing a map
load that hangs in the port with the same load on the PS1. Close DuckStation to stop.

    python3 tools/port/duckstation_world_watch.py
"""
import os
import shutil
import signal
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CUE = os.path.join(REPO, "rom", "image", "SLUS-00707.cue")
DUCKSTATION = os.path.expanduser("~/Downloads/DuckStation-x64.AppImage")
OUT = os.path.join(REPO, "build", "port", "watch")
sys.path.insert(0, os.path.join(HERE, "gdb"))
import warp_sweep  # noqa: E402 (settings file, wait_port)

# Only what the watch needs; speed, renderer etc. stay as the user set them.
OVERRIDES = {"EnableGDBServer": "true", "ConfirmPowerOff": "false", "SaveStateOnExit": "false",
             "PauseOnFocusLoss": "false"}


def main():
    os.makedirs(OUT, exist_ok=True)
    backup = warp_sweep.SETTINGS + ".world_watch.bak"
    if os.path.exists(backup):
        shutil.copy2(backup, warp_sweep.SETTINGS)
    shutil.copy2(warp_sweep.SETTINGS, backup)
    lines = []
    for line in open(warp_sweep.SETTINGS):
        key = line.split("=", 1)[0].strip()
        lines.append("%s = %s\n" % (key, OVERRIDES[key]) if key in OVERRIDES else line)
    open(warp_sweep.SETTINGS, "w").writelines(lines)
    ds = None
    try:
        ds = subprocess.Popen([DUCKSTATION, "-nofullscreen", "--", CUE], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, start_new_session=True)
        if not warp_sweep.wait_port(2345, 30):
            sys.exit("DuckStation's GDB server didn't start")
        env = dict(os.environ, WATCH_OUT=OUT)
        with open(os.path.join(OUT, "world_watch.log"), "w") as log:
            subprocess.run(["gdb-multiarch", "-q", "-batch", "-ex", "set architecture mips:3000",
                            "-ex", "target remote localhost:2345", "-x", os.path.join(HERE, "gdb", "world_watch.py")],
                           env=env, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    finally:
        if ds:
            for sig in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(ds.pid, sig)
                    ds.wait(5)
                    break
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    pass
        shutil.copy2(backup, warp_sweep.SETTINGS)
        os.remove(backup)


if __name__ == "__main__":
    main()
