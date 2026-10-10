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

void Port_DemoTick(void)
{
#ifdef SH_PORT_MSG_DEBUG
    msg_debug();
#endif
    if (s_Chosen < 0 || !s_Started)
    {
        return;
    }
    if (g_GameWork.gameState == GameState_InGame && g_SysWork.sysState == SysState_Gameplay &&
        !(g_SysWork.sysFlags & SysFlag_CutsceneActive))
    {
        s_ControlFrames++;
    }
    else
    {
        s_ControlFrames = 0;
    }
    if (s_ControlFrames >= 60)
    {
        printf("demo: %s done (gameplay), back to the list\n", g_PortCutscenes[s_Chosen].name);
        s_Chosen      = -1;
        s_Started     = 0;
        s_ReturnToList = 1;
        g_SysWork.sysFlags |= SysFlag_DoWarmReset; /* back to the main menu as a soft reset does */
    }
}

static volatile int s_Remote = -1; /* cutscene requested over the network (ps2_ctl.py demo) */

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
