"""gdb script (run by tools/port/gpu_test.py): draws the synthetic GPU test on the PS1 game in
DuckStation. Once the game reaches its first PutDispEnv, the test VRAM ($GPU_TEST_DIR/gpu_in_vram.bin)
is loaded with the game's LoadImage, the packets (gpu_gp0.bin: u32 word count, then the words) are
linked into an ordering table in RAM and drawn with DrawOTag, and VRAM is read back with StoreImage
into gpu_ps1_vram.bin. The game's RAM used for this is not restored (the emulator is killed after).
"""
import os
import struct

import gdb

DIR = os.environ["GPU_TEST_DIR"]
PUTDISPENV = 0x80018CA8
LOADIMAGE = 0x80018724
STOREIMAGE = 0x80018784
DRAWSYNC = 0x80018478
DRAWOTAG = 0x80018A6C
BUF = 0x80100000  # RECT + 1024 x 64 x 2 bytes, then the packets
PKT = 0x80130000

REGS = ["at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2",
        "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra", "lo", "hi", "pc"]
inf = gdb.selected_inferior()


def reg(name):
    return int(gdb.parse_and_eval("$" + name)) & 0xFFFFFFFF


def call(fn, args):
    saved = {r: reg(r) for r in REGS}
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


gdb.execute("set pagination off")
gdb.Breakpoint("*0x%08X" % PUTDISPENV, internal=True)
for _ in range(30):  # a few frames in: the GPU set up and idle between frames
    gdb.execute("continue", to_string=True)
call(DRAWSYNC, [0])

vram = open(os.path.join(DIR, "gpu_in_vram.bin"), "rb").read()
for chunk in range(8):
    inf.write_memory(BUF, struct.pack("<hhhh", 0, chunk * 64, 1024, 64))
    inf.write_memory(BUF + 16, vram[chunk * 64 * 2048:(chunk + 1) * 64 * 2048])
    call(LOADIMAGE, [BUF, BUF + 16])
    call(DRAWSYNC, [0])
print("gpu test: VRAM loaded", flush=True)

data, pos, nodes = open(os.path.join(DIR, "gpu_gp0.bin"), "rb").read(), 0, []
while pos + 4 <= len(data):
    (n,) = struct.unpack_from("<I", data, pos)
    nodes.append(data[pos + 4:pos + 4 + 4 * n])
    pos += 4 + 4 * n
addr, blob = PKT, b""
for i, words in enumerate(nodes):
    nxt = (addr + 4 + len(words)) & 0xFFFFFF if i + 1 < len(nodes) else 0xFFFFFF
    blob += struct.pack("<I", (len(words) // 4) << 24 | nxt) + words
    addr += 4 + len(words)
inf.write_memory(PKT, blob)
call(DRAWOTAG, [PKT])
call(DRAWSYNC, [0])
print("gpu test: %d packets drawn" % len(nodes), flush=True)

out = bytearray()
for chunk in range(8):
    inf.write_memory(BUF, struct.pack("<hhhh", 0, chunk * 64, 1024, 64))
    call(STOREIMAGE, [BUF, BUF + 16])
    call(DRAWSYNC, [0])
    out += bytes(inf.read_memory(BUF + 16, 1024 * 64 * 2))
open(os.path.join(DIR, "gpu_ps1_vram.bin"), "wb").write(out)
print("gpu test: VRAM read back", flush=True)
gdb.execute("kill")
