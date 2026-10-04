#!/usr/bin/env python3
"""Boot a PS2 ELF in PCSX2, print what the program writes to the console, then close PCSX2.

    python3 tools/port/pcsx2_run.py <file.elf> [--seconds 15] [--until "text"]

ps2sdk's printf reaches PCSX2's IOP console (via the IOP's tty), so the EE and IOP console logs are
switched on in PCSX2.ini for the run and the original file restored afterwards (also on the next
run if one was killed). Output is the PCSX2 log after the ELF starts, minus emulator noise.
"""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

INI = os.path.expanduser("~/.config/PCSX2/inis/PCSX2.ini")
BACKUP = INI + ".pcsx2_run.bak"
OVERRIDES = {"EnableEEConsole": "true", "EnableIOPConsole": "true", "EnableFileLogging": "true"}
NOISE = ("GL_EXTENSIONS", "UpdateVSyncRate", "Frame rate:", "Set GS CRTC", "Vulkan", "OpenGL")


def find_pcsx2():
    for d in ("~/Downloads", "~/Applications", "~"):
        d = os.path.expanduser(d)
        for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            if f.lower().startswith("pcsx2") and f.endswith(".AppImage"):
                return os.path.join(d, f)
    return shutil.which("pcsx2-qt") or shutil.which("pcsx2")


def patch_ini():
    if os.path.exists(BACKUP):
        shutil.copy2(BACKUP, INI)
    shutil.copy2(INI, BACKUP)
    lines = []
    for line in open(INI):
        key = line.split("=", 1)[0].strip()
        lines.append("%s = %s\n" % (key, OVERRIDES[key]) if key in OVERRIDES else line)
    open(INI, "w").writelines(lines)


def restore_ini():
    shutil.copy2(BACKUP, INI)
    os.remove(BACKUP)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--seconds", type=float, default=15)
    ap.add_argument("--until", help="stop as soon as this text appears in the output")
    ap.add_argument("--pcsx2", default=find_pcsx2())
    args = ap.parse_args()
    elf = os.path.abspath(args.elf)
    log = tempfile.NamedTemporaryFile(suffix=".log", delete=False).name

    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    patch_ini()
    proc = None
    try:
        proc = subprocess.Popen([args.pcsx2, "-batch", "-fastboot", "-nofullscreen", "-logfile", log, "-elf", elf, "--", elf],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL,
                                start_new_session=True)
        end = time.time() + args.seconds
        while time.time() < end and proc.poll() is None:
            if args.until and os.path.exists(log) and args.until in open(log, errors="replace").read():
                break
            time.sleep(0.5)
    finally:
        if proc:
            for sig in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(proc.pid, sig)
                    proc.wait(5)
                    break
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    pass
        restore_ini()

    text = open(log, errors="replace").read().splitlines() if os.path.exists(log) else []
    os.remove(log)
    started = next((i for i, l in enumerate(text) if "Initializing Elf" in l), 0)
    out = [l for l in text[started:] if not any(n in l for n in NOISE)]
    print("\n".join(out))
    return 0 if not args.until or any(args.until in l for l in out) else 1


if __name__ == "__main__":
    sys.exit(main())
