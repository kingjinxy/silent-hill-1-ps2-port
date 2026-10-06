"""gdb script (run by tools/port/compare_frames.py) capturing frames of the first attract demo from
the PS1 game in DuckStation: for each frame in CAP_FRAMES (g_Demo_DemoStep values), writes to CAP_OUT
ps1_<N>_gp0.bin (every GP0 packet drawn: u32 word count, then the words), ps1_<N>_vram.bin (the
1024x512 VRAM) and ps1_<N>_disp.txt (display x y w h), the same files as the port's
src/port/ps2/capture_ps2.c.

GP0 packets come from the ordering tables (walked at DrawOTag) and the drawing environment packets
(PutDrawEnv's DRAWENV.dr_env, read once it's built). VRAM is read by calling the game's own StoreImage
+ DrawSync from gdb, in 64-line chunks through a scratch RAM area whose bytes are restored after.
"""
import os
import struct

import gdb

FRAMES = sorted(int(x) for x in os.environ["CAP_FRAMES"].split(","))
OUT = os.environ["CAP_OUT"]

STEP = 0x800C4894          # g_Demo_DemoStep
DEMO_ID = 0x800AFDB8       # g_Demo_DemoId
SYS_FLAGS = 0x800BC264     # g_SysWork.sysFlags (offset 0x22A4)
DEMO_ACTIVE = 1 << 1       # SysFlag_DemoActive
DRAWOTAG = 0x80018A6C
PUTDRAWENV = 0x80018ADC
PUTDISPENV = 0x80018CA8
STOREIMAGE = 0x80018784
DRAWSYNC = 0x80018478
SYSWORK, SYSWORK_SIZE = 0x800B9FC0, 0x2768  # g_SysWork, for comparing game state
GAMEWORK, GAMEWORK_SIZE = 0x800BC728, 0x5D8  # g_GameWork
DELTA_TIME = 0x800B5CC0                      # g_DeltaTime
SCRATCH = 0x80100000       # 16-byte RECT + 1024x64x2 bytes, restored after each chunk

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


def demo_step():
    """The first demo's step, or None outside it."""
    global first_demo
    if not (r32(SYS_FLAGS) & DEMO_ACTIVE):
        return None
    demo = r32(DEMO_ID)
    if first_demo is None:
        first_demo = demo
    return r32(STEP) if demo == first_demo else None


first_demo = None
done = set()
open_frame = None     # frame whose packets are being collected
packets = []
pending_env = None    # DRAWENV* from the last PutDrawEnv, its packet not yet recorded

gdb.execute("set pagination off")
for addr in (DRAWOTAG, PUTDRAWENV, PUTDISPENV):
    gdb.Breakpoint("*0x%08X" % addr, internal=True)

while len(done) < len(FRAMES):
    gdb.execute("continue", to_string=True)
    pc = reg("pc")
    step = demo_step()

    if open_frame is not None and pending_env is not None and pc in (DRAWOTAG, PUTDRAWENV, PUTDISPENV):
        # The DRAWENV packet built by the previous PutDrawEnv (tag at +1Ch, words at +20h).
        n = r32(pending_env + 0x1C) >> 24
        packets.append([r32(pending_env + 0x20 + 4 * i) for i in range(n)])
        pending_env = None

    if pc == PUTDISPENV and open_frame is not None:
        disp = struct.unpack("<hhhh", bytes(inf.read_memory(reg("a0"), 8)))
        vram = read_vram()
        with open(os.path.join(OUT, "ps1_%d_gp0.bin" % open_frame), "wb") as f:
            for words in packets:
                f.write(struct.pack("<I", len(words)))
                f.write(struct.pack("<%dI" % len(words), *words))
        open(os.path.join(OUT, "ps1_%d_vram.bin" % open_frame), "wb").write(vram)
        open(os.path.join(OUT, "ps1_%d_disp.txt" % open_frame), "w").write("%d %d %d %d\n%d\n" % (disp + (struct.unpack("<i", bytes(inf.read_memory(DELTA_TIME, 4)))[0],)))
        open(os.path.join(OUT, "ps1_%d_gamework.bin" % open_frame), "wb").write(bytes(inf.read_memory(GAMEWORK, GAMEWORK_SIZE)))
        open(os.path.join(OUT, "ps1_%d_syswork.bin" % open_frame), "wb").write(bytes(inf.read_memory(SYSWORK, SYSWORK_SIZE)))
        print("capture: frame %d (%d packets)" % (open_frame, len(packets)), flush=True)
        done.add(open_frame)
        open_frame, packets = None, []
        continue

    if step is None or step not in FRAMES or step in done:
        continue
    if open_frame is None:
        open_frame, packets, pending_env = step, [], None

    if pc == PUTDRAWENV:
        pending_env = reg("a0")
    elif pc == DRAWOTAG:
        addr, count = reg("a0") & 0xFFFFFF, 0
        while addr != 0xFFFFFF and count < 100000:
            hdr = r32(0x80000000 | addr)
            n = hdr >> 24
            if n:
                packets.append(list(struct.unpack("<%dI" % n, bytes(inf.read_memory(0x80000000 | (addr + 4), 4 * n)))))
            addr = hdr & 0xFFFFFF
            count += 1

print("capture: all frames done", flush=True)
gdb.execute("kill")
