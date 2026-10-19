"""gdb script (run by tools/port/duckstation_movie.py) capturing decoded movie frames from the PS1 game in
DuckStation, the reference for the port's movie decoder (src/port/ps2/strdec.c).

The movie loop (stream overlay movie_main) calls PutDispEnv at the top of each iteration to show the
buffer the MDEC finished last; by then strNextVlc has already fetched the next frame, so the frame on
screen is frame_cnt - 1. For each wanted frame, the whole VRAM is read (the game's StoreImage and
DrawSync called from gdb, as capture_ps1.py does) into CAP_OUT/ps1movie_<frame>_vram.bin with the
DISPENV in ps1movie_<frame>_disp.txt; the movie's start sector (movie_main's third argument) goes to
ps1movie_sector.txt.
"""
import os
import struct

import gdb

FRAMES = sorted(int(x) for x in os.environ["CAP_FRAMES"].split(","))
OUT = os.environ["CAP_OUT"]

PUTDISPENV = 0x80018CA8
STOREIMAGE = 0x80018784
DRAWSYNC = 0x80018478
MOVIE_MAIN = 0x801E2B9C
FRAME_CNT = 0x801E3F44
STREAM_TEXT = (0x801E2654, 0x801E3F3C)  # the stream overlay's code: PutDispEnv called from the movie loop
SCRATCH = 0x80100000                    # 16-byte RECT + 1024x64x2 bytes, restored after each chunk

REGS = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra",
        "lo", "hi", "pc"]

inf = gdb.selected_inferior()


def r32(addr):
    return struct.unpack("<I", bytes(inf.read_memory(addr, 4)))[0]


def reg(name):
    return int(gdb.parse_and_eval("$" + name)) & 0xFFFFFFFF


def call(fn, args):
    """Calls a PS1 function at a breakpoint: returns to the current PC (a breakpoint address)."""
    saved = {r: reg(r) for r in REGS if r != "zero"}
    here = saved["pc"]
    gdb.execute("set $sp = %d" % ((saved["sp"] - 64) & 0xFFFFFFFF))
    for i, a in enumerate(args):
        gdb.execute("set $a%d = %d" % (i, a))
    gdb.execute("set $ra = %d" % here)
    gdb.execute("set $pc = %d" % fn)
    gdb.execute("continue", to_string=True)
    if reg("pc") != here:
        raise RuntimeError("call to %08X stopped at %08X" % (fn, reg("pc")))
    for r, v in saved.items():
        gdb.execute("set $%s = %d" % (r, v))


def read_vram():
    data = bytearray()
    for chunk in range(8):
        size = 16 + 1024 * 64 * 2
        original = bytes(inf.read_memory(SCRATCH, size))
        inf.write_memory(SCRATCH, struct.pack("<hhhh", 0, chunk * 64, 1024, 64))
        call(STOREIMAGE, [SCRATCH, SCRATCH + 16])
        call(DRAWSYNC, [0])
        data += bytes(inf.read_memory(SCRATCH + 16, 1024 * 64 * 2))
        inf.write_memory(SCRATCH, original)
    return bytes(data)


gdb.execute("set pagination off")
for addr in (PUTDISPENV, MOVIE_MAIN):
    gdb.Breakpoint("*0x%08X" % addr, internal=True)

done = set()
while len(done) < len(FRAMES):
    gdb.execute("continue", to_string=True)
    pc = reg("pc")
    if pc == MOVIE_MAIN:
        sector = reg("a2")
        open(os.path.join(OUT, "ps1movie_sector.txt"), "w").write("%d\n" % sector)
        print("capture: movie at sector %d" % sector, flush=True)
        continue
    if not (STREAM_TEXT[0] <= reg("ra") < STREAM_TEXT[1]):
        continue
    frame = r32(FRAME_CNT) - 1
    if frame not in FRAMES or frame in done:
        continue
    disp = struct.unpack("<hhhhhhhh", bytes(inf.read_memory(reg("a0"), 16)))
    open(os.path.join(OUT, "ps1movie_%d_vram.bin" % frame), "wb").write(read_vram())
    open(os.path.join(OUT, "ps1movie_%d_disp.txt" % frame), "w").write(" ".join(str(v) for v in disp) + "\n")
    print("capture: frame %d, display %s" % (frame, disp[:4]), flush=True)
    done.add(frame)
