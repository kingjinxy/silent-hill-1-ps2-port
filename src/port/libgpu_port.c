/** @brief PS1 libgpu system layer (sys.o) for the port: VRAM transfers, ordering tables, draw/display
 * environments. Sony's primitive builders and sys.o's pure packet builders (SetDrawEnv, SetDrawMode,
 * ...) are recompiled (tools/port/recomp_all.sh); this file replaces the parts that drive the GPU.
 *
 * GPU command packets (from ordering tables, DrawPrim, draw environments) go to Gpu_Submit(), then to
 * one of two renderers:
 *  - GS (default, src/port/ps2/gpu_gs.c): draws with the GS into an image of VRAM in GS memory.
 *    g_PortVram is the EE's copy, updated by CPU uploads (LoadImage, ClearImage) and read back from
 *    the GS by StoreImage; the GS renderer builds 4/8-bit texture pages from it.
 *  - Software (gpu_soft.c, g_PortGsRenderer = 0): draws into g_PortVram; the reference renderer.
 * PutDispEnv shows the display area (display_ps2.c).
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
extern void GpuSoft_Commands(const u32* words, s32 count);      /* src/port/gpu_soft.c */
extern void Display_Present(const u16* vram, s32 x, s32 y, s32 w, s32 h, s32 rgb24, s32 isinter); /* src/port/ps2/display_ps2.c */
extern void Display_PresentGs(s32 x, s32 y, s32 w, s32 h, s32 rgb24, s32 isinter);
extern void GpuGs_Commands(const u32* words, s32 count); /* src/port/ps2/gpu_gs.c */
extern void GpuGs_LoadImage(s32 x, s32 y, s32 w, s32 h);
extern void GpuGs_ClearImage(s32 x, s32 y, s32 w, s32 h, u32 rgb);
extern void GpuGs_StoreAll(void);

/** 1: GS renderer, 0: software renderer, 2: both (compare mode: the software renderer owns
 * g_PortVram, the GS output is shown, and the debug dumps compare the two; see display_ps2.c).
 * Chosen at build time: SH1_GPU=gs|soft|compare for tools/port/port_link.py. */
#ifndef SH_PORT_GPU
#define SH_PORT_GPU 1
#endif
s32 g_PortGsRenderer = SH_PORT_GPU;

u16 g_PortVram[VRAM_H][VRAM_W] __attribute__((aligned(64)));

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
extern void Capture_Display(const unsigned short* vram, int x, int y, int w, int h, const void* state, int stateSize,
                            const void* state2, int state2Size, int deltaTime);
extern int  Capture_Pending(void);

/** The first attract demo's id (captures are only of it), -1 until a demo runs. */
static s32 s_CaptureDemoId = -1;

/** @brief Every GPU command packet (GP0 words) ends up here. */
static void Gpu_Submit(const u32* words, s32 count)
{
    if (g_SysWork.sysFlags & SysFlag_DemoActive)
    {
        if (s_CaptureDemoId < 0)
        {
            s_CaptureDemoId = g_Demo_DemoId;
        }
        if (g_Demo_DemoId == s_CaptureDemoId)
        {
            Capture_Packet(g_Demo_DemoStep, words, count);
        }
    }
    if (g_PortGsRenderer)
    {
        GpuGs_Commands(words, count);
    }
    if (g_PortGsRenderer != 1)
    {
        GpuSoft_Commands(words, count);
    }
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
    (void)mode;
    if (g_PortGsRenderer)
    {
        extern void GpuGs_Sync(void);
        GpuGs_Sync(); /* the GS renderer sends packets without waiting (src/port/ps2/gpu_gs.c) */
    }
    return 0;
}

int ClearImage(RECT* rect, u_char r, u_char g, u_char b)
{
    u16 c = (u16)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
    s32 x, y;
    for (y = 0; y < rect->h; y++)
    {
        for (x = 0; x < rect->w; x++)
        {
            g_PortVram[(rect->y + y) & (VRAM_H - 1)][(rect->x + x) & (VRAM_W - 1)] = c;
        }
    }
    if (g_PortGsRenderer)
    {
        GpuGs_ClearImage(rect->x, rect->y, rect->w, rect->h, r | (g << 8) | (b << 16));
    }
    return 0;
}

int ClearImage2(RECT* rect, u_char r, u_char g, u_char b)
{
    return ClearImage(rect, r, g, b);
}

int LoadImage(RECT* rect, u_long* p)
{
    const u16* src = (const u16*)p;
    s32 x, y;
    for (y = 0; y < rect->h; y++)
    {
        for (x = 0; x < rect->w; x++)
        {
            g_PortVram[(rect->y + y) & (VRAM_H - 1)][(rect->x + x) & (VRAM_W - 1)] = *src++;
        }
    }
    if (g_PortGsRenderer)
    {
        GpuGs_LoadImage(rect->x, rect->y, rect->w, rect->h);
    }
    return 0;
}

int StoreImage(RECT* rect, u_long* p)
{
    u16* dst = (u16*)p;
    s32 x, y;
    if (g_PortGsRenderer == 1)
    {
        GpuGs_StoreAll(); /* the GS holds the current VRAM */
    }
    for (y = 0; y < rect->h; y++)
    {
        for (x = 0; x < rect->w; x++)
        {
            *dst++ = g_PortVram[(rect->y + y) & (VRAM_H - 1)][(rect->x + x) & (VRAM_W - 1)];
        }
    }
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
    PROF_BEGIN("DrawOTag (GPU packets)")
    DrawOTagImpl(p);
    PROF_END("DrawOTag (GPU packets)")
}

static void DrawOTagImpl(u_long* p)
{
    u32 addr  = (u32)(unsigned long)p & 0xFFFFFF;
    u32 start = addr;
    s32 steps = 0;

    while (addr != OT_END)
    {
        const u32* prim;
        if (addr == 0 || steps > 0x40000)
        {
            static int reported;
            if (reported++ < 4)
            {
                u32 a = start;
                s32 i;
                printf("libgpu: DrawOTag(%08X): %s after %d links; chain:", start, addr == 0 ? "NULL link" : "no end", steps);
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
        if (g_PortGsRenderer)
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
        if (g_PortGsRenderer == 1)
        {
            GpuGs_StoreAll();
        }
        Capture_Display(&g_PortVram[0][0], env->disp.x, env->disp.y, env->disp.w, env->disp.h, &g_SysWork,
                        (int)sizeof(g_SysWork), &g_GameWork, (int)sizeof(g_GameWork), g_DeltaTime);
    }
#ifdef SH_PORT_PROF
    {
        static u32 frames;
        if (++frames == 120)
        {
            PROF_BEGIN("prof: report (printing)")
            Prof_Report(frames);
            {
                extern void Gte_ProfSites(unsigned int frames);
                Gte_ProfSites(frames);
            }
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
        if (g_PortGsRenderer)
        {
            PROF_BEGIN("gs: Display_PresentGs")
            Display_PresentGs(env->disp.x, env->disp.y, env->disp.w, env->disp.h, env->isrgb24, env->isinter);
            PROF_END("gs: Display_PresentGs")
        }
        else
        {
            Display_Present(&g_PortVram[0][0], env->disp.x, env->disp.y, env->disp.w, env->disp.h, env->isrgb24, env->isinter);
        }
    }
    return env;
}

DRAWENV* GetDrawEnv(DRAWENV* env)
{
    *env = s_DrawEnv;
    return env;
}
