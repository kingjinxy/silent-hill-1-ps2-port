#!/usr/bin/env python3
"""Boot a PS2 ELF or disc image in PCSX2, print what the program writes to the console, then close it.

    python3 tools/port/pcsx2_run.py <file.elf|file.iso> [--seconds 15] [--until "text"] [--all]

ps2sdk's printf reaches PCSX2's IOP console (via the IOP's tty), so the EE and IOP console logs are
switched on in PCSX2.ini for the run and the original file restored afterwards (also on the next
run if one was killed). Output is the PCSX2 log after the ELF starts, minus emulator noise.
"""
import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

INI = os.path.expanduser("~/.config/PCSX2/inis/PCSX2.ini")
BACKUP = INI + ".pcsx2_run.bak"
# OutputMuted: PCSX2's "Mute" (audio is still emulated, just not played). HostFs: lets the program
# write to host: (= the ELF's folder, e.g. frame dumps from display_ps2.c).
# For comparisons with DuckStation: native resolution (upscale 1), "Bilinear (Sharp)" output
# (linear_present_mode 2), texture filtering as the program sets it on the GS (filter 2 = PS2).
OVERRIDES = {"EnableEEConsole": "true", "EnableIOPConsole": "true", "EnableFileLogging": "true", "OutputMuted": "true",
             "HostFs": "true", "upscale_multiplier": "1", "linear_present_mode": "2", "filter": "2",
             "Renderer": "13"}  # 13: PCSX2's software GS renderer (test runs use it)
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
    ap.add_argument("--all", action="store_true", help="print the whole log, not just from the ELF start")
    ap.add_argument("--slowboot", action="store_true", help="boot discs through the BIOS (OSDSYS) instead of fast boot")
    ap.add_argument("--cdvd-verbose", action="store_true", help="log every disc read (CdvdVerboseReads)")
    ap.add_argument("--bios", help="BIOS file name in PCSX2's bios folder to use for this run")
    ap.add_argument("--stall", type=float, default=15,
                    help="stop when no new frame has been shown for this many emulated seconds (crash), 0 = off")
    ap.add_argument("--gameplay", type=float, metavar="SECONDS",
                    help="stop SECONDS emulated seconds (heartbeats) of unbroken gameplay "
                         "(the port's \"port: gameplay\" line, without a later \"port: gameplay ended\"); the run then counts as passed")
    ap.add_argument("--progress", metavar="TEXT",
                    help="count the run as stuck when TEXT hasn't appeared for --progress-seconds emulated "
                         "seconds (heartbeats), e.g. a game sitting on the title screen still draws frames")
    ap.add_argument("--progress-seconds", type=float, default=60)
    ap.add_argument("--heartbeat", action="store_true", help="keep the port's heartbeat lines in the output")
    ap.add_argument("--realtime", action="store_true", help="run at normal speed (default: unlimited)")
    args = ap.parse_args()
    elf = os.path.abspath(args.elf)
    log = tempfile.NamedTemporaryFile(suffix=".log", delete=False).name

    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    if args.cdvd_verbose:
        OVERRIDES["CdvdVerboseReads"] = "true"
    if args.bios:
        OVERRIDES["BIOS"] = args.bios
    patch_ini()
    proc = None
    stalled = False
    try:
        boot = ["--", elf] if elf.lower().endswith(".iso") else ["-elf", elf, "--", elf]
        speed = [] if args.realtime else ["-unlimited"]
        proc = subprocess.Popen([args.pcsx2, "-batch", "-slowboot" if args.slowboot else "-fastboot", "-nofullscreen",
                                 *speed, "-logfile", log, *boot],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL,
                                start_new_session=True)
        end = time.time() + args.seconds
        # Crash detection, from the port's heartbeat (one line per 60 vertical blanks, i.e. per emulated
        # second, with the presented frame count): the game is considered crashed when it presents no
        # new frame for --stall emulated seconds, or when no heartbeat comes for --stall real seconds
        # (the EE is stuck, or emulation runs below full speed).
        beats, frame, frame_beat, last_beat = 0, None, 0, time.time()
        gameplay_done = False
        while time.time() < end and proc.poll() is None:
            text = open(log, errors="replace").read() if os.path.exists(log) else ""
            if args.until and args.until in text:
                break
            hb = re.findall(r"heartbeat: vblank \d+ frame (\d+)", text)
            if args.gameplay is not None and "port: gameplay\n" in text:
                after = text[text.rindex("port: gameplay\n"):]
                if "port: gameplay ended" not in after and after.count("heartbeat: vblank") >= args.gameplay:
                    gameplay_done = True
                    break
            if len(hb) != beats:
                beats, last_beat = len(hb), time.time()
                if hb[-1] != frame:
                    frame, frame_beat = hb[-1], beats
            if args.progress:
                at = text.rfind(args.progress)
                since = text[at:].count("heartbeat: vblank") if at >= 0 else len(hb)
                if since >= args.progress_seconds:
                    stalled = "no \"%s\" for %g emulated seconds" % (args.progress, args.progress_seconds)
                    break
            if args.stall and beats - frame_beat >= args.stall:
                stalled = "no new frame for %d emulated seconds (frame %s)" % (beats - frame_beat, frame)
                break
            if args.stall and time.time() - last_beat > args.stall:
                stalled = "no heartbeat for %g s (EE stuck, or %s)" % (
                    args.stall, "game not started" if not beats else "last frame %s" % frame)
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
    started = 0 if args.all else next((i for i, l in enumerate(text) if "Initializing Elf" in l), 0)
    out = [l for l in text[started:] if not any(n in l for n in NOISE) and (args.heartbeat or "heartbeat:" not in l)]
    print("\n".join(out))
    if stalled:
        print("pcsx2_run: %s, assuming the game crashed" % stalled)
    if args.gameplay is not None:
        print("pcsx2_run: %s" % ("%g s of gameplay without a crash" % args.gameplay if gameplay_done
                                 else "gameplay not reached / not held for %g s" % args.gameplay))
        return 0 if gameplay_done else 1
    return 0 if not args.until or any(args.until in l for l in out) else 1


if __name__ == "__main__":
    sys.exit(main())
