#!/usr/bin/env python3
"""Compares frames of an attract demo (--demo N: the Nth played, default the first) between the PS1 game in DuckStation (software renderer,
the reference) and the port in PCSX2 (current build/port/sh1_ps2.iso; build it without SH1_BENCH,
which desyncs the demo).

Frames are demo frames (g_Demo_DemoStep), so both runs capture the same moment. For each frame,
build/port/cap/ gets:
  - cmp_<N>_display.png: displayed area, PS1 | PS2 | difference (white = differs)
  - cmp_<N>_vram.png:    the whole 1024x512 VRAM, PS1 above PS2, then the difference
and the report lists pixel differences and the first GP0 packets that differ (same packets but
different pixels: the renderer; different packets: game logic, GTE, ...).

--mode N forces the port's Debug Options rendering bits (1 2x, 2 32-bit, 4 480p); 2x captures are
read back scaled down to VRAM. --walk (with --load) has Harry turn 90 degrees right and walk 1.5 s
first, frames then counting from the walk's end. --load F,D loads file F's Data D on card 1 instead, through Load Game
(DuckStation's card 1 must hold the same saves) and frames are vertical blanks of gameplay.

--replay replays the DuckStation captures of --frames (already made) through the port's GS renderer
(capture_ps2.c) and compares those instead: the renderer alone, with the PS1's exact packets.

    python3 tools/port/compare_frames.py [--frames 60,200,400] [--demo 2 | --load 2,10] [--mode 3] [--skip-ps1] [--skip-ps2]
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

def capture_ps2(frames, demo, mode, cont, walk=False):
    open(os.path.join(REPO, "build", "port", "capture.txt"), "w").write(
        ",".join(map(str, frames)) + " demo=%d" % demo + (" mode=%d" % mode if mode is not None else "") +
        (" load=%s" % cont.replace(",", ":") if cont else "") + (" walk" if walk else "") + (" state=%s" % STATE_RANGE if STATE_RANGE else "") + "".join(" dump=%x:%x" % (g[2], g[3]) for g in GLOBALS) + "".join(" hold=%d:%d" % h for h in HOLDS) + "\n")
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


def replay_ps2(frames):
    open(os.path.join(REPO, "build", "port", "capture.txt"), "w").write(" ".join("replay=%d" % f for f in frames) + "\n")
    try:
        out = subprocess.run([sys.executable, os.path.join(HERE, "pcsx2_run.py"), "--seconds", "120",
                              "--until", "capture: all frames done", ISO], capture_output=True, text=True).stdout
    finally:
        os.remove(os.path.join(REPO, "build", "port", "capture.txt"))
    print("\n".join(l.split("] ", 1)[-1] for l in out.splitlines() if "capture:" in l or l.startswith("pcsx2_run:")))
    for f in frames:  # the replay draws the PS1's packets: its display area and game state are the PS1's
        for ext in ("_disp.txt", "_gp0.bin", "_syswork.bin", "_gamework.bin"):
            src = os.path.join(CAP, "ps1_%d%s" % (f, ext))
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(CAP, "ps2r_%d%s" % (f, ext)))


def capture_ps1(frames, demo, cont, walk=False):
    backup = patch_duckstation()
    ds = None
    try:
        ds = subprocess.Popen([DUCKSTATION, "-batch", "-fastboot", "-nofullscreen", "--", CUE], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, start_new_session=True)
        if not warp_sweep.wait_port(2345, 30):
            sys.exit("DuckStation's GDB server didn't start")
        time.sleep(1)
        env = dict(os.environ, CAP_FRAMES=",".join(map(str, frames)), CAP_OUT=CAP, CAP_DEMO=str(demo), CAP_LOAD=cont or "", CAP_WALK="1" if walk else "0", CAP_STATE=STATE_RANGE or "",
                   CAP_DUMP=",".join("%x:%x" % (g[1], g[3]) for g in GLOBALS))
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
    the UV words after the second (the first holds the CLUT, the second the texture page); for
    gouraud polygons, the top byte of the colour words after the first (the command byte's place)."""
    cmd = words[0] >> 24
    if 0x20 <= cmd < 0x40:
        gouraud, textured, nv = cmd & 0x10, cmd & 0x04, 4 if cmd & 0x08 else 3
        w = list(words)
        stride = (2 + (1 if textured else 0)) if gouraud else (1 + (1 if textured else 0))
        for i in range(nv):
            if textured and i >= 2:
                k = (2 + i * 3) if gouraud else (2 + i * 2)
                if k < len(w):
                    w[k] &= 0xFFFF
            if gouraud and i >= 1 and i * stride < len(w):
                w[i * stride] &= 0xFFFFFF
        return tuple(w)
    return tuple(words)


def compare(n, side2="ps2"):
    a, da, pa = load("ps1", n)
    b, db, pb = load(side2, n)
    x, y, w, h = da
    x2, y2 = db[0], db[1]  # the PS2's displayed buffer (the double buffer's parity can differ)
    lines = ["frame %d: display PS1 %s, PS2 %s" % (n, da, db)]

    def pa_px(xx, yy):
        return a[((y + yy) & 511) * 1024 + ((x + xx) & 1023)]

    def pb_px(xx, yy):
        return b[((y2 + yy) & 511) * 1024 + ((x2 + xx) & 1023)]

    # Displayed areas (each side's own), and all of VRAM; bit 15 (mask) compared separately.
    dd = sum(1 for yy in range(h) for xx in range(w) if (pa_px(xx, yy) ^ pb_px(xx, yy)) & 0x7FFF)
    dm = sum(1 for yy in range(h) for xx in range(w) if (pa_px(xx, yy) ^ pb_px(xx, yy)) & 0x8000)
    vd = sum(1 for i in range(1024 * 512) if (a[i] ^ b[i]) & 0x7FFF)
    vm = sum(1 for i in range(1024 * 512) if (a[i] ^ b[i]) & 0x8000)
    lines.append("  display: %d of %d pixels differ (bit 15: %d); VRAM: %d of 524288 (bit 15: %d)%s" % (
        dd, w * h, dm, vd, vm, "" if (x, y) == (x2, y2) else " (buffers swapped: VRAM totals not comparable)"))

    rows = []
    for yy in range(h):
        row = b"".join(rgb(pa_px(xx, yy)) for xx in range(w)) + b"".join(rgb(pb_px(xx, yy)) for xx in range(w))
        row += b"".join(b"\xff\xff\xff" if (pa_px(xx, yy) ^ pb_px(xx, yy)) & 0x7FFF else b"\0\0\0" for xx in range(w))
        rows.append(row)
    write_png(os.path.join(CAP, "cmp%s_%d_display.png" % (side2[3:], n)), rows)
    rows = [b"".join(rgb(c) for c in src[yy * 1024:(yy + 1) * 1024]) for src in (a, b) for yy in range(512)]
    rows += [b"".join(b"\xff\xff\xff" if (a[i] ^ b[i]) & 0x7FFF else b"\0\0\0" for i in range(yy * 1024, (yy + 1) * 1024))
             for yy in range(512)]
    write_png(os.path.join(CAP, "cmp%s_%d_vram.png" % (side2[3:], n)), rows)

    # Game state (g_SysWork, g_GameWork): differing fields, pointers skipped (PS1 8xxxxxxx vs the port's).
    for struct_name, file_tag in (("g_SysWork", "syswork"), ("g_GameWork", "gamework")):
        sa = os.path.join(CAP, "ps1_%d_%s.bin" % (n, file_tag))
        sb = os.path.join(CAP, "%s_%d_%s.bin" % (side2, n, file_tag))
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
    db_dt = open(os.path.join(CAP, "%s_%d_disp.txt" % (side2, n))).read().split()[4:]
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
STATE_RANGE = None  # --state A:B
GLOBALS = []        # --globals: (name, PS1 address, port address, size)
HOLDS = []          # --ps1-holds: (step, frames) the PS1 spent more than a frame on


def resolve_globals(spec):
    """name[:size],...: PS1 addresses from configs/USA/sym.*.txt (or a D_xxxxxxxx name's own), the port's
    and the size from sh1.elf's symbols."""
    import glob
    import re
    syms = {}
    for f in glob.glob(os.path.join(REPO, "configs", "USA", "sym.*.txt")):
        for line in open(f):
            m = re.match(r"\s*(\w+)\s*=\s*0x([0-9A-Fa-f]+);", line)
            if m:
                syms.setdefault(m.group(1), int(m.group(2), 16))
    nm = subprocess.run(["mips64r5900el-ps2-elf-nm", "-S", os.path.join(REPO, "build", "port", "sh1.elf")],
                        capture_output=True, text=True).stdout
    port = {}
    for line in nm.splitlines():
        p = line.split()
        if len(p) == 4:
            port[p[3]] = (int(p[0], 16), int(p[1], 16))
        elif len(p) == 3:
            port.setdefault(p[2], (int(p[0], 16), 4))  # no size: give one (name:size)
    out = []
    for item in spec.split(","):
        name, _, size = item.partition(":")
        ps1 = syms.get(name) or (int(name[2:], 16) if re.match(r"D_8[0-9A-F]{7}$", name) else None)
        if ps1 is None or name not in port:
            sys.exit("--globals: %s not found (PS1 %s, port %s)" % (name, ps1, name in port))
        out.append((name, ps1, port[name][0], int(size, 0) if size else port[name][1]))
    return out


def compare_states(args):
    """--state: both sides' game state at every demo step of the range; the first steps where they
    differ, field by field (pointers skipped, as compare())."""
    os.makedirs(CAP, exist_ok=True)
    extra = sum(g[3] for g in GLOBALS)
    rec = 12 + 0x2768 + 0x5D8 + extra
    if not args.skip_ps1:
        capture_ps1([0], args.demo, args.cont, args.walk)
    if args.ps1_holds:
        # The PS1's load timing: how many frames it spent on each demo step that took more than one.
        d = open(os.path.join(CAP, "ps1_state.bin"), "rb").read()
        counts = {}
        for pos in range(0, len(d) - rec + 1, rec):
            step = struct.unpack_from("<i", d, pos)[0]
            counts[step] = counts.get(step, 0) + 1
        HOLDS[:] = sorted((s, n) for s, n in counts.items() if n > 1 and s > 0)  # step 0: the start, before gameplay
        print("state: PS1 holds (step, frames): %s" % HOLDS)
    if not args.skip_ps2:
        capture_ps2([], args.demo, args.mode, args.cont, args.walk)
    sys_l, game_l = struct_layout.layout("s_SysWork"), struct_layout.layout("s_GameWork")

    def read(side):
        d, out = open(os.path.join(CAP, "%s_state.bin" % side), "rb").read(), {}
        for pos in range(0, len(d) - rec + 1, rec):
            step, seed, dt = struct.unpack_from("<iIi", d, pos)
            out.setdefault(step, ((seed, dt), d[pos + 12:pos + 12 + 0x2768], d[pos + 12 + 0x2768:pos + rec - extra],
                                  d[pos + rec - extra:pos + rec]))
        return out

    a, b = read("ps1"), read("ps2")
    shown, summary = 0, {}
    for step in sorted(set(a) & set(b)):
        fields = []
        if a[step][0][0] != b[step][0][0]:
            fields.append("RNG seed: %08X vs %08X" % (a[step][0][0], b[step][0][0]))
        off = 0
        for name, _, _, size in GLOBALS:
            ga, gb = a[step][3][off:off + size], b[step][3][off:off + size]
            if ga != gb:
                fields.append("%s: %s vs %s" % (name, ga.hex(), gb.hex()))
            off += size
        if a[step][0][1] != b[step][0][1]:
            fields.append("g_DeltaTime: %d vs %d" % (a[step][0][1], b[step][0][1]))
        for layout, wa, wb, name in ((sys_l, a[step][1], b[step][1], "g_SysWork"), (game_l, a[step][2], b[step][2], "g_GameWork")):
            for off, sz, path in layout:
                if off + sz > len(wa) or wa[off:off + sz] == wb[off:off + sz] or path.startswith(("field_2514", "field_254C", "gameStateCounter", "gameStateStepCounter", "sysStateCounter", "savegame.gameplayTimer", "controllers[0].pulseTicks", "controllers[1].pulseTicks")):
                    continue
                if sz == 4:
                    va, vb = struct.unpack_from("<I", wa, off)[0], struct.unpack_from("<I", wb, off)[0]
                    if va >> 24 == 0x80 and vb >> 24 == 0 and vb != 0:
                        continue
                    fields.append("%s.%s: %d vs %d" % (name, path, struct.unpack_from("<i", wa, off)[0], struct.unpack_from("<i", wb, off)[0]))
                elif sz == 2:
                    fields.append("%s.%s: %d vs %d" % (name, path, struct.unpack_from("<h", wa, off)[0], struct.unpack_from("<h", wb, off)[0]))
                else:
                    fields.append("%s.%s: %s vs %s" % (name, path, wa[off:off + sz].hex(), wb[off:off + sz].hex()))
        if fields and shown < 3:
            print("step %d: %d fields differ" % (step, len(fields)))
            for f in fields[:25]:
                print("    " + f)
            shown += 1
        for f in fields:
            name = f.split(":")[0].split("[")[0] if "BoneCoords" in f else f.split(":")[0]
            summary.setdefault(name, [0, step])[0] += 1
    print("state: %d steps compared (PS1 %d, PS2 %d)" % (len(set(a) & set(b)), len(a), len(b)))
    for name, (count, first) in sorted(summary.items(), key=lambda kv: kv[1][1]):
        print("state:   %-60s differs in %5d steps, from step %d" % (name, count, first))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", default="60,200,400,600")
    ap.add_argument("--skip-ps1", action="store_true", help="reuse the PS1 capture")
    ap.add_argument("--skip-ps2", action="store_true", help="reuse the PS2 capture")
    ap.add_argument("--demo", type=int, default=1, help="which attract demo (1: the first played)")
    ap.add_argument("--load", dest="cont", metavar="F,D", help="load file F's Data D on card 1 instead of a demo")
    ap.add_argument("--state", metavar="A:B", help="only the game state at every demo step A-B (ps1/ps2_state.bin), "
                    "compared step by step: where the game first diverges")
    ap.add_argument("--ps1-holds", action="store_true", help="with --state: the port's demo playback holds each step "
                    "as many frames as the PS1's did (its load timing; captures DuckStation first)")
    ap.add_argument("--globals", help="with --state: also these globals, name[:size],... (symbol names)")
    ap.add_argument("--walk", action="store_true", help="with --load: turn 90 degrees right and walk 1.5 s first; frames count from its end")
    ap.add_argument("--replay", action="store_true", help="replay the DuckStation captures through the port's renderer")
    ap.add_argument("--mode", type=int, help="the port's rendering option bits (1 2x, 2 32-bit, 4 480p)")
    args = ap.parse_args()
    global STATE_RANGE, GLOBALS
    STATE_RANGE = args.state
    GLOBALS = resolve_globals(args.globals) if args.globals else []
    frames = sorted(int(f) for f in args.frames.split(","))
    if args.state:
        compare_states(args)
        return
    os.makedirs(CAP, exist_ok=True)
    if args.replay:
        replay_ps2(frames)
    elif not args.skip_ps2:
        capture_ps2(frames, args.demo, args.mode, args.cont, args.walk)
    if not args.skip_ps1 and not args.replay:
        capture_ps1(frames, args.demo, args.cont, args.walk)
    LAYOUTS["g_SysWork"] = struct_layout.layout("s_SysWork")
    LAYOUTS["g_GameWork"] = struct_layout.layout("s_GameWork")
    side2 = "ps2r" if args.replay else "ps2"
    report = [compare(n, side2) for n in frames
              if all(os.path.exists(os.path.join(CAP, "%s_%d_vram.bin" % (s, n))) for s in ("ps1", side2))]
    text = "\n".join(report)
    open(os.path.join(CAP, "report.txt"), "w").write(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
