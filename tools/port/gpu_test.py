#!/usr/bin/env python3
"""Synthetic GPU test: the same VRAM and GP0 packets drawn by DuckStation's software renderer (the
reference, through the game's own LoadImage/DrawOTag/StoreImage from gdb: gdb/gpu_test_ps1.py) and
by the port's GS renderer (capture_ps2.c's replay), then compared pixel by pixel, case by case.

Layout: 108 cells of 48x48 (x 0-575, y 0-431), one primitive configuration each (its own clip area and
drawing offset) over a varied background; CLUTs at y 448+; texture pages at x 640+ (4-bit page at
x 640, 8-bit at 704, 15-bit at 640/256).

--uv: texture coordinates instead: the 15-bit page's texels encode their own (u, v) (R = u & 31,
G = v & 31, B = u >> 5 | (v >> 5) << 3), and every cell draws raw-textured polygons whose UVs follow
a linear map of the screen; the texel each pixel sampled is decoded on both sides and compared.

    python3 tools/port/gpu_test.py [--seed 1] [--uv] [--skip-ps1] [--skip-ps2] [--show CASE]
"""
import argparse
import os
import random
import shutil
import signal
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CAP = os.path.join(REPO, "build", "port", "cap")
sys.path.insert(0, HERE)
import compare_frames as cf  # noqa: E402  (DuckStation launch, PCSX2 runs, PNG writer)

TEST_ID = 9000  # replay frame number used for the test files
CELL, COLS, ROWS = 48, 12, 9
CLUT8 = (0, 448)                                 # 256 entries
CLUT4 = [(256 + 16 * i, 448) for i in range(8)]  # 16 entries each
PAGE4, PAGE8, PAGE15 = (10, 0), (11, 0), (10, 1)  # (tpage x / 64, tpage y / 256)


def rgb15(r, g, b, stp=0):
    return (r & 31) | ((g & 31) << 5) | ((b & 31) << 10) | (stp << 15)


def make_vram(rng):
    v = [0] * (1024 * 512)
    for y in range(512):
        for x in range(640):
            # Smooth gradients with noise: every level from dark to bright appears under the cells.
            r = (x * 31 // 639 + rng.randint(0, 3)) % 32
            g = (y * 31 // 511 + rng.randint(0, 3)) % 32
            b = ((x + y) * 31 // 1150 + rng.randint(0, 2)) % 32
            v[y * 1024 + x] = rgb15(r, g, b, 1 if rng.random() < 0.05 else 0)
    for y in range(512):
        for x in range(640, 1024):
            # Texture pages: random 16-bit words (4/8-bit indices pack into them); some transparent
            # (0x0000) and some semi-transparent (bit 15) texels in the 15-bit page.
            w = rng.getrandbits(16)
            if y >= 256 and x < 896:
                w = 0 if rng.random() < 0.1 else rgb15(rng.randint(0, 31), rng.randint(0, 31), rng.randint(0, 31),
                                                       1 if rng.random() < 0.4 else 0)
            v[y * 1024 + x] = w
    for i in range(256):
        v[CLUT8[1] * 1024 + CLUT8[0] + i] = 0 if i == 0 else rgb15(rng.randint(0, 31), rng.randint(0, 31),
                                                                   rng.randint(0, 31), 1 if i % 3 == 0 else 0)
    for cx, cy in CLUT4:
        for i in range(16):
            v[cy * 1024 + cx + i] = 0 if i == 0 else rgb15(rng.randint(0, 31), rng.randint(0, 31), rng.randint(0, 31),
                                                           1 if i % 2 == 0 else 0)
    return v


def cases():
    """(name, params) per cell."""
    out = []
    for semi in (None, 0, 1, 2, 3):
        for dither in (0, 1):
            for shape in ("flat3", "gour3", "flat4", "gour4"):
                if shape.startswith("flat") and dither:
                    continue
                out.append(("%s semi%s dith%d" % (shape, semi, dither), dict(kind="poly", shape=shape, tex=None, semi=semi, dither=dither)))
    for depth in (4, 8, 15):
        for raw in (0, 1):
            for semi in (None, 0, 1, 2, 3):
                for gour in ((0,) if raw else (0, 1)):
                    out.append(("tex%d %s %s semi%s" % (depth, "raw" if raw else ("gour" if gour else "mod"), "quad", semi),
                                dict(kind="poly", shape="gour4" if gour else "flat4", tex=depth, raw=raw, semi=semi, dither=1)))
    for tex in (None, 4, 15):
        for semi in (None, 1):
            out.append(("rect tex%s semi%s" % (tex, semi), dict(kind="rect", tex=tex, semi=semi, raw=0, dither=1)))
    return out[:COLS * ROWS]


def tpage_word(depth, semi, dither):
    page = {4: PAGE4, 8: PAGE8, 15: PAGE15, None: PAGE4}[depth]
    d = {4: 0, 8: 1, 15: 2, None: 0}[depth]
    return page[0] | (page[1] << 4) | ((semi or 0) << 5) | (d << 7) | (dither << 9) | (1 << 10)


def packets(rng):
    pk = []
    for i, (name, c) in enumerate(cases()):
        ox, oy = (i % COLS) * CELL, (i // COLS) * CELL
        pk.append([0xE3000000 | ox | (oy << 10)])
        pk.append([0xE4000000 | (ox + CELL - 1) | ((oy + CELL - 1) << 10)])
        pk.append([0xE5000000 | (ox & 0x7FF) | ((oy & 0x7FF) << 11)])
        pk.append([0xE6000000])
        pk.append([0xE2000000])
        tp = tpage_word(c.get("tex"), c["semi"], c["dither"])
        pk.append([0xE1000000 | tp])
        semi = 2 if c["semi"] is not None else 0

        def col():
            top = 127 if DARK else 255
            return rng.randint(0, top) | (rng.randint(0, top) << 8) | (rng.randint(0, top) << 16)

        def xy():
            return (rng.randint(-6, CELL + 5) & 0xFFFF) | ((rng.randint(-6, CELL + 5) & 0xFFFF) << 16)

        clut = (CLUT8[0] // 16) | (CLUT8[1] << 6) if c.get("tex") == 8 else (CLUT4[i % 8][0] // 16) | (CLUT4[i % 8][1] << 6)
        if c["kind"] == "poly":
            nv = 4 if c["shape"].endswith("4") else 3
            gour = c["shape"].startswith("gour")
            tex = c.get("tex")
            cmd = 0x20 | (0x10 if gour else 0) | (0x08 if nv == 4 else 0) | (0x04 if tex else 0) | semi | (1 if c.get("raw") else 0)
            if tex and c.get("raw"):
                gour = False
            w = [(cmd << 24) | col()]
            for k in range(nv):
                if gour and k > 0:
                    w.append(col())
                w.append(xy())
                if tex:
                    uv = rng.randint(0, 255) | (rng.randint(0, 255) << 8)
                    if k == 0:
                        uv |= clut << 16
                    elif k == 1:
                        uv |= tp << 16
                    w.append(uv)
            pk.append(w)
        else:
            tex = c["tex"]
            cmd = 0x60 | (0x04 if tex else 0) | semi
            w = [(cmd << 24) | col(), (rng.randint(-4, 20) & 0xFFFF) | ((rng.randint(-4, 20) & 0xFFFF) << 16)]
            if tex:
                w.append(rng.randint(0, 200) | (rng.randint(0, 200) << 8) | (clut << 16))
            w.append(rng.randint(8, 40) | (rng.randint(8, 40) << 16))
            pk.append(w)
    return pk


UV_MODE = False
DARK = False  # --dark: vertex colours 0-127 only


def uv_vram(rng):
    v = make_vram(rng)
    px, py = PAGE15[0] * 64, PAGE15[1] * 256
    for t in range(256):
        for s_ in range(256):
            if px + s_ < 1024:
                v[(py + t) * 1024 + px + s_] = rgb15(s_ & 31, t & 31, (s_ >> 5) | ((t >> 5) << 3))
    return v


def uv_cases():
    return [("uv %s %s" % ("tri" if i % 2 else "quad", ["scale1", "scale2", "half", "rot"][i // 2 % 4]), dict(i=i))
            for i in range(COLS * ROWS)]


def uv_packets(rng):
    import math
    pk = []
    tp = PAGE15[0] | (PAGE15[1] << 4) | (2 << 7) | (1 << 10)
    for i, (name, c) in enumerate(uv_cases()):
        ox, oy = (i % COLS) * CELL, (i // COLS) * CELL
        pk += [[0xE3000000 | ox | (oy << 10)], [0xE4000000 | (ox + CELL - 1) | ((oy + CELL - 1) << 10)],
               [0xE5000000 | (ox & 0x7FF) | ((oy & 0x7FF) << 11)], [0xE6000000], [0xE2000000], [0xE1000000 | tp]]
        kind = i // 2 % 4
        scale = [1.0, 2.0, 0.5, rng.uniform(0.6, 1.8)][kind]
        ang = math.radians(rng.uniform(0, 360)) if kind == 3 else 0.0
        u0, v0 = rng.randint(20, 120), rng.randint(20, 120)
        nv = 3 if i % 2 else 4
        if nv == 4 and kind != 3:
            x0, y0, w, h = rng.randint(0, 10), rng.randint(0, 10), rng.randint(16, 36), rng.randint(16, 36)
            pts = [(x0, y0), (x0 + w, y0), (x0, y0 + h), (x0 + w, y0 + h)]
        else:
            pts = [(rng.randint(0, CELL - 1), rng.randint(0, CELL - 1)) for _ in range(nv)]
            if nv == 4:
                cx, cy, r = CELL // 2, CELL // 2, rng.randint(12, 20)
                pts = [(int(cx + r * math.cos(ang + a)), int(cy + r * math.sin(ang + a))) for a in (math.pi * 1.25, math.pi * 1.75, math.pi * 0.75, math.pi * 0.25)]
        uvs = []
        for x, y in pts:
            u = u0 + scale * (x * math.cos(ang) + y * math.sin(ang))
            v = v0 + scale * (-x * math.sin(ang) + y * math.cos(ang))
            uvs.append((max(0, min(255, int(round(u)))), max(0, min(255, int(round(v))))))
        cmd = 0x25 | (0x08 if nv == 4 else 0)  # raw textured
        w = [(cmd << 24) | 0x808080]
        for k, ((x, y), (u, v)) in enumerate(zip(pts, uvs)):
            w.append((x & 0xFFFF) | ((y & 0xFFFF) << 16))
            uvw = u | (v << 8)
            if k == 0:
                uvw |= ((CLUT4[0][0] // 16) | (CLUT4[0][1] << 6)) << 16
            elif k == 1:
                uvw |= tp << 16
            w.append(uvw)
        pk.append(w)
    return pk


def write_inputs(seed):
    rng = random.Random(seed)
    vram = uv_vram(rng) if UV_MODE else make_vram(rng)
    pk = uv_packets(rng) if UV_MODE else packets(rng)
    os.makedirs(CAP, exist_ok=True)
    open(os.path.join(CAP, "gpu_in_vram.bin"), "wb").write(struct.pack("<%dH" % len(vram), *vram))
    with open(os.path.join(CAP, "gpu_gp0.bin"), "wb") as f:
        for w in pk:
            f.write(struct.pack("<I", len(w)) + struct.pack("<%dI" % len(w), *w))


def run_ps1():
    backup = cf.patch_duckstation()
    ds = None
    try:
        ds = subprocess.Popen([cf.DUCKSTATION, "-batch", "-fastboot", "-nofullscreen", "--", cf.CUE], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, start_new_session=True)
        if not cf.warp_sweep.wait_port(2345, 30):
            sys.exit("DuckStation's GDB server didn't start")
        time.sleep(1)
        out = subprocess.run(["gdb-multiarch", "-q", "-batch", "-ex", "set architecture mips:3000", "-ex",
                              "target remote localhost:2345", "-x", os.path.join(HERE, "gdb", "gpu_test_ps1.py")],
                             env=dict(os.environ, GPU_TEST_DIR=CAP), capture_output=True, text=True, timeout=900,
                             stdin=subprocess.DEVNULL)
        print("\n".join(l for l in (out.stdout + out.stderr).splitlines() if "gpu test" in l or "rror" in l))
    finally:
        if ds:
            for sig in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(ds.pid, sig)
                    ds.wait(5)
                    break
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    pass
        shutil.copy2(backup, cf.warp_sweep.SETTINGS)
        os.remove(backup)


def run_ps2():
    shutil.copy2(os.path.join(CAP, "gpu_in_vram.bin"), os.path.join(CAP, "ps1_%d_vram.bin" % TEST_ID))
    shutil.copy2(os.path.join(CAP, "gpu_gp0.bin"), os.path.join(CAP, "ps1_%d_gp0.bin" % TEST_ID))
    open(os.path.join(REPO, "build", "port", "capture.txt"), "w").write("replay=%d\n" % TEST_ID)
    try:
        subprocess.run([sys.executable, os.path.join(HERE, "pcsx2_run.py"), "--seconds", "120", "--until",
                        "capture: all frames done", cf.ISO], capture_output=True, text=True)
    finally:
        os.remove(os.path.join(REPO, "build", "port", "capture.txt"))
    shutil.copy2(os.path.join(CAP, "ps2r_%d_vram.bin" % TEST_ID), os.path.join(CAP, "gpu_ps2_vram.bin"))


def compare_uv():
    ld = lambda n: struct.unpack("<%dH" % (1024 * 512), open(os.path.join(CAP, n), "rb").read())
    a, b, src = ld("gpu_ps1_vram.bin"), ld("gpu_ps2_vram.bin"), ld("gpu_in_vram.bin")
    dec = lambda p: ((p & 31) | (((p >> 10) & 7) << 5), ((p >> 5) & 31) | (((p >> 13) & 3) << 5))
    allc = {}
    for i, (name, _) in enumerate(uv_cases()):
        ox, oy = (i % COLS) * CELL, (i // COLS) * CELL
        deltas, only = {}, 0
        for y in range(oy, oy + CELL):
            for x in range(ox, ox + CELL):
                pa, pb, ps = a[y * 1024 + x], b[y * 1024 + x], src[y * 1024 + x]
                if pa == pb:
                    if pa != ps:
                        deltas[(0, 0)] = deltas.get((0, 0), 0) + 1
                    continue
                if pa == ps or pb == ps:
                    only += 1  # drawn on one side only (edge)
                    continue
                (ua, va), (ub, vb) = dec(pa), dec(pb)
                d = (ub - ua, vb - va)
                deltas[d] = deltas.get(d, 0) + 1
        for k, n in deltas.items():
            allc[(name.split()[1], k)] = allc.get((name.split()[1], k), 0) + n
        if i < 16:
            print("%3d %-16s edge-only %3d  (du,dv):count %s" % (i, name, only, " ".join("%s:%d" % kv for kv in sorted(deltas.items(), key=lambda kv: -kv[1])[:6])))
    print("by kind:")
    for kind in ("scale1", "scale2", "half", "rot"):
        tot = sum(n for (k, d), n in allc.items() if k == kind)
        print("  %-7s %s" % (kind, " ".join("%s:%.1f%%" % (d, 100.0 * n / tot) for (k, d), n in sorted(allc.items(), key=lambda kv: -kv[1]) if k == kind)[:300]))


def compare(show):
    if UV_MODE:
        compare_uv()
        return
    ld = lambda n: struct.unpack("<%dH" % (1024 * 512), open(os.path.join(CAP, n), "rb").read())
    a, b, src = ld("gpu_ps1_vram.bin"), ld("gpu_ps2_vram.bin"), ld("gpu_in_vram.bin")
    total = 0
    for i, (name, _) in enumerate(cases()):
        ox, oy = (i % COLS) * CELL, (i // COLS) * CELL
        n = big = m15 = drawn = 0
        hist = {}
        for y in range(oy, oy + CELL):
            for x in range(ox, ox + CELL):
                pa, pb = a[y * 1024 + x], b[y * 1024 + x]
                drawn += pa != src[y * 1024 + x]
                if pa == pb:
                    continue
                n += 1
                m15 += (pa ^ pb) >> 15
                d = max(abs(((pa >> s) & 31) - ((pb >> s) & 31)) for s in (0, 5, 10))
                big += d > 1
                for s in (0, 5, 10):
                    dd = ((pb >> s) & 31) - ((pa >> s) & 31)
                    if dd:
                        hist[dd] = hist.get(dd, 0) + 1
        total += n
        print("%3d %-28s drawn %4d  differ %4d (>1 level %4d, bit 15 %3d)  %s" % (
            i, name, drawn, n, big, m15, " ".join("%+d:%d" % kv for kv in sorted(hist.items()))))
    print("gpu test: %d pixels differ in all" % total)
    rows = []
    for y in range(ROWS * CELL):
        rows.append(b"".join(cf.rgb(a[y * 1024 + x]) for x in range(COLS * CELL)) +
                    b"".join(cf.rgb(b[y * 1024 + x]) for x in range(COLS * CELL)) +
                    b"".join(b"\xff\xff\xff" if a[y * 1024 + x] != b[y * 1024 + x] else b"\0\0\0" for x in range(COLS * CELL)))
    cf.write_png(os.path.join(CAP, "gpu_test.png"), rows)
    if show is not None:
        ox, oy = (show % COLS) * CELL, (show // COLS) * CELL
        for y in range(oy, oy + CELL):
            print(" ".join("%04X/%04X" % (a[y * 1024 + x], b[y * 1024 + x]) if a[y * 1024 + x] != b[y * 1024 + x] else "    .    "
                           for x in range(ox, ox + 16)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--skip-ps1", action="store_true")
    ap.add_argument("--skip-ps2", action="store_true")
    ap.add_argument("--show", type=int, help="print a case's first 16 columns (PS1/PS2 values)")
    ap.add_argument("--uv", action="store_true", help="texture coordinate test (see above)")
    ap.add_argument("--dark", action="store_true", help="vertex colours 0-127 only (as most of the game's)")
    args = ap.parse_args()
    global UV_MODE, DARK
    UV_MODE, DARK = args.uv, args.dark
    write_inputs(args.seed)
    if not args.skip_ps1:
        run_ps1()
    if not args.skip_ps2:
        run_ps2()
    compare(args.show)


if __name__ == "__main__":
    main()
