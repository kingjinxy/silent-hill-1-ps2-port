/** @brief SH1's side of the HAL's game hooks (psx-to-ps2-HAL include/port/hal_game.h): frame capture,
 * the per-second state line and load watchdog, the screen-read context and the Debug Options display
 * mode, all read from the game's state. Moved out of libgpu_port.c when the HAL was split off. */

#include "common.h"
#include "game.h"

#include <libgpu.h>
#include <libgs.h>

#include "port/hal_game.h"
#include "bodyprog/demo.h"
#include "main/fsqueue.h"
#include "bodyprog/screen/screen_data.h"
#include "port/widescreen.h"

extern int printf(const char* fmt, ...);

extern void Capture_Packet(int step, const unsigned int* words, int count); /* src/port/ps2/capture_ps2.c */
extern void Capture_Display(int x, int y, int w, int h, const void* state, int stateSize,
                            const void* state2, int state2Size, int deltaTime);
extern int  Capture_Pending(void);
extern int  Capture_Demo(void);
extern int  Capture_Mode(void);
extern int  Capture_Load(int* file, int* data);
extern int  Capture_LoadStep(int counter);
extern void Capture_State(int step, unsigned int seed, int deltaTime, const void* state, int stateSize, const void* state2, int state2Size);
extern u32  g_RngSeed; /* main/rng.c */
extern void Capture_Replay(int frame);

/** Save captures: the frame's listed frame, latched at its first packet (-2: not yet). */
static s32 s_CaptureFrameStep = -2;

/** Demos started so far (counted as the demo flag turns on), and whether one is running. */
static s32 s_CaptureDemoCount;
static s32 s_CaptureDemoOn;

/** The demo playing (1: the first), as counted for captures. */
int Capture_DemoCurrent(void)
{
    return s_CaptureDemoCount;
}

void HalGame_OnPacket(const u32* words, s32 count)
{
    if (Capture_Load(NULL, NULL))
    {
        if (s_CaptureFrameStep == -2)
        {
            s_CaptureFrameStep = g_GameWork.gameState == GameState_InGame ? Capture_LoadStep(g_SysWork.gameStateCounter) : -1;
        }
        if (s_CaptureFrameStep >= 0)
        {
            Capture_Packet(s_CaptureFrameStep, words, count);
        }
    }
    else if (g_SysWork.sysFlags & SysFlag_DemoActive)
    {
        if (!s_CaptureDemoOn)
        {
            s_CaptureDemoOn = 1;
            s_CaptureDemoCount++;
        }
        if (s_CaptureDemoCount == Capture_Demo())
        {
            Capture_Packet(g_Demo_DemoStep, words, count);
        }
    }
    else
    {
        s_CaptureDemoOn = 0;
    }
}

/* Widescreen (include/port/widescreen.h): Debug Options bit 3, or SH1_WIDESCREEN=1 builds. */
int         g_PortWidescreen;
extern void GpuGs_SetMovieXScale(s32 q12); /* ps2/gpu_gs.c */
extern void GpuGs_SetXScaleFilter(s32 on);
static s32  s_WsFull; /* the whole picture is 4:3, squeezed into the middle (black side bars) */
extern void GpuGs_SetXScale(s32 q12); /* ps2/gpu_gs.c */

#define WS_SQUEEZE Q12(0.75f)

/** Widescreen per game state: 0 off (menus: drawn 4:3 and stretched by the TV), 1 gameplay (3D hor+,
 * 2D squeezed), 2 a 4:3 picture in the middle (title, load screens, inventory, map). Movies are
 * pillarboxed whenever widescreen is on. */
static s32 widescreen_mode(void)
{
    s32 on;
    s32 mode;

#ifdef SH_PORT_WIDESCREEN
    on = 1;
#else
    on = (g_GameWork.config.unused_26 >> 3) & 1;
#endif
    GpuGs_SetMovieXScale(on ? WS_SQUEEZE : Q12(1.0f));
    GpuGs_SetXScaleFilter((g_GameWork.config.unused_26 >> 4) & 1); /* Debug Options: 2D Filter */
    if (!on)
    {
        mode = 0;
    }
    else switch (g_GameWork.gameState)
    {
        case GameState_InGame:
        case GameState_MapEvent:
            mode = 1;
            break;

        case GameState_AutoLoadSavegame:
        case GameState_LoadSavegameScreen:
        case GameState_SaveScreen:
        case GameState_OptionScreen:
            mode = 0;
            break;

        default:
            mode = 2;
            break;
    }
    g_PortWidescreen = mode == 1;
    return mode;
}

/** Widescreen: the X squeeze of each ordering-table layer. In gameplay, the 2D layers
 * (g_OrderingTable2: g_OtTags0) are squeezed, except the full-screen ones (slot 4: cutscene borders,
 * 5: screen fades); the 3D table is squeezed by its projection, except full-screen pictures, which
 * background_draw.c marks (they share the fog backdrop's entry). */
void HalGame_OnOtTag(const void* tag)
{
    u32 a  = (u32)tag;
    u32 t0 = (u32)&g_OtTags0[0][0];
    s32 slot;

    if (s_WsFull != 1)
    {
        return;
    }
    if (a >= t0 && a < t0 + sizeof(g_OtTags0))
    {
        slot = ((a - t0) / sizeof(GsOT_TAG)) % 16;
        GpuGs_SetXScale(slot == 4 || slot == 5 ? Q12(1.0f) : WS_SQUEEZE);
    }
    else if (((const u32*)tag)[1] == PORT_PICTURE_START)
    {
        GpuGs_SetXScale(WS_SQUEEZE);
    }
    else if (((const u32*)tag)[1] == PORT_PICTURE_END)
    {
        GpuGs_SetXScale(Q12(1.0f));
    }
}

void HalGame_OnDrawOTag(void)
{
    s_WsFull = widescreen_mode();
    GpuGs_SetXScale(s_WsFull == 2 ? WS_SQUEEZE : Q12(1.0f));
    if (s_WsFull == 2)
    {
        /* The 4:3 picture's side bars: the game only draws the middle now, and the sides would keep
         * whatever VRAM held before. */
        RECT bar = GsDRAWENV.clip;
        bar.w    = GsDRAWENV.clip.w / 8;
        ClearImage(&bar, 0, 0, 0);
        bar.x = GsDRAWENV.clip.x + GsDRAWENV.clip.w - bar.w;
        ClearImage(&bar, 0, 0, 0);
    }
#ifdef SH_PORT_SCREENREAD
    extern unsigned int g_PortSrContext; /* ps2/gpu_gs.c: the screen-read inventory's context */
    g_PortSrContext = ((u32)g_GameWork.gameState << 16) | (u32)(g_GameWork.gameStateSteps[0] & 0xFFFF);
#endif
}

void HalGame_Status(void)
{
    /* Load watchdog: the map load screen with nothing left to read for 10 s is a hang (the game waits
     * for something that will never load): dump RAM for tools/port/world_diag.py. */
    static u32 stuckTicks;
    extern int Port_RamDump(const char* path); /* debug_ps2.c */

    printf("; gameState %d step %d sysState %d; file queue %d entries, state %u, post-load %u, read idx %d\n",
           g_GameWork.gameState, g_GameWork.gameStateSteps[0], g_SysWork.sysState,
           Fs_QueueGetLength(), g_FsQueue.state, g_FsQueue.postLoadState, g_FsQueue.read.idx);
    if (g_GameWork.gameState == GameState_MainLoadScreen && Fs_QueueGetLength() == 0)
    {
        if (++stuckTicks == 20)
        {
            printf("port: load watchdog: map load stuck for 10 s (step %d) with no file left to read; "
                   "writing RAM to host:ramdump.bin\n", g_GameWork.gameStateSteps[0]);
            printf("port: load watchdog: %s; then tools/port/world_diag.py --ps2-ram build/port/ramdump.bin\n",
                   Port_RamDump("host:ramdump.bin") ? "written" : "could not write it");
        }
    }
    else
    {
        stuckTicks = 0;
    }
}

void HalGame_OnPutDispEnv(s32 x, s32 y, s32 w, s32 h)
{
    if (Capture_Pending())
    {
        Capture_Display(x, y, w, h, &g_SysWork, (int)sizeof(g_SysWork), &g_GameWork, (int)sizeof(g_GameWork), g_DeltaTime);
    }
    s_CaptureFrameStep = -2;
    if (s_CaptureDemoCount == Capture_Demo())
    {
        /* Out of the demo (its end): a step past any range ends the state capture. */
        Capture_State((g_SysWork.sysFlags & SysFlag_DemoActive) ? g_Demo_DemoStep : 0x7FFFFFFF, g_RngSeed, g_DeltaTime, &g_SysWork,
                      (int)sizeof(g_SysWork), &g_GameWork, (int)sizeof(g_GameWork));
    }
    {
        static s32 frames;
        Capture_Replay(++frames); /* compare_frames.py --replay */
    }
}

int HalGame_DisplayMode(void)
{
    if (Capture_Mode() >= 0)
    {
        return Capture_Mode(); /* compare_frames.py --mode */
    }
    return g_GameWork.config.unused_26 & 7; /* Debug Options: Resolution, Color Depth, Video Mode (options.c) */
}

void HalGame_Init(void)
{
    extern void Port_OverlaySnapshot(void); /* overlay.c */
    Port_OverlaySnapshot();
}
