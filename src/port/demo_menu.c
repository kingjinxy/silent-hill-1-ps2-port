/** @brief The port's Demo menu: a scrolling list of the game's in-game cutscenes (no FMVs) under New
 * Game in the main menu, for testing. The list comes from tools/port/cutscene_list.py, sorted by the
 * events' function names.
 *
 * Starting one (src/bodyprog/events/title.c) begins a new game (normal difficulty) on the
 * cutscene's map, with Harry at the event's trigger point. The first Event_Update then starts that
 * event directly: its required event flag is set and its completion flag cleared, the trigger checks
 * are skipped, and nothing else from earlier progress is set up. Once the player has had control for
 * a second (gameplay, no cutscene flag), the game soft-resets (Game_WarmBoot) back to the main menu,
 * which then opens the list again.
 */

#include "game.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/events/bodyprog_data_800A99B4.h"
#include "bodyprog/map/map.h"
#include "bodyprog/sys/joy.h"
#include "bodyprog/text/text_draw.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/screen/screen_data.h"
#include "port/demo_menu.h"

#define ROWS       9
#define LIST_X     24
#define LIST_Y     44
#define ROW_HEIGHT 18

static int s_Selected;
static int s_Top;
static int s_Chosen = -1;  /* cutscene being started or played */
static int s_EventPending; /* waiting for Event_Update to start it */
static int s_Started;
static int s_ControlFrames;
static int s_ReturnToList;
static s32 s_StartTick, s_StartVBlank; /* g_TickCount and the vertical blank count at the event's start */
static s32 s_ControlVBlanks, s_ControlTick; /* the same when the player had control */

int Port_DemoMenuUpdate(void)
{
    u32 pulsed  = g_Controller0->buttonFlags.pulsed;
    u32 clicked = g_Controller0->buttonFlags.clicked;

    g_SysWork.gameStateStepCounter = 0; /* no attract demo or intro movie while the list is open */

    if (pulsed & ControllerFlag_LStickHighUp)
    {
        s_Selected = (s_Selected + g_PortCutsceneCount - 1) % g_PortCutsceneCount;
        SD_Call(Sfx_MenuMove);
    }
    if (pulsed & ControllerFlag_LStickHighDown)
    {
        s_Selected = (s_Selected + 1) % g_PortCutsceneCount;
        SD_Call(Sfx_MenuMove);
    }
    if (s_Selected < s_Top)
    {
        s_Top = s_Selected;
    }
    if (s_Selected >= s_Top + ROWS)
    {
        s_Top = s_Selected - ROWS + 1;
    }
    if (clicked & g_GameWorkPtr->config.controllerConfig.enter)
    {
        SD_Call(Sfx_MenuStartGame);
        return s_Selected;
    }
    if (clicked & g_GameWorkPtr->config.controllerConfig.cancel)
    {
        SD_Call(Sfx_MenuCancel);
        return -1;
    }
    return -2;
}

void Port_DemoMenuDraw(void)
{
    char        line[64];
    int         i, k;
    const char* s;

    Gfx_StringPositionSet(LIST_X, LIST_Y - 24);
    Gfx_StringColorSet(StringColorId_White);
    Gfx_StringDraw("DEMO", DEFAULT_MAP_MESSAGE_LENGTH);

    for (i = s_Top; i < s_Top + ROWS && i < g_PortCutsceneCount; i++)
    {
        /* The font draws '_' as a space. */
        line[0] = i == s_Selected ? '[' : '_';
        for (k = 1, s = g_PortCutscenes[i].name; *s && k < (int)sizeof(line) - 2; s++)
        {
            line[k++] = *s == '_' ? '-' : *s;
        }
        line[k++] = i == s_Selected ? ']' : '_';
        line[k]   = 0;
        Gfx_StringPositionSet(LIST_X, LIST_Y + (i - s_Top) * ROW_HEIGHT);
        Gfx_StringColorSet(i == s_Selected ? StringColorId_White : StringColorId_LightGrey);
        Gfx_StringDraw(line, DEFAULT_MAP_MESSAGE_LENGTH);
    }
}

int Port_DemoChosen(void)
{
    return s_Chosen;
}

void Port_DemoBegin(int idx)
{
    s_Chosen        = idx;
    s_EventPending  = 1;
    s_Started       = 0;
    s_ControlFrames = 0;
}

int Port_DemoEventStart(void)
{
    s_EventData* ev;

    if (s_Chosen < 0 || !s_EventPending)
    {
        return 0;
    }
    s_EventPending = 0;
    s_Started      = 1;
    ev             = &g_MapOverlayHdr.mapEvents[g_PortCutscenes[s_Chosen].event];
    printf("demo: %s started\n", g_PortCutscenes[s_Chosen].name);
    s_StartTick = g_TickCount;
    s_StartVBlank = VSync(SyncMode_Count);
    if (ev->requiredEventFlag != EventFlag_None)
    {
        Savegame_EventFlagSet(ev->requiredEventFlag);
    }
    if (ev->completeEventFlag != EventFlag_None)
    {
        Savegame_EventFlagClear(ev->completeEventFlag);
    }
    g_MapEventData     = ev;
    g_MapEventSysState = ev->sysState;
    g_MapEventParam    = ev->eventParam;
    return 1;
}

#ifdef SH_PORT_MSG_DEBUG
void Port_Spike(const char* name, unsigned int ms)
{
    printf("spike: %s took %u ms (sysState %d step %d)\n", name, ms, g_SysWork.sysState, g_SysWork.sysStateSteps[0]);
}

/* Test (SH1_MSG_DEBUG=1): once a second, how far the voiced text timer fell against the frame time
 * and the vertical blanks that passed. */
static void msg_debug(void)
{
    extern s32 Port_VBlanks(u32* cycles);
    static s32 prevTimer = -1, frames, timerDrop, rawSum, vbStart = -1;
    u32        cycles;
    s32        vb = Port_VBlanks(&cycles), t = g_SysWork.mapMsgTimer;
    if (vbStart < 0)
    {
        vbStart = vb;
    }
    if (prevTimer > 0 && t >= 0 && t < prevTimer)
    {
        timerDrop += prevTimer - t;
    }
    prevTimer = t;
    {
        static s32 prevStep = -1;
        if (g_SysWork.sysStateSteps[0] != prevStep)
        {
            printf("msgdbg: step %d at vblank %d\n", g_SysWork.sysStateSteps[0], vb);
            prevStep = g_SysWork.sysStateSteps[0];
        }
    }
    rawSum += g_DeltaTimeRaw;
    frames++;
    if (vb - vbStart >= 60)
    {
        printf("msgdbg: %d vblanks, %d frames, frame time %d.%03d s, text timer fell %d.%03d s\n", vb - vbStart, frames,
               rawSum >> 12, ((rawSum & 0xFFF) * 1000) >> 12, timerDrop >> 12, ((timerDrop & 0xFFF) * 1000) >> 12);
        vbStart = vb;
        frames = timerDrop = rawSum = 0;
    }
}
#endif


static volatile int s_Remote = -1; /* cutscene requested over the network (ps2_ctl.py demo) */

/* Demo sweep (host:demo.txt, src/port/ps2/demo_sweep_ps2.c; tools/port/demo_sweep.py): the list
 * played from a given cutscene to the end without input, each given a timeout to reach gameplay. */
static int s_Sweep = -1;    /* next cutscene to request (-1: off) */
static int s_SweepCur = -1; /* the one playing */
static int s_SweepTimeout;  /* vertical blanks */
static int s_SweepAt;       /* vertical blank count when it was requested */
static int s_SweepEnd;      /* one past the last to play */

static void sweep_tick(void)
{
    extern int Port_DemoSweepConfig(int* first, int* timeout, int* last); /* demo_sweep_ps2.c */
    static int checked;
    int        now = VSync(SyncMode_Count);

    if (!checked)
    {
        int first, timeout, last;
        checked = 1;
        if (Port_DemoSweepConfig(&first, &timeout, &last))
        {
            s_SweepEnd     = last + 1 < g_PortCutsceneCount ? last + 1 : g_PortCutsceneCount;
            s_Sweep        = first;
            s_SweepTimeout = timeout * 60;
            printf("demo sweep: from %d of %d, %d s each\n", first, g_PortCutsceneCount, timeout);
        }
    }
    if (s_Sweep < 0)
    {
        return;
    }
    if (s_SweepCur >= 0 && now - s_SweepAt > s_SweepTimeout)
    {
        printf("demo: %s timeout (no gameplay within %d s; sysState %d, steps %d %d %d)\n", g_PortCutscenes[s_SweepCur].name,
               s_SweepTimeout / 60, g_SysWork.sysState, g_SysWork.sysStateSteps[0], g_SysWork.sysStateSteps[1],
               g_SysWork.sysStateSteps[2]);
        s_SweepCur = -1;
    }
    /* The next one once the last is over, from the main menu or gameplay (not during the boot logos
     * or a load), a couple of seconds after the last request. */
    if (s_SweepCur < 0 && s_Remote < 0 && now - s_SweepAt > 120 &&
        (g_GameWork.gameState == GameState_MainMenu || g_GameWork.gameState == GameState_InGame))
    {
        if (s_Sweep >= s_SweepEnd)
        {
            printf("demo sweep: done\n");
            s_Sweep = -1;
            return;
        }
        s_SweepCur = s_Sweep++;
        s_SweepAt  = now;
        Port_DemoRemote(s_SweepCur);
    }
}

/** Demo cutscenes play without input: a wait point (`site`: 0 map messages, 1 a shown image, 2 its
 * "continue") that has been `waiting` for two seconds gets an X press (once, then the wait starts
 * over). Pages with voice lines aren't waits (they go on by themselves), so they aren't cut short. */
int Port_DemoAutoPress(int site, int waiting)
{
    static int since[4] = { -1, -1, -1, -1 };
    int        now;
    if (s_Chosen < 0 || !s_Started || site < 0 || site >= 4)
    {
        return 0;
    }
    now = VSync(SyncMode_Count);
    if (!waiting)
    {
        since[site] = -1;
        return 0;
    }
    if (since[site] < 0 || now - since[site] > 240) /* a new wait (or one left long ago) */
    {
        since[site] = now;
        return 0;
    }
    if (now - since[site] < 120)
    {
        return 0;
    }
    since[site] = -1;
    printf("demo: X pressed for the player (%s)\n", site == 0 ? "message" : site == 1 ? "image" : "continue");
    return 1;
}

void Port_DemoTick(void)
{
#ifdef SH_PORT_BENCH
    {
        /* Benchmark builds (fixed step in Demo cutscenes, game_main.c): a frame dump every 200 frames
         * after the event started, the same frames in every run (tools/port/bench_cutscene.sh). */
        extern void Display_RequestDump(void);
        if (s_Started && s_Chosen >= 0 && g_TickCount != s_StartTick && (g_TickCount - s_StartTick) % 200 == 0 &&
            g_TickCount - s_StartTick <= 2000)
        {
            Display_RequestDump();
        }
    }
#endif
#ifdef SH_PORT_MSG_DEBUG
    msg_debug();
#endif
    sweep_tick();
    if (s_Chosen < 0 || !s_Started)
    {
        return;
    }
    if (g_GameWork.gameState == GameState_InGame && g_SysWork.sysState == SysState_Gameplay &&
        !(g_SysWork.sysFlags & SysFlag_CutsceneActive))
    {
        if (s_ControlFrames++ == 0)
        {
            s_ControlVBlanks = VSync(SyncMode_Count);
            s_ControlTick    = g_TickCount;
        }
    }
    else
    {
        s_ControlFrames = 0;
    }
    if (s_ControlFrames >= 60)
    {
        {
            /* Frames drawn per second from the event's start until the player had control (60 frames
             * ago, when these were taken), as tools/port/gdb/cutscene_ps1.py measures the PS1 game (tools/port/compare_sweeps.py). */
            s32 frames = s_ControlTick - s_StartTick, vbs = s_ControlVBlanks - s_StartVBlank;
            printf("demo: %s fps: %d frames in %d vertical blanks = %d.%02d fps\n", g_PortCutscenes[s_Chosen].name,
                   frames, vbs, frames * 5994 / (vbs > 0 ? vbs : 1) / 100, frames * 5994 / (vbs > 0 ? vbs : 1) % 100);
        }
        printf("demo: %s done (gameplay), back to the list\n", g_PortCutscenes[s_Chosen].name);
        s_SweepCur = -1;
        s_Chosen      = -1;
        s_Started     = 0;
        s_ReturnToList = 1;
        g_SysWork.sysFlags |= SysFlag_DoWarmReset; /* back to the main menu as a soft reset does */
    }
}


/** From the remote control agent's thread: start cutscene `idx` as soon as the main menu runs; from
 * anywhere else in the game, soft-reset back to it first. */
void Port_DemoRemote(int idx)
{
    if (idx < 0 || idx >= g_PortCutsceneCount)
    {
        printf("demo: no cutscene %d (0-%d)\n", idx, g_PortCutsceneCount - 1);
        return;
    }
    if (s_Remote >= 0 || (s_Chosen >= 0 && s_EventPending))
    {
        printf("demo: %s ignored (a cutscene is already being started)\n", g_PortCutscenes[idx].name);
        return;
    }
    printf("demo: %s requested\n", g_PortCutscenes[idx].name);
    s_Remote = idx;
    s_Chosen = -1; /* a cutscene still playing doesn't return to the list first */
    if (g_GameWork.gameState != GameState_MainMenu)
    {
        g_SysWork.sysFlags |= SysFlag_DoWarmReset;
    }
}

/** A New Game (normal difficulty) requested over the network, for hardware tests: as the main menu's
 * New Game. From elsewhere in the game it soft-resets to the main menu first. */
static volatile int s_RemoteNewGame;

void Port_NewGameRemote(void)
{
    printf("newgame: requested\n");
    s_RemoteNewGame = 1;
    s_Remote        = -1;
    s_Chosen        = -1;
    if (g_GameWork.gameState != GameState_MainMenu)
    {
        g_SysWork.sysFlags |= SysFlag_DoWarmReset;
    }
}

int Port_NewGameRemoteTake(void)
{
    int r           = s_RemoteNewGame;
    s_RemoteNewGame = 0;
    return r;
}

int Port_DemoRemotePending(void)
{
    return s_Remote >= 0;
}

/** The main menu: a cutscene requested over the network, or -1. */
int Port_DemoRemoteTake(void)
{
    int r    = s_Remote;
    s_Remote = -1;
    return r;
}

int Port_DemoReturnToList(void)
{
    int r          = s_ReturnToList;
    s_ReturnToList = 0;
    return r;
}
