/** @brief PS1 libgpu system layer (sys.o) for the port: VRAM transfers, ordering tables, draw/display
 * environments. Sony's primitive builders and sys.o's pure packet builders (SetDrawEnv, SetDrawMode,
 * ...) are recompiled (tools/port/recomp_all.sh); this file replaces the parts that drive the GPU.
 *
 * VRAM is emulated as a 1024x512 16-bit array, as on the PS1. GPU command packets (from ordering
 * tables, DrawPrim, draw environments) go to Gpu_Submit(), the hook for the GS renderer (Step 4);
 * for now it only counts them.
 *
 * Ordering tables and packets use 24-bit addresses, as on the PS1. That's valid on the EE as long as
 * all drawing memory is below 16 MiB (the whole program currently ends below 8 MiB).
 */

#include "common.h"
#include "libgte.h"
#include "libgpu.h"

#define VRAM_W 1024
#define VRAM_H 512
#define OT_END 0x00FFFFFF

extern int printf(const char* fmt, ...);

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

/** @brief Every GPU command packet (GP0 words) ends up here. */
static void Gpu_Submit(const u32* words, s32 count)
{
    (void)words;
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

int ResetGraph(int mode)
{
    (void)mode;
    return 0;
}

void SetDispMask(int mask)
{
    s_DispMask = mask;
}

int DrawSync(int mode)
{
    (void)mode;
    return 0; /* Everything completes immediately. */
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
    return 0;
}

int StoreImage(RECT* rect, u_long* p)
{
    u16* dst = (u16*)p;
    s32 x, y;
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

void DrawOTag(u_long* p)
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
        submit_tagged(prim);
        addr = prim[0] & 0xFFFFFF;
        steps++;
    }

    if (++s_DrawOTags % 60 == 0)
    {
        printf("libgpu: %u DrawOTag calls; last 60: %u packets, %u words\n", s_DrawOTags, s_FramePackets, s_FrameWords);
        s_FramePackets = 0;
        s_FrameWords   = 0;
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
    s_DispEnv = *env;
    return env;
}

DRAWENV* GetDrawEnv(DRAWENV* env)
{
    *env = s_DrawEnv;
    return env;
}
