"""gdb (DuckStation) side of tools/port/duckstation_cutscene.py: plays one in-game cutscene on the
unmodified PS1 game, as the port's Demo menu does (src/port/demo_menu.c), and logs its timing.

Loaded after sh1.py (which provides `warp`). Command:

    sh-cutscene <map> <event> [seconds] [function-address-hex [until-step]]

A New Game is started from the title menu (Cross pressed for START and for the difficulty, as the port's
Demo menu starts a new game; a demo's world state lacks the map's characters), the opening movie is cut
short, and the new game's map load loads <map> instead. At the first Event_Update there, the event's
required flag is set, its completion flag cleared, and event <event> of the map's MAP_EVENTS starts
without its trigger. Logged, with the frame
(g_SysWork.gameStateCounter, vertical blanks in the current game state):
- "page"      a voiced text page starts: its ~J time, the cutscene step and the message
- "XA start"  a voice line starts: its XA index, the page timer (~J time left) and the cutscene step
- "XA stop"   the sound code stops the line (fade, then CdlPause)
The run stops when the player has control again (Event_Update runs only then). gdb halts the game
only in the title menu, for the movie skip, and in the XA routines around each voice line.
"""
import gdb

MAPS = [
    "map0_s00", "map0_s01", "map0_s02",
    "map1_s00", "map1_s01", "map1_s02", "map1_s03", "map1_s04", "map1_s05", "map1_s06",
    "map2_s00", "map2_s01", "map2_s02", "map2_s03", "map2_s04",
    "map3_s00", "map3_s01", "map3_s02", "map3_s03", "map3_s04", "map3_s05", "map3_s06",
    "map4_s00", "map4_s01", "map4_s02", "map4_s03", "map4_s04", "map4_s05", "map4_s06",
    "map5_s00", "map5_s01", "map5_s02", "map5_s03",
    "map6_s00", "map6_s01", "map6_s02", "map6_s03", "map6_s04", "map6_s05",
    "map7_s00", "map7_s01", "map7_s02", "map7_s03",
]  # as sh1.py (loaded by gdb as a script, not importable)

G_SYSWORK = 0x800B9FC0
SYS_STATE, SYS_STEP0, SYS_GAMESTATE_COUNTER, SYS_PROCESS_FLAGS, SYS_FLAGS, SYS_MSG_TIMER = 0x8, 0xC, 0x1C, 0x2298, 0x22A4, 0x234C
G_GAMEWORK_GAMESTATE = 0x800BC728 + 0x594
G_SAVEGAME_PTR = 0x80024D48
SAVEGAME_EVENT_FLAGS = 0x168
G_MAP_OVERLAY_HDR = 0x800C957C
HDR_MAP_EVENTS = 0x24
EVENT_DATA_SIZE = 12
G_MAP_EVENT_DATA = 0x800BCDD8
G_MAP_EVENT_SYSSTATE = 0x800A9A10
G_MAP_EVENT_PARAM = 0x800A9A14
G_SD_AUDIOWORK = 0x800C1658  # xaAudioIdxCheck at +2
G_SD_STREAMING_STATES = 0x800C1670  # xaLoadState at +1 (s_Sd_AudioStreamingStates: audio, xa, xaStop, xaPreLoad)
SD_XA_AUDIO_PLAY = 0x80046E00
SD_XA_AUDIO_STOP = 0x80047634
XA_LOAD_STATE_ENABLE = 7
EVENT_UPDATE = 0x800373CC
CD_CONTROL = 0x80014168
MAINLOOP_STATE_CALL = 0x80033088
GAMESTATE_INGAME = 11
GAMESTATE_MOVIE_OPENING = 9
G_MAIN_MENU_STATE = 0x800A9A74
G_MAIN_MENU_SELECTED = 0x800A9A78
MAIN_MENU_STATE_MAIN, MAIN_MENU_STATE_DIFFICULTY = 1, 3
MAIN_MENU_ENTRY_START = 2
G_CONTROLLER0_PTR = 0x80024D4C   # s_ControllerData*; buttonFlags.clicked at +0x10
G_GAMEWORK_ENTER = 0x800BC728    # g_GameWork.config.controllerConfig.enter
GAMESTATE_MAIN_MENU_UPDATE = 0x8003AB28
STREAM_MAX_FRAME = 0x801E3F40    # STREAM.BIN max_frame: the movie stops once frame_cnt reaches it
G_SAVEGAME_MAPIDX = 0xA4
G_MAP_MSG_CURRENT_IDX = 0x800A99AC  # g_MapMsg_CurrentIdx
SYSFLAG_DEMO_ACTIVE = 1 << 1


def rd(addr, size=4, signed=False):
    t = {1: "char", 2: "short", 4: "int"}[size]
    v = int(gdb.parse_and_eval("*(%s %s*)0x%08X" % ("signed" if signed else "unsigned", t, addr)))
    return v


def wr(addr, value, size=4):
    t = {1: "char", 2: "short", 4: "int"}[size]
    gdb.execute("set *(unsigned %s*)0x%08X = %d" % (t, addr, value & ((1 << (size * 8)) - 1)))


class State:
    event = None
    map_idx = None
    xa_states = False
    pending = False
    started = False
    control = 0
    last_step = None
    last_timer = 0
    frame0 = None
    limit = None


def frame():
    return rd(G_SYSWORK + SYS_GAMESTATE_COUNTER)


def log(text):
    print("[cut] frame %6d  %s" % (frame(), text), flush=True)


class EventStart(gdb.Breakpoint):
    def __init__(self):
        super().__init__("*0x%08X" % EVENT_UPDATE, internal=True)

    def stop(self):
        if State.started:
            # Event_Update only runs in gameplay: the player has control again.
            log("cutscene over (player in control)")
            return True
        if not State.pending:
            return False
        events = rd(G_MAP_OVERLAY_HDR + HDR_MAP_EVENTS)
        if not events or rd(rd(G_SAVEGAME_PTR) + G_SAVEGAME_MAPIDX, 1) != State.map_idx:
            return False
        ev = events + State.event * EVENT_DATA_SIZE
        required, complete = rd(ev, 2, True), rd(ev + 2, 2, True)
        bits = rd(ev + 8)
        sys_state, param = bits & 0x1F, (bits >> 5) & 0xFF
        save = rd(G_SAVEGAME_PTR)
        for flag, on in ((required, True), (complete, False)):
            if flag > 0:
                a = save + SAVEGAME_EVENT_FLAGS + (flag >> 5) * 4
                v = rd(a)
                wr(a, v | (1 << (flag & 31)) if on else v & ~(1 << (flag & 31)))
        wr(G_SYSWORK + SYS_FLAGS, rd(G_SYSWORK + SYS_FLAGS) & ~SYSFLAG_DEMO_ACTIVE)
        wr(G_MAP_EVENT_DATA, ev)
        wr(G_MAP_EVENT_SYSSTATE, sys_state)
        wr(G_MAP_EVENT_PARAM, param)
        gdb.execute("set $pc = $ra")  # return from Event_Update without its trigger checks
        State.pending, State.started = False, True
        State.frame0 = frame()
        log("event %d started (sysState %d, param %d, required flag %d, completion flag %d)" % (
            State.event, sys_state, param, required, complete))
        return False


class XaPlayWatch(gdb.Breakpoint):
    """Sd_XaAudioPlay runs for a few frames per voice line: logs when it starts the line."""

    def __init__(self):
        super().__init__("*0x%08X" % SD_XA_AUDIO_PLAY, internal=True)

    last_state = None

    def stop(self):
        st = rd(G_SD_STREAMING_STATES + 1, 1)
        if State.started and State.xa_states and st != self.last_state:
            log("XA load state %d" % st)
            self.last_state = st
        if State.started and st == XA_LOAD_STATE_ENABLE:
            log("XA start: index %d (page timer %d ms, step %d)" % (
                rd(G_SD_AUDIOWORK + 2, 2), rd(G_SYSWORK + SYS_MSG_TIMER, 4, True) * 1000 >> 12, rd(G_SYSWORK + SYS_STEP0)))
        return False


class XaStopWatch(gdb.Breakpoint):
    """Sd_XaAudioStop runs for a few frames per stop: logs the first."""

    def __init__(self):
        super().__init__("*0x%08X" % SD_XA_AUDIO_STOP, internal=True)

    def stop(self):
        if State.started and rd(G_SD_STREAMING_STATES + 2, 1) == 0:  # XaStopState_FadeOut: the first frame
            log("XA stop (page timer %d ms, step %d)" % (
                rd(G_SYSWORK + SYS_MSG_TIMER, 4, True) * 1000 >> 12, rd(G_SYSWORK + SYS_STEP0)))
        return False


class StepWatch(gdb.Breakpoint):
    """The event function, per frame, only for the cutscene's opening steps (then removed)."""

    def __init__(self, addr, until):
        super().__init__("*0x%08X" % addr, internal=True)
        self.until = until

    def stop(self):
        if State.started:
            step = rd(G_SYSWORK + SYS_STEP0)
            if step != State.last_step:
                log("step %d" % step)
                State.last_step = step
            if step >= self.until:
                self.enabled = False
        return False


class PageWatch(gdb.Breakpoint):
    """The store of a ~J time into g_SysWork.mapMsgTimer in Gfx_MapMsg_StringDraw: once per voiced page."""

    def __init__(self):
        super().__init__("*0x8004B2CC", internal=True)

    def stop(self):
        if State.started:
            log("page %d ms (step %d, message %d)" % (int(gdb.parse_and_eval("$a0")) * 1000 >> 12, rd(G_SYSWORK + SYS_STEP0),
                                                     rd(G_MAP_MSG_CURRENT_IDX)))
        return False


class MenuPress(gdb.Breakpoint):
    """Presses Cross in the title menu: on START, then on the difficulty."""

    def __init__(self):
        super().__init__("*0x%08X" % GAMESTATE_MAIN_MENU_UPDATE, internal=True)
        self.n = 0

    def stop(self):
        if not State.pending:
            return False
        state = rd(G_MAIN_MENU_STATE)
        self.n += 1
        if state in (MAIN_MENU_STATE_MAIN, MAIN_MENU_STATE_DIFFICULTY) and self.n % 20 == 0:
            if state == MAIN_MENU_STATE_MAIN:
                wr(G_MAIN_MENU_SELECTED, MAIN_MENU_ENTRY_START)
            pad = rd(G_CONTROLLER0_PTR)
            wr(pad + 0x10, rd(pad + 0x10) | rd(G_GAMEWORK_ENTER, 2))
            print("[cut] menu: Cross (%s)" % ("START" if state == MAIN_MENU_STATE_MAIN else "difficulty"), flush=True)
            if state == MAIN_MENU_STATE_DIFFICULTY:
                self.enabled = False  # done: no more stops in the menu
        return False


class MovieSkip(gdb.Breakpoint):
    """Cuts the opening movie short (the new game plays it before loading the map)."""

    def __init__(self):
        super().__init__("*0x%08X" % MAINLOOP_STATE_CALL, internal=True)
        self.done = False

    def stop(self):
        if rd(G_GAMEWORK_GAMESTATE) == GAMESTATE_INGAME:
            self.enabled = False  # in the game without the movie: nothing to skip
        elif rd(G_GAMEWORK_GAMESTATE) == GAMESTATE_MOVIE_OPENING:
            wr(STREAM_MAX_FRAME, 1)
            self.enabled = False  # done: no more stops every frame
            print("[cut] opening movie cut short", flush=True)
        return False


class Cutscene(gdb.Command):
    """sh-cutscene <map> <event> [seconds]: play a map event as the port's Demo menu does."""

    def __init__(self):
        super().__init__("sh-cutscene", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        a = arg.split()
        gdb.execute("warp %s" % a[0])
        State.event = int(a[1])
        State.map_idx = int(a[0]) if a[0].isdigit() else MAPS.index(a[0])
        State.limit = float(a[2]) if len(a) > 2 else None
        State.pending = True
        EventStart()
        MenuPress()
        MovieSkip()
        State.xa_states = bool(int(__import__("os").environ.get("SH1_XA_STATES", "0")))
        if len(a) > 3:
            StepWatch(int(a[3], 16), int(a[4]) if len(a) > 4 else 12)
        PageWatch()
        XaPlayWatch()
        XaStopWatch()
        print("[cut] %s event %d armed" % (a[0], State.event))


Cutscene()
