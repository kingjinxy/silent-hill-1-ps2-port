#!/usr/bin/env python3
"""Compares frames of the first attract demo between the PS1 game in DuckStation (software renderer,
the reference) and the port in PCSX2 (current build/port/sh1_ps2.iso; build it without SH1_BENCH,
which desyncs the demo).

Frames are demo frames (g_Demo_DemoStep), so both runs capture the same moment. For each frame,
build/port/cap/ gets:
  - cmp_<N>_display.png: displayed area, PS1 | PS2 | difference (white = differs)
  - cmp_<N>_vram.png:    the whole 1024x512 VRAM, PS1 above PS2, then the difference
and the report lists pixel differences and the first GP0 packets that differ (same packets but
different pixels: the renderer; different packets: game logic, GTE, ...).

    python3 tools/port/compare_frames.py [--frames 60,200,400] [--skip-ps1] [--skip-ps2]
"""
import argparse
import os
import shutil
import signal
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CAP = os.path.join(REPO, "build", "port", "cap")
ISO = os.path.join(REPO, "build", "port", "sh1_ps2.iso")
CUE = os.path.join(REPO, "rom", "image", "SLUS-00707.cue")
DUCKSTATION = os.path.expanduser("~/Downloads/DuckStation-x64.AppImage")
sys.path.insert(0, os.path.join(HERE, "gdb"))
import warp_sweep  # noqa: E402  (DuckStation settings patching)
sys.path.insert(0, HERE)
import struct_layout  # noqa: E402  (field names for the game state diff)


# --- Captures --------------------------------------------------------------------------------------

def capture_ps2(frames):
    open(os.path.join(REPO, "build", "port", "capture.txt"), "w").write(",".join(map(str, frames)) + "\n")
    try:
        out = subprocess.run([sys.executable, os.path.join(HERE, "pcsx2_run.py"), "--seconds", "300",
                              "--until", "capture: all frames done", ISO], capture_output=True, text=True).stdout
    finally:
        os.remove(os.path.join(REPO, "build", "port", "capture.txt"))
    print("\n".join(l.split("] ", 1)[-1] for l in out.splitlines() if "capture:" in l or l.startswith("pcsx2_run:")))


def patch_duckstation():
    """warp_sweep's settings plus the software renderer (added to [GPU] if missing)."""
    backup = warp_sweep.patch_settings()
    lines = open(warp_sweep.SETTINGS).read().splitlines(True)
    if any(l.split("=", 1)[0].strip() == "Renderer" for l in lines):
        lines = ["Renderer = Software\n" if l.split("=", 1)[0].strip() == "Renderer" else l for l in lines]
    else:
        lines = [l + ("Renderer = Software\n" if l.strip() == "[GPU]" else "") for l in lines]
    open(warp_sweep.SETTINGS, "w").writelines(lines)
    return backup


def capture_ps1(frames):
    backup = patch_duckstation()
    ds = None
    try:
        ds = subprocess.Popen([DUCKSTATION, "-batch", "-fastboot", "-nofullscreen", "--", CUE], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, start_new_session=True)
        if not warp_sweep.wait_port(2345, 30):
            sys.exit("DuckStation's GDB server didn't start")
        time.sleep(1)
        env = dict(os.environ, CAP_FRAMES=",".join(map(str, frames)), CAP_OUT=CAP)
        out = subprocess.run(["gdb-multiarch", "-q", "-batch", "-ex", "set architecture mips:3000",
                              "-ex", "target remote localhost:2345", "-x", os.path.join(HERE, "gdb", "capture_ps1.py")],
                             env=env, capture_output=True, text=True, timeout=1800, stdin=subprocess.DEVNULL)
        print("\n".join(l for l in (out.stdout + out.stderr).splitlines() if "capture:" in l or "Error" in l or "rror:" in l))
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


# --- Comparison ------------------------------------------------------------------------------------

def load(side, n):
    base = os.path.join(CAP, "%s_%d" % (side, n))
    vram = struct.unpack("<%dH" % (1024 * 512), open(base + "_vram.bin", "rb").read())
    disp = tuple(int(x) for x in open(base + "_disp.txt").read().split()[:4])
    packets, data, pos = [], open(base + "_gp0.bin", "rb").read(), 0
    while pos + 4 <= len(data):
        (cnt,) = struct.unpack_from("<I", data, pos)
        packets.append(struct.unpack_from("<%dI" % cnt, data, pos + 4))
        pos += 4 + 4 * cnt
    return vram, disp, packets


def rgb(c):
    return bytes(((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3))


def write_png(path, rows):
    w, h = len(rows[0]) // 3, len(rows)
    raw = b"".join(b"\0" + r for r in rows)

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))

    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def normalize(words):
    """A GP0 packet with the bits the GPU ignores cleared: for textured polygons, the upper half of
    the UV words after the second (the first holds the CLUT, the second the texture page)."""
    cmd = words[0] >> 24
    if 0x20 <= cmd < 0x40 and cmd & 0x04:
        gouraud, nv = cmd & 0x10, 4 if cmd & 0x08 else 3
        w = list(words)
        for i in range(2, nv):
            k = (2 + i * 3) if gouraud else (2 + i * 2)
            if k < len(w):
                w[k] &= 0xFFFF
        return tuple(w)
    return tuple(words)


def compare(n):
    a, da, pa = load("ps1", n)
    b, db, pb = load("ps2", n)
    x, y, w, h = da
    lines = ["frame %d: display PS1 %s, PS2 %s" % (n, da, db)]

    # Displayed area (the PS1's), and all of VRAM; bit 15 (mask) compared separately.
    def stats(x0, y0, ww, hh):
        diff = mask = 0
        for yy in range(y0, y0 + hh):
            for xx in range(x0, x0 + ww):
                i = (yy & 511) * 1024 + (xx & 1023)
                diff += (a[i] ^ b[i]) & 0x7FFF != 0
                mask += (a[i] ^ b[i]) & 0x8000 != 0
        return diff, mask
    dd, dm = stats(x, y, w, h)
    vd, vm = stats(0, 0, 1024, 512)
    lines.append("  display: %d of %d pixels differ (bit 15: %d); VRAM: %d of 524288 (bit 15: %d)" % (dd, w * h, dm, vd, vm))

    rows = []
    for yy in range(h):
        row = b""
        for src in (a, b):
            row += b"".join(rgb(src[((y + yy) & 511) * 1024 + ((x + xx) & 1023)]) for xx in range(w))
        row += b"".join(b"\xff\xff\xff" if (a[((y + yy) & 511) * 1024 + ((x + xx) & 1023)] ^ b[((y + yy) & 511) * 1024 + ((x + xx) & 1023)]) & 0x7FFF
                        else b"\0\0\0" for xx in range(w))
        rows.append(row)
    write_png(os.path.join(CAP, "cmp_%d_display.png" % n), rows)
    rows = [b"".join(rgb(c) for c in src[yy * 1024:(yy + 1) * 1024]) for src in (a, b) for yy in range(512)]
    rows += [b"".join(b"\xff\xff\xff" if (a[i] ^ b[i]) & 0x7FFF else b"\0\0\0" for i in range(yy * 1024, (yy + 1) * 1024))
             for yy in range(512)]
    write_png(os.path.join(CAP, "cmp_%d_vram.png" % n), rows)

    # Game state (g_SysWork, g_GameWork): differing fields, pointers skipped (PS1 8xxxxxxx vs the port's).
    for struct_name, file_tag in (("g_SysWork", "syswork"), ("g_GameWork", "gamework")):
        sa = os.path.join(CAP, "ps1_%d_%s.bin" % (n, file_tag))
        sb = os.path.join(CAP, "ps2_%d_%s.bin" % (n, file_tag))
        if not (os.path.exists(sa) and os.path.exists(sb)):
            continue
        wa, wb = open(sa, "rb").read(), open(sb, "rb").read()
        fields = []
        for off, sz, path in LAYOUTS[struct_name]:
            if off + sz > min(len(wa), len(wb)) or wa[off:off + sz] == wb[off:off + sz]:
                continue
            if sz == 4:
                va, vb = struct.unpack_from("<I", wa, off)[0], struct.unpack_from("<I", wb, off)[0]
                if va >> 24 == 0x80 and (vb >> 24 == 0 and vb != 0):
                    continue
                fields.append("%s: %d vs %d" % (path, struct.unpack_from("<i", wa, off)[0], struct.unpack_from("<i", wb, off)[0]))
            else:
                fields.append("%s: %s vs %s" % (path, wa[off:off + sz].hex(), wb[off:off + sz].hex()))
        lines.append("  %s: %d fields differ%s" % (struct_name, len(fields), ":" if fields else ""))
        lines += ["    " + f for f in fields[:30]]
    da_dt = open(os.path.join(CAP, "ps1_%d_disp.txt" % n)).read().split()[4:]
    db_dt = open(os.path.join(CAP, "ps2_%d_disp.txt" % n)).read().split()[4:]
    if da_dt and db_dt and da_dt != db_dt:
        lines.append("  g_DeltaTime: %s vs %s" % (da_dt[0], db_dt[0]))

    # GP0 streams, without the bits the GPU ignores.
    lines.append("  GP0: PS1 %d packets, PS2 %d" % (len(pa), len(pb)))
    differ = [i for i in range(min(len(pa), len(pb))) if normalize(pa[i]) != normalize(pb[i])]
    lines.append("  GP0: %d of the first %d packets differ" % (len(differ), min(len(pa), len(pb))))
    for i in differ[:6]:
        lines.append("    #%d PS1 %s" % (i, " ".join("%08X" % v for v in pa[i])))
        lines.append("    #%d PS2 %s" % (i, " ".join("%08X" % v for v in pb[i])))
    return "\n".join(lines)


LAYOUTS = {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", default="60,200,400,600")
    ap.add_argument("--skip-ps1", action="store_true", help="reuse the PS1 capture")
    ap.add_argument("--skip-ps2", action="store_true", help="reuse the PS2 capture")
    args = ap.parse_args()
    frames = sorted(int(f) for f in args.frames.split(","))
    os.makedirs(CAP, exist_ok=True)
    if not args.skip_ps2:
        capture_ps2(frames)
    if not args.skip_ps1:
        capture_ps1(frames)
    LAYOUTS["g_SysWork"] = struct_layout.layout("s_SysWork")
    LAYOUTS["g_GameWork"] = struct_layout.layout("s_GameWork")
    report = [compare(n) for n in frames
              if all(os.path.exists(os.path.join(CAP, "%s_%d_vram.bin" % (s, n))) for s in ("ps1", "ps2"))]
    text = "\n".join(report)
    open(os.path.join(CAP, "report.txt"), "w").write(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
