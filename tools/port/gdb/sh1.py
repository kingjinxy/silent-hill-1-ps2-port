"""gdb helpers for debugging Silent Hill (USA) in DuckStation.

DuckStation: Settings -> Advanced -> enable "GDB Server" (port 2345), boot the game, then:

    gdb-multiarch -x tools/port/gdb/sh1.py -ex "target remote localhost:2345"   (from the repo root)

or use the "DuckStation (attach)" VS Code launch config. Commands:

    warp <map>    Make the next map load (New Game, Load Game, demo) load <map> instead.
                  <map> is an index (0-42) or a name like map4_s02. Also loads that map's symbols.
    warp off      Cancel a pending warp.
    sh-maps       List map indices and names.
"""
import os

import gdb

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT = os.path.join(REPO, "build", "USA", "out")

GAMEBOOT_MAPLOAD = 0x8003521C  # GameBoot_MapLoad(s32 mapIdx)
G_SAVEGAME_PTR = 0x80024D48    # s_Savegame* g_SavegamePtr
SAVEGAME_MAPIDX = 0xA4         # s_Savegame.mapIdx (s8)

MAPS = [
    "map0_s00", "map0_s01", "map0_s02",
    "map1_s00", "map1_s01", "map1_s02", "map1_s03", "map1_s04", "map1_s05", "map1_s06",
    "map2_s00", "map2_s01", "map2_s02", "map2_s03", "map2_s04",
    "map3_s00", "map3_s01", "map3_s02", "map3_s03", "map3_s04", "map3_s05", "map3_s06",
    "map4_s00", "map4_s01", "map4_s02", "map4_s03", "map4_s04", "map4_s05", "map4_s06",
    "map5_s00", "map5_s01", "map5_s02", "map5_s03",
    "map6_s00", "map6_s01", "map6_s02", "map6_s03", "map6_s04", "map6_s05",
    "map7_s00", "map7_s01", "map7_s02", "map7_s03",
]

_map_symfile = None


def load_map_symbols(idx):
    """Map overlays all load at the same address, so only one map's symbols can be loaded at a time."""
    global _map_symfile
    if _map_symfile:
        gdb.execute("remove-symbol-file " + _map_symfile, to_string=True)
        _map_symfile = None
    path = os.path.join(OUT, "VIN", MAPS[idx].upper() + ".BIN.elf")
    if os.path.exists(path):
        gdb.execute("add-symbol-file " + path, to_string=True)
        _map_symfile = path


class MapLoadBreakpoint(gdb.Breakpoint):
    def __init__(self):
        super().__init__("*0x%08X" % GAMEBOOT_MAPLOAD, internal=True)
        self.target = None

    def stop(self):
        loaded = int(gdb.parse_and_eval("$a0")) & 0xFF
        if self.target is not None:
            gdb.execute("set $a0 = %d" % self.target)
            savegame = int(gdb.parse_and_eval("*(unsigned int*)0x%08X" % G_SAVEGAME_PTR))
            gdb.execute("set *(signed char*)0x%08X = %d" % (savegame + SAVEGAME_MAPIDX, self.target))
            print("[sh1] warp: %s -> %s" % (MAPS[loaded], MAPS[self.target]))
            loaded = self.target
            self.target = None
        else:
            print("[sh1] loading %s" % MAPS[loaded])
        load_map_symbols(loaded)
        return False  # Keep running.


class Warp(gdb.Command):
    """warp <map|off>: redirect the next map load. See `sh-maps`."""

    def __init__(self):
        super().__init__("warp", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        arg = arg.strip().lower()
        if arg == "off":
            _bp.target = None
            print("[sh1] warp cancelled")
            return
        idx = int(arg, 0) if arg[:1].isdigit() else MAPS.index(arg)
        _bp.target = idx
        print("[sh1] next map load -> %s (%d). Start a new game or load a save." % (MAPS[idx], idx))


class Maps(gdb.Command):
    """sh-maps: list map indices."""

    def __init__(self):
        super().__init__("sh-maps", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        for i, name in enumerate(MAPS):
            print("%2d  %s" % (i, name))


gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("file " + os.path.join(OUT, "SLUS_007.07.elf"), to_string=True)
gdb.execute("add-symbol-file " + os.path.join(OUT, "1ST", "BODYPROG.BIN.elf"), to_string=True)
_bp = MapLoadBreakpoint()
Warp()
Maps()
print("[sh1] ready: `warp <map>`, `sh-maps`, then `continue`")
