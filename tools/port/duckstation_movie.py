#!/usr/bin/env python3
"""Compares the port's movie decoder (src/port/ps2/strdec.c) with the PS1's MDEC output as DuckStation
renders it, on the intro movie that plays after boot.

DuckStation side (tools/port/gdb/movie_ps1.py): chosen frames of the movie, read from VRAM (24-bit,
at the displayed area). Port side: the same frames decoded on the host by tools/port/str/strdump
(built here from src/port/ps2/strdec.c). Prints per-channel differences and writes a side-by-side PNG
with an amplified difference image.

    python3 tools/port/duckstation_movie.py [--frames 100,300,600,900] [--no-capture]
"""
import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import compare_frames  # noqa: E402  (DuckStation launch and settings)

CAP = os.path.join(REPO, "build", "port", "cap")
HILL = os.path.join(REPO, "rom", "USA", "HILL.")
STRDUMP = os.path.join(REPO, "build", "port", "strdump")


def capture(frames):
    backup = compare_frames.patch_duckstation()
    settings = compare_frames.warp_sweep.SETTINGS
    lines = open(settings).read().splitlines(True)
    if any(l.split("=", 1)[0].strip() == "OutputMuted" for l in lines):  # muted, as every test run
        lines = ["OutputMuted = true\n" if l.split("=", 1)[0].strip() == "OutputMuted" else l for l in lines]
    else:
        lines = [l + ("OutputMuted = true\n" if l.strip() == "[Audio]" else "") for l in lines]
    open(settings, "w").writelines(lines)
    ds = None
    try:
        ds = subprocess.Popen([compare_frames.DUCKSTATION, "-batch", "-fastboot", "-nofullscreen", "--", compare_frames.CUE],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL,
                              start_new_session=True)
        if not compare_frames.warp_sweep.wait_port(2345, 30):
            sys.exit("DuckStation's GDB server didn't start")
        env = dict(os.environ, CAP_FRAMES=",".join(map(str, frames)), CAP_OUT=CAP)
        out = subprocess.run(["gdb-multiarch", "-q", "-batch", "-ex", "set architecture mips:3000",
                              "-ex", "target remote localhost:2345", "-x", os.path.join(HERE, "gdb", "movie_ps1.py")],
                             env=env, capture_output=True, text=True, timeout=1800, stdin=subprocess.DEVNULL)
        print("\n".join(l for l in (out.stdout + out.stderr).splitlines() if "capture:" in l or "rror" in l))
    finally:
        if ds:
            import signal
            for sig in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(ds.pid, sig)
                    ds.wait(5)
                    break
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    pass
        import shutil
        shutil.copy2(backup, compare_frames.warp_sweep.SETTINGS)
        os.remove(backup)


def ps1_frame(n, w, h):
    """The frame's 24-bit pixels from the captured VRAM: bytes packed along each row from (disp x, disp y)."""
    vram = open(os.path.join(CAP, "ps1movie_%d_vram.bin" % n), "rb").read()
    disp = [int(v) for v in open(os.path.join(CAP, "ps1movie_%d_disp.txt" % n)).read().split()]
    x, y = disp[0], disp[1]
    rows = []
    for r in range(h):
        start = ((y + r) * 1024 + x) * 2
        rows.append(vram[start:start + w * 3])
    return b"".join(rows)


def port_frames(sector, frames):
    """The port decoder's frames (RGB) for the movie at PS1 sector `sector`."""
    silent = os.path.getsize(os.path.join(REPO, "rom", "USA", "SILENT.")) // 2048
    raw = sector - (64 + silent)
    subprocess.run(["gcc", "-O2", "-o", STRDUMP, os.path.join(HERE, "str", "strdump.c"),
                    os.path.join(REPO, "src", "port", "ps2", "strdec.c")], check=True)
    out = {}
    prefix = os.path.join(CAP, "portmovie_")
    subprocess.run([STRDUMP, HILL, str(raw), str(max(frames) + 1), "1", prefix], check=True, stdout=subprocess.DEVNULL)
    for n in frames:
        data = open("%s%04d.ppm" % (prefix, n), "rb").read()
        header = data.split(b"\n", 3)
        w, h = (int(v) for v in header[1].split())
        out[n] = (w, h, header[3])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", default="100,300,600,900")
    ap.add_argument("--no-capture", action="store_true", help="compare the last capture again")
    args = ap.parse_args()
    frames = [int(f) for f in args.frames.split(",")]
    os.makedirs(CAP, exist_ok=True)
    if not args.no_capture:
        capture(frames)
    sector = int(open(os.path.join(CAP, "ps1movie_sector.txt")).read())
    port = port_frames(sector, frames)
    from PIL import Image
    for n in frames:
        w, h, a = port[n]
        b = ps1_frame(n, w, h)
        diffs = [abs(x - y) for x, y in zip(a, b)]
        hist = [0] * 256
        for d in diffs:
            hist[d] += 1
        mean = sum(diffs) / len(diffs)
        exact = hist[0] * 100.0 / len(diffs)
        within1 = (hist[0] + hist[1]) * 100.0 / len(diffs)
        print("frame %4d: mean |diff| %.3f, max %d; channel values exact %.1f%%, within 1: %.1f%%" %
              (n, mean, max(diffs), exact, within1))
        sheet = Image.new("RGB", (w * 3, h))
        sheet.paste(Image.frombytes("RGB", (w, h), b), (0, 0))
        sheet.paste(Image.frombytes("RGB", (w, h), a), (w, 0))
        sheet.paste(Image.frombytes("RGB", (w, h), bytes(min(255, d * 32) for d in diffs)), (2 * w, 0))
        sheet.save(os.path.join(CAP, "movie_compare_%d.png" % n))
    print("PNGs (DuckStation | port | difference x32): %s/movie_compare_<frame>.png" % CAP)


if __name__ == "__main__":
    main()
