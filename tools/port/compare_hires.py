#!/usr/bin/env python3
"""Compares the port's 2x renderer (a replay's shadow buffer: compare_frames.py --replay --mode 1/3, which
writes build/port/cap/ps2r_N_hires.bin) with DuckStation's 1x frame (ps1_N_vram.bin) or a DuckStation 2x
screenshot (--ds2x PNG, 640x448).

Against 1x: each 2x2 block of the port's image, averaged, against the PS1 pixel it came from: overall
brightness ratio, block differences in 5-bit levels, the mean difference per dither cell (a dither
problem shows as a pattern there). Against a 2x screenshot: the same per pixel. Writes
build/port/cap/hires_N.png: reference (doubled) | port 2x | signed difference (red darker, green brighter).

    python3 tools/port/compare_hires.py N [--ds2x shot.png]
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CAP = os.path.join(REPO, "build", "port", "cap")
sys.path.insert(0, HERE)
import compare_frames as cf  # noqa: E402  (PNG writer)


def channels16(p):
    return ((p & 31) << 3, ((p >> 5) & 31) << 3, ((p >> 10) & 31) << 3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frame", type=int)
    ap.add_argument("--ds2x", help="DuckStation 2x screenshot (PNG, the display area)")
    args = ap.parse_args()
    n = args.frame
    ps1 = struct.unpack("<%dH" % (1024 * 512), open(os.path.join(CAP, "ps1_%d_vram.bin" % n), "rb").read())
    x0, y0, w, h = [int(v) for v in open(os.path.join(CAP, "ps1_%d_disp.txt" % n)).read().split()[:4]]
    data = open(os.path.join(CAP, "ps2r_%d_hires.bin" % n), "rb").read()
    sh, bits = struct.unpack_from("<ii", data, 0)
    k, hw = 1 << sh, 320 << sh

    def hires(x, y):  # 8-bit RGB at shadow buffer pixel (x, y)
        if bits == 16:
            return channels16(struct.unpack_from("<H", data, 8 + 2 * (y * hw + x))[0])
        v = struct.unpack_from("<I", data, 8 + 4 * (y * hw + x))[0]
        return (v & 255, (v >> 8) & 255, (v >> 16) & 255)

    ref = None
    if args.ds2x:
        from PIL import Image
        ref = Image.open(args.ds2x).convert("RGB").resize((w * k, h * k), Image.NEAREST).load()

    M = [[-4, 0, -3, 1], [2, -2, 3, -1], [-3, 1, -4, 0], [3, -1, 2, -2]]
    sum_a = sum_b = 0
    hist = {}
    cell = {}
    rows = []
    for y in range(h * k):
        row = b""
        for x in range(w * k):
            if ref is not None:
                a = ref[x, y]
            else:
                a = channels16(ps1[(y0 + y // k) * 1024 + x0 + x // k])
            row += bytes(a)
        rows.append(row)
    port_rows, diff_rows = [], []
    for y in range(h * k):
        pr, dr = b"", b""
        for x in range(w * k):
            b = hires(x0 * k + x, y0 * k + y)
            pr += bytes(b)
            a = rows[y][3 * x:3 * x + 3]
            d = sum(b) - sum(a)
            dr += bytes((min(255, max(0, -d)), min(255, max(0, d)), 0))
        port_rows.append(pr)
        diff_rows.append(dr)
    # Statistics: per reference pixel (1x) or per pixel (2x screenshot), in 5-bit levels.
    step = 1 if ref is not None else k
    for y in range(0, h * k, step):
        for x in range(0, w * k, step):
            a = rows[y][3 * x:3 * x + 3]
            bs = [port_rows[y + j][3 * (x + i):3 * (x + i) + 3] for j in range(step) for i in range(step)]
            for c in range(3):
                va = a[c] / 8.0
                vb = sum(bb[c] for bb in bs) / 8.0 / len(bs)
                sum_a += va
                sum_b += vb
                d = round(vb - va)
                hist[d] = hist.get(d, 0) + 1
                ck = M[(y // step) & 3][(x // step) & 3]
                cell.setdefault(ck, [0, 0])
                cell[ck][0] += vb - va
                cell[ck][1] += 1
    total = sum(hist.values())
    print("frame %d (%dx shadow buffer, %d-bit) against %s:" % (n, k, bits, "DuckStation 2x" if ref is not None else "DuckStation 1x (2x2 blocks averaged)"))
    print("  brightness port / reference: %.4f" % (sum_b / max(1, sum_a)))
    print("  channel differences (levels): " + "  ".join("%+d: %.2f%%" % (d, 100.0 * c / total) for d, c in sorted(hist.items()) if c * 1000 >= total))
    print("  mean difference per dither cell: " + "  ".join("%+d: %+.3f" % (ck, cell[ck][0] / cell[ck][1]) for ck in sorted(cell)))
    out = [r + b"\x10\x10\x10" * 8 + p + b"\x10\x10\x10" * 8 + d for r, p, d in zip(rows, port_rows, diff_rows)]
    cf.write_png(os.path.join(CAP, "hires_%d.png" % n), out)
    print("  image: %s" % os.path.join(CAP, "hires_%d.png" % n))


if __name__ == "__main__":
    main()
