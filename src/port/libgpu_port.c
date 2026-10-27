/** @brief PS1 libgpu system layer (sys.o) for the port: VRAM transfers, ordering tables, draw/display
 * environments. Sony's primitive builders and sys.o's pure packet builders (SetDrawEnv, SetDrawMode,
 * ...) are recompiled (tools/port/recomp_all.sh); this file replaces the parts that drive the GPU.
 *
 * GPU command packets (from ordering tables, DrawPrim, draw environments) go to Gpu_Submit(), then to
 * the GS renderer (src/port/ps2/gpu_gs.c), which draws into an image of the PS1's VRAM in GS memory:
 * the only copy of VRAM (LoadImage uploads to it, StoreImage reads it back). PutDispEnv shows the
 * display area (display_ps2.c). (The software renderer, gpu_soft.c, was retired on 2026-10-17:
 * comparisons are against DuckStation.)
 *
 * Ordering tables and packets use 24-bit addresses, as on the PS1. That's valid on the EE as long as
 * all drawing memory is below 16 MiB (the whole program currently ends below 8 MiB).
 */

#include "common.h"
#include "libgte.h"
#include "libgpu.h"
#include "game.h"
#include "port/prof.h"
#include "bodyprog/demo.h"
#include "main/fsqueue.h"
#include "bodyprog/screen/screen_data.h"

#define VRAM_W 1024
#define VRAM_H 512
#define OT_END 0x00FFFFFF

extern int printf(const char* fmt, ...);
extern void Display_PresentGs(s32 x, s32 y, s32 w, s32 h, s32 rgb24, s32 isinter);
extern void GpuGs_Commands(const u32* words, s32 count); /* src/port/ps2/gpu_gs.c */
extern void GpuGs_LoadImage(s32 x, s32 y, s32 w, s32 h, const u16* src, s32 stride);
extern void GpuGs_StoreImage(s32 x, s32 y, s32 w, s32 h, u16* dst);
extern void GpuGs_ClearImage(s32 x, s32 y, s32 w, s32 h, u32 rgb);

static DRAWENV s_DrawEnv;
static DISPENV s_DispEnv;
static s32     s_DispMask;

/* Statistics, printed about once a second until the GS renderer exists. */
static u32 s_FramePackets;
static u32 s_FrameWords;
static u32 s_DrawOTags;

static void* ptr24(u32 addr)
{
    return (void*)(unsigned long)(addr & 0xFFFFFF);
}

extern void Capture_Packet(int step, const unsigned int* words, int count); /* src/port/ps2/capture_ps2.c */
extern void Capture_Display(int x, int y, int w, int h, const void* state, int stateSize,
                            const void* state2, int state2Size, int deltaTime);
extern int  Capture_Pending(void);
extern int  Capture_Demo(void);
extern int  Capture_Mode(void);
extern int  Capture_Load(int* file, int* data);
extern void Capture_State(int step, unsigned int seed, int deltaTime, const void* state, int stateSize, const void* state2, int state2Size);
extern u32  g_RngSeed; /* main/rng.c */
extern void Capture_Replay(int frame);
extern int  Capture_LoadStep(int vblank);

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


/** @brief Every GPU command packet (GP0 words) ends up here. */
static void Gpu_Submit(const u32* words, s32 count)
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
    GpuGs_Commands(words, count);
    s_FramePackets++;
    s_FrameWords += count;
}

static void submit_tagged(const u32* prim)
{
    s32 len = prim[0] >> 24;
    if (len > 0)
    {
        Gpu_Submit(&prim[1], len);
    }
}

/* sys.o's state (recompiled data). Its packet builders (SetDrawEnv etc.) clamp against fields that
 * the PS1's ResetGraph sets up for the detected GPU. */
extern u32 GEnv[];

int ResetGraph(int mode)
{
    (void)mode;
    /* As left by the PS1's ResetGraph (read from the running game in DuckStation; the rest of GEnv
     * is per-frame state): +0 0x100, +4 VRAM size 1024x512 (h << 16 | w), +8 1. */
    GEnv[0] = 0x100;
    GEnv[1] = (VRAM_H << 16) | VRAM_W;
    GEnv[2] = 1;
    return 0;
}

void SetDispMask(int mask)
{
    s_DispMask = mask;
}

int DrawSync(int mode)
{
    extern void GpuGs_Sync(void);
    (void)mode;
    GpuGs_Sync();
    return 0;
}

int ClearImage(RECT* rect, u_char r, u_char g, u_char b)
{
    GpuGs_ClearImage(rect->x, rect->y, rect->w, rect->h, r | (g << 8) | (b << 16));
    return 0;
}

int ClearImage2(RECT* rect, u_char r, u_char g, u_char b)
{
    return ClearImage(rect, r, g, b);
}

int LoadImage(RECT* rect, u_long* p)
{
#ifdef SH_PORT_SCREENREAD
    {
        extern unsigned int g_PortGpuCaller; /* ps2/gpu_gs.c: screen-read inventory */
        g_PortGpuCaller = (unsigned int)(unsigned long)__builtin_return_address(0);
    }
#endif
    GpuGs_LoadImage(rect->x, rect->y, rect->w, rect->h, (const u16*)p, rect->w);
    return 0;
}

int StoreImage(RECT* rect, u_long* p)
{
#ifdef SH_PORT_SCREENREAD
    {
        extern unsigned int g_PortGpuCaller; /* ps2/gpu_gs.c: screen-read inventory */
        g_PortGpuCaller = (unsigned int)(unsigned long)__builtin_return_address(0);
    }
#endif
    GpuGs_StoreImage(rect->x, rect->y, rect->w, rect->h, (u16*)p);
    return 0;
}

int StoreImage2(RECT* rect, u_long* p)
{
    return StoreImage(rect, p);
}

u_long* ClearOTagR(u_long* ot, int n)
{
    s32 i;
    for (i = n - 1; i > 0; i--)
    {
        ot[i] = (u32)(unsigned long)&ot[i - 1] & 0xFFFFFF;
    }
    ot[0] = OT_END;
    return ot;
}

static void DrawOTagImpl(u_long* p);

void DrawOTag(u_long* p)
{
    LP_BEGIN(LP_DRAWOTAG)
    PROF_BEGIN("DrawOTag (GPU packets)")
    DrawOTagImpl(p);
    PROF_END("DrawOTag (GPU packets)")
    LP_END(LP_DRAWOTAG);
}

static void DrawOTagImpl(u_long* p)
{
#ifdef SH_PORT_SCREENREAD
    {
        extern unsigned int g_PortSrContext; /* ps2/gpu_gs.c: the screen-read inventory's context */
        g_PortSrContext = ((u32)g_GameWork.gameState << 16) | (u32)(g_GameWork.gameStateSteps[0] & 0xFFFF);
    }
#endif
    u32 addr  = (u32)(unsigned long)p & 0xFFFFFF;
    u32 start = addr;
    s32 steps = 0;

    while (addr != OT_END)
    {
        const u32* prim;
        /* Below 0x100000: a null link, or a packet outside RAM (the scratchpad at 0x70000000 truncates
         * to 0: the PS1's GPU couldn't read its scratchpad either). */
        if (addr < 0x100000 || steps > 0x40000)
        {
            static int reported;
            if (reported++ < 4)
            {
                u32 a = start;
                s32 i;
                printf("libgpu: DrawOTag(%08X): %s after %d links; chain:", start, addr == 0 ? "NULL link" : addr < 0x100000 ? "link outside RAM" : "no end", steps);
                for (i = 0; i < 12 && a != OT_END && a != 0; i++)
                {
                    printf(" %06X:%08X", a, *(const u32*)ptr24(a));
                    a = *(const u32*)ptr24(a) & 0xFFFFFF;
                }
                printf("\n");
            }
            break;
        }
        prim = (const u32*)ptr24(addr);
        {
            /* The next primitive's line starts loading while this one is converted: the ordering
             * table links scattered packets, and each walk step otherwise waited for a cache miss. */
            u32 next = prim[0] & 0xFFFFFF;
            if (next != OT_END && next != 0)
            {
                __asm__ volatile("pref 0, 0(%0)" : : "r"(ptr24(next)));
            }
            submit_tagged(prim);
            addr = next;
        }
        steps++;
    }

    if (++s_DrawOTags % 60 == 0)
    {
        printf("libgpu: %u DrawOTag calls; last 60: %u packets, %u words; gameState %d step %d sysState %d; "
               "file queue %d entries, state %u, post-load %u, read idx %d\n", s_DrawOTags,
               s_FramePackets, s_FrameWords, g_GameWork.gameState, g_GameWork.gameStateSteps[0], g_SysWork.sysState,
               Fs_QueueGetLength(), g_FsQueue.state, g_FsQueue.postLoadState, g_FsQueue.read.idx);
        s_FramePackets = 0;
        s_FrameWords   = 0;
        {
            /* Load watchdog: the map load screen with nothing left to read for 10 s is a hang (the
             * game waits for something that will never load): dump RAM for tools/port/world_diag.py. */
            static u32 stuckTicks;
            extern int Port_RamDump(const char* path); /* src/port/ps2/debug_ps2.c */
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
        {
            extern void GpuGs_Stats(u32 frames);
            GpuGs_Stats(30); /* 60 DrawOTag calls: two per frame */
        }
    }
}

void DrawPrim(void* p)
{
    submit_tagged((const u32*)p);
}

DRAWENV* PutDrawEnv(DRAWENV* env)
{
    /* As on the PS1: build the environment packet in env->dr_env, then send it. */
    SetDrawEnv(&env->dr_env, env);
    submit_tagged((const u32*)&env->dr_env);
    if (env->isbg)
    {
        ClearImage(&env->clip, env->r0, env->g0, env->b0);
    }
    s_DrawEnv = *env;
    return env;
}

DISPENV* PutDispEnv(DISPENV* env)
{
    if (Capture_Pending())
    {
        Capture_Display(env->disp.x, env->disp.y, env->disp.w, env->disp.h, &g_SysWork,
                        (int)sizeof(g_SysWork), &g_GameWork, (int)sizeof(g_GameWork), g_DeltaTime);
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
#ifdef SH_PORT_PROF
    {
        static u32 frames;
        if (++frames == 120)
        {
            PROF_BEGIN("prof: report (printing)")
            Prof_Report(frames);
#ifdef SH_PORT_PROF_GTE
            {
                extern void Gte_ProfSites(unsigned int frames);
                Gte_ProfSites(frames);
            }
#endif
            {
                extern void ProfFn_Report(unsigned int frames);
                ProfFn_Report(frames);
            }
            PROF_END("prof: report (printing)")
            frames = 0;
        }
    }
#endif
    s_DispEnv = *env;
    /* The game swaps display buffers with PutDispEnv once per frame: present the new display area. */
    if (s_DispMask)
    {
        extern int g_PortHiresOption; /* display_ps2.c: the 2x renderer */
        g_PortHiresOption = g_GameWork.config.unused_26 & 7; /* Debug Options: Resolution, Color Depth, Video Mode (options.c) */
        if (Capture_Mode() >= 0)
        {
            g_PortHiresOption = Capture_Mode(); /* compare_frames.py --mode */
        }
        PROF_BEGIN("gs: Display_PresentGs")
        Display_PresentGs(env->disp.x, env->disp.y, env->disp.w, env->disp.h, env->isrgb24, env->isinter);
        PROF_END("gs: Display_PresentGs")
    }
    return env;
}

DRAWENV* GetDrawEnv(DRAWENV* env)
{
    *env = s_DrawEnv;
    return env;
}
