"""gdb helpers for debugging Silent Hill (USA) in DuckStation.

DuckStation: Settings -> Advanced -> enable "GDB Server" (port 2345), boot the game, then:

    gdb-multiarch -x tools/port/gdb/sh1.py -ex "target remote localhost:2345"   (from the repo root)

or use the "DuckStation (attach)" VS Code launch config. Commands:

    warp <map>    Make the next map load (New Game, Load Game, demo) load <map> instead.
                  <map> is an index (0-42) or a name like map4_s02. Also loads that map's symbols.
    warp off      Cancel a pending warp.
    sh-maps       List map indices and names.
    sh-skip-intro [on|off]
                  Skip the title-screen intro movie.
"""
import os

import gdb

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT = os.path.join(REPO, "build", "USA", "out")

GAMEBOOT_MAPLOAD = 0x8003521C  # GameBoot_MapLoad(s32 mapIdx)
G_SAVEGAME_PTR = 0x80024D48    # s_Savegame* g_SavegamePtr
SAVEGAME_MAPIDX = 0xA4         # s_Savegame.mapIdx (s8)

# Intro movie skip. STREAM.BIN (which plays it) isn't loaded when gdb attaches, and a breakpoint in it
# would be overwritten when it loads. So break in bodyprog's MainLoop where it calls the current
# game state's update function; when that's GameState_MovieIntro, STREAM.BIN is in memory and the
# `jal open_main` in GameState_MovieIntro_Update can be patched to a nop.
MAINLOOP_STATE_CALL = 0x80033088            # MainLoop: g_GameStateUpdateFuncs[g_GameWork.gameState]()
G_GAMEWORK_GAMESTATE = 0x800BC728 + 0x594   # g_GameWork.gameState
GAMESTATE_MOVIE_INTRO = 6
MOVIE_INTRO_JAL = 0x801E27C4                # jal open_main
MOVIE_INTRO_JAL_INSN = 0x0C078AA9
GAMEFS_STREAMBIN_LOAD = 0x80032C40          # GameFs_StreamBinLoad(): queues STREAM.BIN
STREAM_MAX_FRAME = 0x801E3F40               # max_frame: movie_main() stops once frame_cnt reaches it

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
        self.stop_after_warp = False  # Halt at the first map load after a warp (used by warp_sweep.py).

    def stop(self):
        loaded = int(gdb.parse_and_eval("$a0")) & 0xFF
        if self.target is not None:
            gdb.execute("set $a0 = %d" % self.target)
            savegame = int(gdb.parse_and_eval("*(unsigned int*)0x%08X" % G_SAVEGAME_PTR))
            gdb.execute("set *(signed char*)0x%08X = %d" % (savegame + SAVEGAME_MAPIDX, self.target))
            print("[sh1] warp: %s -> %s" % (MAPS[loaded], MAPS[self.target]))
            loaded = self.target
            self.target = None
            load_map_symbols(loaded)
            return False
        print("[sh1] loading %s" % MAPS[loaded])
        load_map_symbols(loaded)
        if self.stop_after_warp:
            self.stop_after_warp = False
            print("[sh1] next map load reached")
            return True
        return False  # Keep running.


class SkipIntroBreakpoint(gdb.Breakpoint):
    """Patch out the intro movie (title FMV) just before GameState_MovieIntro_Update runs.

    Hit every frame while enabled (and each hit pauses the emulator briefly), so it's only enabled
    from the moment STREAM.BIN is queued until the intro state runs.
    """

    def __init__(self):
        super().__init__("*0x%08X" % MAINLOOP_STATE_CALL, internal=True)
        self.enabled = False

    def stop(self):
        state = int(gdb.parse_and_eval("*(int*)0x%08X" % G_GAMEWORK_GAMESTATE))
        if state == GAMESTATE_MOVIE_INTRO:
            insn = int(gdb.parse_and_eval("*(unsigned int*)0x%08X" % MOVIE_INTRO_JAL)) & 0xFFFFFFFF
            if insn == MOVIE_INTRO_JAL_INSN:
                gdb.execute("set *(unsigned int*)0x%08X = 0" % MOVIE_INTRO_JAL)
                print("[sh1] skipped intro movie")
        if state >= GAMESTATE_MOVIE_INTRO:
            self.enabled = False  # The target is halted here, so this applies before it resumes.
        return False


class StreamBinLoadBreakpoint(gdb.Breakpoint):
    """Arms SkipIntroBreakpoint when STREAM.BIN (title screens and movies) is queued."""

    def __init__(self):
        super().__init__("*0x%08X" % GAMEFS_STREAMBIN_LOAD, internal=True)
        self.enabled = False

    def stop(self):
        print("[sh1] STREAM.BIN queued")
        _skip.enabled = True
        return False


class SkipIntro(gdb.Command):
    """sh-skip-intro [on|off]: skip the title-screen intro movie."""

    def __init__(self):
        super().__init__("sh-skip-intro", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        on = arg.strip().lower() != "off"
        _stream_load.enabled = on
        state = int(gdb.parse_and_eval("*(int*)0x%08X" % G_GAMEWORK_GAMESTATE))
        # Attached during the logos: STREAM.BIN may already be queued, so arm the skip right away.
        _skip.enabled = on and state < GAMESTATE_MOVIE_INTRO
        if on and state == GAMESTATE_MOVIE_INTRO:
            # Attached while the intro is starting or playing: patch the call if it hasn't happened
            # yet, and end a movie that's already running.
            insn = int(gdb.parse_and_eval("*(unsigned int*)0x%08X" % MOVIE_INTRO_JAL)) & 0xFFFFFFFF
            if insn == MOVIE_INTRO_JAL_INSN:
                gdb.execute("set *(unsigned int*)0x%08X = 0" % MOVIE_INTRO_JAL)
            gdb.execute("set *(int*)0x%08X = 0" % STREAM_MAX_FRAME)
            print("[sh1] skipped intro movie")
        print("[sh1] intro movie skip %s" % ("on" if on else "off"))


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
        _bp.stop_after_warp = os.environ.get("SH1_SWEEP") == "1"
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
_skip = SkipIntroBreakpoint()
_stream_load = StreamBinLoadBreakpoint()
SkipIntro()
Warp()
Maps()
print("[sh1] ready: `warp <map>`, `sh-maps`, then `continue`")
