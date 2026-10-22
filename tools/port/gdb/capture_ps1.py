"""gdb script (run by tools/port/compare_frames.py) capturing frames of an attract demo (CAP_DEMO: the Nth
played, from 1; only Demo_Start stops the emulator before it) from
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
DEMO = int(os.environ.get("CAP_DEMO", "1"))
# CAP_LOAD=F,D: the save is loaded instead (Load Game, then file F's Data D on card 1 confirmed), and
# frames are vertical blanks of gameplay (g_SysWork.gameStateCounter in GameState_InGame), each
# captured at the first frame reaching it, as the port's capture_ps2.c.
LOAD = [int(x) for x in os.environ["CAP_LOAD"].split(",")] if os.environ.get("CAP_LOAD") else None

STEP = 0x800C4894          # g_Demo_DemoStep
DEMO_ID = 0x800AFDB8       # g_Demo_DemoId
SYS_FLAGS = 0x800BC264     # g_SysWork.sysFlags (offset 0x22A4)
DEMO_ACTIVE = 1 << 1       # SysFlag_DemoActive
DEMO_START = 0x8008F398    # Demo_Start
DRAWOTAG = 0x80018A6C
PUTDRAWENV = 0x80018ADC
PUTDISPENV = 0x80018CA8
STOREIMAGE = 0x80018784
DRAWSYNC = 0x80018478
SYSWORK, SYSWORK_SIZE = 0x800B9FC0, 0x2768  # g_SysWork, for comparing game state
GAMEWORK, GAMEWORK_SIZE = 0x800BC728, 0x5D8  # g_GameWork
DELTA_TIME = 0x800B5CC0                      # g_DeltaTime
ENVWORK, ENVWORK_SIZE = 0x800C4168, 0x150    # g_WorldEnvWork
GAMESTATE = 0x800BC728 + 0x594             # g_GameWork.gameState
GAMESTATE_INGAME = 11
STATE_COUNTER = SYSWORK + 0x1C              # g_SysWork.gameStateCounter
MAIN_MENU_UPDATE = 0x8003AB28               # GameState_MainMenu_Update
MAIN_MENU_STATE, MAIN_MENU_SELECTED = 0x800A9A74, 0x800A9A78
MAIN_MENU_ENTRY_LOAD, MAIN_MENU_ENTRY_CONTINUE = 0, 1
GAMESTATE_STEP1 = 0x800BC728 + 0x59C        # g_GameWork.gameStateSteps[1]
GAMESTATE_LOAD_SCREEN = 8                   # GameState_LoadSavegameScreen
CONTROLLER0_PTR = 0x80024D4C                # s_ControllerData*; buttonFlags.clicked at +0x10
CONFIG_ENTER = 0x800BC728                   # g_GameWork.config.controllerConfig.enter
SLOT1_ENTRIES = 0x801E09E0                  # card 1's s_SaveScreenElement list (16 bytes each)
ELEMENT_COUNT0 = 0x800BCD30                 # g_Savegame_ElementCount0[2]
SLOT_ELEMENT_SELECTED = 0x800A97D4          # g_SlotElementSelectedIdx[2]
SELECTED_SAVE_SLOT = 0x800A97D6             # g_SelectedSaveSlotIdx
SAVEGAME_ENTRY_TYPE_SAVE = 8                # SavegameEntryType_Save
SAVE_SCREEN_LOGIC = 0x801E649C              # SaveScreen_LogicUpdate (saveload overlay)
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


def load_step():
    """Save captures: the listed frame this frame captures, latched at its first stop."""
    global frame_step
    if frame_step is False:
        frame_step = None
        if r32(GAMESTATE) == GAMESTATE_INGAME:
            n = r32(STATE_COUNTER)
            frame_step = next((f for f in FRAMES if f not in done and f <= n and open_frame in (None, f)), None)
    return frame_step


def demo_step():
    """The captured demo's step, or None outside it."""
    if LOAD:
        return load_step()
    if demo_count != DEMO or not (r32(SYS_FLAGS) & DEMO_ACTIVE):
        return None
    return r32(STEP)


demo_count = 0
frame_step = False    # load_step's latch (False: not read this frame)
done = set()
open_frame = None     # frame whose packets are being collected
packets = []
pending_env = None    # DRAWENV* from the last PutDrawEnv, its packet not yet recorded

gdb.execute("set pagination off")
# Until the captured demo starts, only Demo_Start stops the emulator (once a demo); the per-frame
# breakpoints come after.
if LOAD:
    # Load Game, once the title menu has selected Continue (memory card scanned) for 20 frames.
    menu_bp, waited = gdb.Breakpoint("*0x%08X" % MAIN_MENU_UPDATE, internal=True), 0
    while True:
        gdb.execute("continue", to_string=True)
        if r32(MAIN_MENU_STATE) == 1 and r32(MAIN_MENU_SELECTED) == MAIN_MENU_ENTRY_CONTINUE:
            waited += 1
            if waited == 20:
                inf.write_memory(MAIN_MENU_SELECTED, struct.pack("<I", MAIN_MENU_ENTRY_LOAD))
                pad = r32(CONTROLLER0_PTR)
                inf.write_memory(pad + 0x10, struct.pack("<I", r32(pad + 0x10) | (r32(CONFIG_ENTER) & 0xFFFF)))
                print("capture: Load Game pressed", flush=True)
                break
    menu_bp.delete()
    # The Load screen's list: the save selected and Cross, as the player would (saveload.c's port hook).
    list_bp, waited, entry = gdb.Breakpoint("*0x%08X" % SAVE_SCREEN_LOGIC, internal=True), 0, None
    while entry is None:
        gdb.execute("continue", to_string=True)
        count = bytes(inf.read_memory(ELEMENT_COUNT0, 1))[0]
        if r32(GAMESTATE) != GAMESTATE_LOAD_SCREEN or r32(GAMESTATE_STEP1) != 0 or count == 0:
            continue
        waited += 1
        if waited < 30:
            continue
        for i in range(count):
            e = bytes(inf.read_memory(SLOT1_ENTRIES + 16 * i, 16))
            if e[4] == SAVEGAME_ENTRY_TYPE_SAVE and e[6] == LOAD[0] - 1 and e[7] == LOAD[1] - 1:
                entry = i
                break
        if entry is None:
            raise RuntimeError("file %d data %d not on card 1" % tuple(LOAD))
        inf.write_memory(SELECTED_SAVE_SLOT, b"\0")
        inf.write_memory(SLOT_ELEMENT_SELECTED, bytes([entry]))
        pad = r32(CONTROLLER0_PTR)
        inf.write_memory(pad + 0x10, struct.pack("<I", r32(pad + 0x10) | (r32(CONFIG_ENTER) & 0xFFFF)))
        print("capture: loading file %d data %d (entry %d of %d)" % (LOAD[0], LOAD[1], entry, count), flush=True)
    list_bp.delete()
else:
    start_bp = gdb.Breakpoint("*0x%08X" % DEMO_START, internal=True)
    while demo_count < DEMO:
        gdb.execute("continue", to_string=True)
        if reg("pc") == DEMO_START:
            demo_count += 1
            print("capture: demo %d started" % demo_count, flush=True)
    start_bp.delete()
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

    if pc == PUTDISPENV:
        frame_step = False
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
        open(os.path.join(OUT, "ps1_%d_envwork.bin" % open_frame), "wb").write(bytes(inf.read_memory(ENVWORK, ENVWORK_SIZE)))
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
