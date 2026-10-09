#!/usr/bin/env python3
"""Reference XA-ADPCM decoder (written from psx-spx "CDROM XA Audio ADPCM Compression"),
independent of the IOP's (src/port/iop/sh1spu/xa.c), for checking it against the disc.

Decodes one file/channel of the XA data in HILL. (raw 2336-byte sectors) from a PS1 sector index
until its end-of-file flag, and writes a WAV at the stream's own rate; with --out48, also a 48 kHz
one resampled with the PS1 SPU's Gaussian table (as sh1spu does).

    python3 tools/port/xa_test/xa_ref.py --index 0 --file 1 --chan 0 --out ref.wav --out48 ref48.wav
"""
import argparse
import os
import re
import struct
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
K0 = (0, 60, 115, 98)
K1 = (0, 0, -52, -55)


def gauss():
    text = open(os.path.join(REPO, "src", "port", "iop", "sh1spu", "gauss_table.h")).read()
    vals = [int(v) for v in re.findall(r"-?\d+", text.split("{", 1)[1])]
    return vals[:512]


def decode_sector(data, stereo, state):
    """data: 2304 bytes (18 groups). state: [oldL, olderL, oldR, olderR]. Returns (left, right)."""
    left, right = [], []
    for g in range(18):
        grp = data[g * 128:(g + 1) * 128]
        for blk in range(4):
            for nib in range(2):
                param = grp[4 + blk * 2 + nib]
                shift = 12 - (param & 0xF)
                f = (param >> 4) & 3
                ch = nib if stereo else 0
                old, older = state[ch * 2], state[ch * 2 + 1]
                out = left if (not stereo or nib == 0) else right
                for j in range(28):
                    t = (grp[16 + blk + j * 4] >> (nib * 4)) & 0xF
                    t = t - 16 if t >= 8 else t
                    s = (t << shift) if shift >= 0 else (t >> -shift)
                    p = old * K0[f] + older * K1[f] + 32
                    s += p >> 6  # psx-spx writes "/64"; emulators shift (rounds toward -inf)
                    s = max(-32768, min(32767, s))
                    out.append(s)
                    older, old = old, s
                state[ch * 2], state[ch * 2 + 1] = old, older
    return left, (right if stereo else left)


def stream(index, file, chan, hill):
    st = [0, 0, 0, 0]
    L, R, rate = [], [], None
    with open(hill, "rb") as f:
        while True:
            f.seek(index * 2336)
            raw = f.read(2336)
            if len(raw) < 2336:
                break
            index += 1
            if raw[0] != file or raw[1] != chan or not (raw[2] & 0x04):
                continue
            stereo = (raw[3] & 3) == 1
            rate = 18900 if (raw[3] >> 2) & 3 else 37800
            l, r = decode_sector(raw[8:8 + 2304], stereo, st)
            L += l
            R += r
            if raw[2] & 0x80:
                break
    return L, R, rate


def resample(L, R, rate, g):
    step = (rate << 16) // 48000
    hl = [0, 0, 0] + L
    hr = [0, 0, 0] + R
    pos, outL, outR = 0, [], []
    while 3 + (pos >> 16) < len(hl):
        n = 3 + (pos >> 16)
        i = (pos >> 8) & 0xFF
        for h, o in ((hl, outL), (hr, outR)):
            v = ((g[0xFF - i] * h[n - 3]) >> 15) + ((g[0x1FF - i] * h[n - 2]) >> 15) + \
                ((g[0x100 + i] * h[n - 1]) >> 15) + ((g[i] * h[n]) >> 15)
            o.append(max(-32768, min(32767, v)))
        pos += step
    return outL, outR


def write(path, L, R, rate):
    w = wave.open(path, "wb")
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(b"".join(struct.pack("<hh", a, b) for a, b in zip(L, R)))
    w.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--index", type=int, required=True, help="PS1 sector in HILL. (sh1spu's log)")
    ap.add_argument("--file", type=int, required=True)
    ap.add_argument("--chan", type=int, required=True)
    ap.add_argument("--hill", default=os.path.join(REPO, "rom", "USA", "HILL."))
    ap.add_argument("--out", required=True)
    ap.add_argument("--out48")
    args = ap.parse_args()
    L, R, rate = stream(args.index, args.file, args.chan, args.hill)
    write(args.out, L, R, rate)
    print("%s: %d frames at %d Hz (%.2f s)" % (args.out, len(L), rate, len(L) / rate))
    if args.out48:
        l48, r48 = resample(L, R, rate, gauss())
        write(args.out48, l48, r48, 48000)
        print("%s: %d frames at 48000 Hz" % (args.out48, len(l48)))


if __name__ == "__main__":
    main()
