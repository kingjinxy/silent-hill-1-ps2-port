"""gdb script (DuckStation's GDB server, PS1 game): at every map load, the world streaming state
when the load starts waiting for the world (GameState_MainLoadScreen step 8, WorldGfx_ChunkInitCheck)
and when that check first passes. Each time, PS1 RAM goes to $WATCH_OUT/ps1_load_<n>_<begin|done>.bin
and tools/port/world_diag.py's diagnosis is printed. Run by tools/port/duckstation_world_watch.py.
"""
import os
import struct
import sys

import gdb

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import world_diag  # noqa: E402

CHUNK_INIT_CHECK = 0x8003C850
GAME_STATE = 0x800BC728 + 0x594  # g_GameWork.gameState, then gameStateSteps[0]
MAIN_LOAD_SCREEN = 10
OUT = os.environ.get("WATCH_OUT", ".")

inf = gdb.selected_inferior()
state = {"load": 0, "begun": False, "done": False, "ret_bp": None}


def r32(a):
    return struct.unpack("<I", bytes(inf.read_memory(a, 4)))[0]


def dump(kind):
    ram = bytes(inf.read_memory(0x80000000, 0x200000))
    path = os.path.join(OUT, "ps1_load_%d_%s.bin" % (state["load"], kind))
    open(path, "wb").write(ram)
    img = world_diag.Image(ram, 0x1FFFFF, world_diag.PS1_SYMS,
                           lambda a: (a >> 24) in (0x00, 0x80) and (a & 0xFFFFFF) < 0x200000 and a & 0x1FFFFF)
    print("world watch: load %d, %s (%s)\n%s" % (state["load"], kind, path, world_diag.diagnose(img)), flush=True)


class ReturnBp(gdb.Breakpoint):
    def stop(self):
        if not state["done"] and int(gdb.parse_and_eval("$v0")) & 0xFFFFFFFF:
            state["done"] = True
            dump("done")
        return False


class EntryBp(gdb.Breakpoint):
    def stop(self):
        gs, step = r32(GAME_STATE), r32(GAME_STATE + 4)
        if gs != MAIN_LOAD_SCREEN or step != 8:
            if state["begun"] and gs != MAIN_LOAD_SCREEN:
                state["begun"] = state["done"] = False
            return False
        if not state["begun"]:
            state["begun"], state["done"] = True, False
            state["load"] += 1
            dump("begin")
        if not state["done"]:
            ra = int(gdb.parse_and_eval("$ra")) & 0xFFFFFFFF
            if state["ret_bp"] is None or state["ret_bp"].location != "*0x%x" % ra:
                if state["ret_bp"] is not None:
                    state["ret_bp"].delete()
                state["ret_bp"] = ReturnBp("*0x%x" % ra, internal=True)
        return False


gdb.execute("set pagination off")
gdb.execute("set confirm off")
EntryBp("*0x%x" % CHUNK_INIT_CHECK, internal=True)
print("world watch: ready (play; every map load is logged)", flush=True)
gdb.execute("continue")
