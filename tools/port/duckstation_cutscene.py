#!/usr/bin/env python3
"""Plays one in-game cutscene on the unmodified PS1 game in DuckStation, as the port's Demo menu does,
and logs its timing (tools/port/gdb/cutscene_ps1.py): cutscene steps, voiced text pages, XA starts and
stops, by frame. The PS1 reference for the port's cutscene timing.

    python3 tools/port/duckstation_cutscene.py "map3_s00 func_800D0CF8" [--seconds 90] [--log out.log]

The name is a Demo menu entry (tools/port/cutscene_list.py). DuckStation runs at unlimited speed;
gdb halts emulation while it logs, so emulated timing is unaffected.

Upscaled reference pictures (the port's 2x renderer): --scale 2 --shots 3 runs DuckStation's hardware
renderer (Vulkan) at 2x internal resolution, at normal speed, and takes a screenshot at internal
resolution every 3 s of the cutscene (the F10 hotkey, pressed through XTEST: DuckStation runs under
XWayland for this, and its window must have the focus). Screenshots go to
build/port/ds_shots/ (moved from DuckStation's screenshot folder).
"""
import argparse
import time
import os
import shutil
import signal
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(HERE, "gdb"))
sys.path.insert(0, HERE)
import cutscene_list  # noqa: E402
import warp_sweep  # noqa: E402 (settings file, overrides, wait_port)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name", help='Demo menu name, e.g. "map3_s00 func_800D0CF8" or MapEvent_CafeCutscene')
    ap.add_argument("--seconds", type=float, default=120, help="emulated seconds after the cutscene starts")
    ap.add_argument("--timeout", type=float, default=600, help="wall-clock seconds")
    ap.add_argument("--cue", default=os.path.join(REPO, "build", "SLUS_007.07.cue"))
    ap.add_argument("--duckstation", default=os.path.expanduser("~/Downloads/DuckStation-x64.AppImage"))
    ap.add_argument("--steps", type=int, default=0, help="also log the cutscene's state steps up to this one "
                    "(gdb stops every frame until then)")
    ap.add_argument("--fps60", action="store_true", help="enable DuckStation's \"60 FPS\" patch for the game "
                    "(its patch database; frame interval 2 -> 1 vertical blank)")
    ap.add_argument("--overclock", type=int, default=0, help="CPU overclock in percent (e.g. 300)")
    ap.add_argument("--scale", type=int, default=0, help="hardware renderer at this internal resolution scale")
    ap.add_argument("--dithering", help="DuckStation's DitheringMode for the run (e.g. Unscaled, TrueColor)")
    ap.add_argument("--shots", type=float, default=0, help="screenshot every N wall-clock seconds of the cutscene")
    ap.add_argument("--log", default=os.path.join(REPO, "build", "port", "ps1_cutscene.log"))
    args = ap.parse_args()
    matches = [c for c in cutscene_list.cutscenes() if cutscene_list.label(c) == args.name or c["func"] == args.name]
    if not matches:
        sys.exit("no cutscene %r (see tools/port/cutscene_list.py --report)" % args.name)
    c = matches[0]

    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    if args.overclock:
        warp_sweep.OVERRIDES.update({"OverclockEnable": "true", "OverclockNumerator": str(args.overclock),
                                     "OverclockDenominator": "100"})
    game_ini = os.path.join(os.path.dirname(warp_sweep.SETTINGS), "gamesettings", "SLUS-00707.ini")
    game_ini_backup = game_ini + ".cutscene.bak"
    if os.path.exists(game_ini_backup):
        shutil.copy2(game_ini_backup, game_ini)
        os.remove(game_ini_backup)
    if args.fps60:
        if os.path.exists(game_ini):
            shutil.copy2(game_ini, game_ini_backup)
        os.makedirs(os.path.dirname(game_ini), exist_ok=True)
        with open(game_ini, "w") as f:
            f.write("[Patches]\nEnable = 60 FPS\n")
    env = None
    if args.scale:
        warp_sweep.OVERRIDES.update({"Renderer": "Vulkan", "ResolutionScale": str(args.scale),
                                     "ScreenshotMode": "InternalResolution", "EmulationSpeed": "1"})
        env = dict(os.environ, QT_QPA_PLATFORM="xcb")
    if args.dithering:
        warp_sweep.OVERRIDES["DitheringMode"] = args.dithering
    shots_dir = os.path.join(os.path.dirname(warp_sweep.SETTINGS), "screenshots")
    before = set(os.listdir(shots_dir)) if os.path.isdir(shots_dir) else set()
    backup = warp_sweep.patch_settings()
    ds = None
    shooter = None
    try:
        ds = subprocess.Popen([args.duckstation, "-batch", "-fastboot", "-nofullscreen", "--", args.cue],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL,
                              start_new_session=True, env=env)
        if not warp_sweep.wait_port(2345, 30):
            sys.exit("DuckStation's GDB server didn't start")
        import threading
        started = threading.Event()
        if args.shots:
            def shoot():
                # F10 (DuckStation's screenshot hotkey) sent to its windows (XSendEvent), which also get the
                # input focus, then pressed through XTEST: what reached it in a test on GNOME/XWayland.
                from Xlib import X, XK, display, protocol
                from Xlib.ext import xtest
                d = display.Display()
                root = d.screen().root
                kc = d.keysym_to_keycode(XK.string_to_keysym("F10"))

                def windows(w, out):
                    try:
                        name = str(w.get_wm_name() or "")
                        if "Silent Hill" in name or "SILENT HILL" in name.upper():
                            out.append(w)
                        for c in w.query_tree().children:
                            windows(c, out)
                    except Exception:
                        pass
                    return out
                started.wait()
                while ds.poll() is None:
                    for w in windows(root, []):
                        for typ in (protocol.event.KeyPress, protocol.event.KeyRelease):
                            w.send_event(typ(time=X.CurrentTime, root=root, window=w, same_screen=1, child=X.NONE,
                                             root_x=0, root_y=0, event_x=0, event_y=0, state=0, detail=kc),
                                         propagate=True)
                        try:
                            w.set_input_focus(X.RevertToParent, X.CurrentTime)
                        except Exception:
                            pass
                    d.sync()
                    xtest.fake_input(d, X.KeyPress, kc)
                    d.sync()
                    xtest.fake_input(d, X.KeyRelease, kc)
                    d.sync()
                    time.sleep(args.shots)
            shooter = threading.Thread(target=shoot, daemon=True)
            shooter.start()
        cmd = ["gdb-multiarch", "-q", "-batch", "-x", os.path.join(HERE, "gdb", "sh1.py"),
               "-x", os.path.join(HERE, "gdb", "cutscene_ps1.py"), "-ex", "target remote localhost:2345",
               "-ex", "sh-skip-intro on", "-ex", "sh-cutscene %s %d %g%s" % (c["map"], c["event"], args.seconds,
                                                                    " %s %d" % (c["func"][5:], args.steps) if args.steps and c["func"].startswith("func_") else ""),
               "-ex", "continue", "-ex", "kill"]
        with open(args.log, "w") as log:
            p = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, text=True)
            # The output loop blocks until gdb exits, so the wall-clock limit is a timer (a cutscene that
            # never gives control back kept gdb, and DuckStation, running).
            import threading
            limit = threading.Timer(args.timeout, p.kill)
            limit.start()
            try:
                for line in p.stdout:
                    log.write(line)
                    if line.startswith("[cut]") or line.startswith("[sh1]"):
                        print(line.rstrip(), flush=True)
                        if line.startswith("[cut]"):
                            started.set()
                p.wait(args.timeout)
            except subprocess.TimeoutExpired:
                p.kill()
            finally:
                limit.cancel()
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
        if args.shots and os.path.isdir(shots_dir):
            out = os.path.join(REPO, "build", "port", "ds_shots")
            os.makedirs(out, exist_ok=True)
            new = sorted(set(os.listdir(shots_dir)) - before)
            for f in new:
                shutil.move(os.path.join(shots_dir, f), os.path.join(out, f))
            print("%d screenshots in %s" % (len(new), out))
        if args.fps60:
            os.remove(game_ini)
            if os.path.exists(game_ini_backup):
                shutil.move(game_ini_backup, game_ini)


if __name__ == "__main__":
    main()
