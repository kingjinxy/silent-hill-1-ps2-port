/** @brief Sony's libgs in C for the port, from the library objects (lib/libgs), object by object.
 *
 * Each function does what the original's instructions do, in the same order, including the
 * variables of the objects it replaces (defined here; the recompiled libgs objects that remain use
 * them by name). Objects not yet here are still recompiled (tools/port/recomp_all.sh).
 */

#include "common.h"
#include <psyq/libgte.h>
#include <psyq/libgpu.h>
#include <psyq/libgs.h>

extern MATRIX _LC; /* gs_107.o: the local colour matrix */

extern long SetRCnt(unsigned long spec, unsigned short target, long mode);
extern long StartRCnt(unsigned long spec);
extern long GetRCnt(unsigned long spec);
extern long ResetRCnt(unsigned long spec);
extern int  GetVideoMode(void);

#define RCNT_VBLANK 0xF2000001 /* RCntCNT1 */

/* --- gs_001.o: display and drawing environments, double buffering ------------------------------*/

/** The two background fills of GsSortClear (BLK_FILL), one per buffer. */
static u32 s_ClearPrim[2][4];

short       PSDOFSX[2], PSDOFSY[2];   /* buffer origins in VRAM */
short       PSDBASEX[2], PSDBASEY[2];
_GsPOSITION POSITION;
DRAWENV     GsDRAWENV;
DISPENV     GsDISPENV;
short       GsORGOFSX, GsORGOFSY;
RECT        CLIP2;
u_long      PSDCNT;
short       PSDIDX;
short       PSDGPU;                   /* intmode bit 2: offsets by the GPU (drawing offset), not the GTE */

/* gs_104.o */
long HWD0, VWD0; /* screen width and height */
long GsCLIP3far, GsCLIP3near;
int  GsLIGHT_MODE;

void GsSetDrawBuffClip(void);
void GsSetDrawBuffOffset(void);
void gte_init(void);

/** Environments for a w x h screen (GsInitGraph's helper). */
static void init_envs(int w, int h, int intmode, int dither, int vram)
{
    u8* env = (u8*)&GsDRAWENV;

    ResetGraph(((intmode >> 4) & 3) == 3 ? 3 : 0);
    *(s16*)(env + 10) = 0; /* ofs */
    *(s16*)(env + 8)  = 0;
    *(s16*)(env + 18) = 0; /* tw */
    *(s16*)(env + 16) = 0;
    *(s16*)(env + 14) = 0;
    *(s16*)(env + 12) = 0;
    *(s16*)(env + 20) = 0; /* tpage */
    env[22]           = dither;
    env[23]           = 0;
    env[24]           = 0;
    PutDrawEnv(&GsDRAWENV);

    GsDISPENV.disp.x   = 0;
    GsDISPENV.disp.y   = 0;
    GsDISPENV.disp.w   = w;
    GsDISPENV.disp.h   = h;
    GsDISPENV.screen.x = 0;
    GsDISPENV.screen.y = 0;
    GsDISPENV.screen.w = 0;
    GsDISPENV.screen.h = 0;
    if (GetVideoMode() == 1) /* PAL */
    {
        GsDISPENV.screen.y = 24;
        GsDISPENV.pad0     = 1;
    }
    GsDISPENV.isinter = intmode & 1;
    PSDGPU            = intmode & 4;
    GsDISPENV.isrgb24 = vram;
    PutDispEnv(&GsDISPENV);
}

/** Screen size, the unit matrices (GsIDMATRIX2 with the aspect ratio), the light matrices, the
 * buffer bases, the clip area and the fill packets (GsInitGraph's and GsInitGraph2's helper). */
static void init_screen(int w, int h)
{
    s32 aspect;
    int i;

    HWD0   = w & 0xFFFF;
    VWD0   = h & 0xFFFF;
    aspect = (VWD0 << 14) / HWD0;

    GsIDMATRIX.m[0][2] = 0;
    GsIDMATRIX.m[0][1] = 0;
    GsIDMATRIX.m[1][2] = 0;
    GsIDMATRIX.m[1][0] = 0;
    GsIDMATRIX.m[2][1] = 0;
    GsIDMATRIX.m[2][0] = 0;
    GsIDMATRIX.t[2]    = 0;
    GsIDMATRIX.t[1]    = 0;
    GsIDMATRIX.t[0]    = 0;
    GsIDMATRIX.m[0][0] = 4096;
    GsIDMATRIX.m[1][1] = 4096;
    GsIDMATRIX.m[2][2] = 4096;
    for (i = 0; i < 8; i++)
    {
        ((u32*)&GsIDMATRIX2)[i] = ((u32*)&GsIDMATRIX)[i];
    }
    for (i = 0; i < 8; i++)
    {
        ((u32*)&GsLIGHTWSMATRIX)[i] = ((u32*)&GsIDMATRIX)[i];
    }
    GsLIGHTWSMATRIX.m[2][2] = 0;
    GsLIGHTWSMATRIX.m[1][1] = 0;
    GsLIGHTWSMATRIX.m[0][0] = 0;
    for (i = 0; i < 8; i++)
    {
        ((u32*)&_LC)[i] = ((u32*)&GsLIGHTWSMATRIX)[i];
    }

    PSDBASEX[0] = 0;
    PSDBASEX[1] = 0;
    PSDBASEY[0] = 0;
    PSDBASEY[1] = 0;
    POSITION.offy = 0;
    POSITION.offx = 0;
    CLIP2.y       = 0;
    GsIDMATRIX2.m[1][1] = aspect / 3;
    CLIP2.x       = 0;
    ((u8*)s_ClearPrim[0])[3] = 3;
    ((u8*)s_ClearPrim[0])[7] = 2;
    ((u8*)s_ClearPrim[1])[3] = 3;
    ((u8*)s_ClearPrim[1])[7] = 2;
    PSDCNT  = 1;
    CLIP2.w = HWD0;
    CLIP2.h = VWD0;
}

void GsInitGraph(unsigned short x_res, unsigned short y_res, unsigned short intmode, unsigned short dither,
                 unsigned short vram)
{
    init_envs(x_res, y_res, intmode, dither, vram);
    gte_init();
    PSDIDX = 0;
    init_screen(x_res, y_res);
    GsSetDrawBuffClip();
    GsSetDrawBuffOffset();
}

/** As GsInitGraph, for an already initialised GPU: environments updated, not put. */
void GsInitGraph2(unsigned short x_res, unsigned short y_res, unsigned short intmode, unsigned short dither,
                  unsigned short vram)
{
    u8* env = (u8*)&GsDRAWENV;
    *(s16*)(env + 20) = 0;
    env[22]           = dither;
    env[23]           = 0;
    env[24]           = 0;
    GsDISPENV.disp.w  = x_res;
    GsDISPENV.disp.h  = y_res;
    GsDISPENV.isinter = intmode & 1;
    PSDGPU            = intmode & 4;
    GsDISPENV.isrgb24 = vram;
    init_screen(x_res, y_res);
}

/** The current buffer's background fill (24-bit screens: 1.5 x the width in 16-bit pixels). */
void GsSortClear(unsigned char r, unsigned char g, unsigned char b, GsOT* ot)
{
    u8* p = (u8*)s_ClearPrim[PSDIDX];
    p[4]  = r;
    p     = (u8*)s_ClearPrim[PSDIDX];
    p[5]  = g;
    p     = (u8*)s_ClearPrim[PSDIDX];
    p[6]  = b;
    p     = (u8*)s_ClearPrim[PSDIDX];
    *(u16*)(p + 8)  = PSDOFSX[PSDIDX];
    *(u16*)(p + 14) = VWD0;
    *(u16*)(p + 10) = PSDOFSY[PSDIDX];
    if (GsDISPENV.isrgb24)
    {
        *(u16*)(p + 12) = (HWD0 * 3) / 2;
    }
    else
    {
        *(u16*)(p + 12) = HWD0;
    }
    AddPrim(ot->tag, s_ClearPrim[PSDIDX]);
}

/* --- gs_002.o, gs_003.o, gs_010.o, gs_127.o: double buffers -------------------------------------*/

int GsGetActiveBuff(void)
{
    return PSDIDX;
}

/** The drawing offset of the current buffer: by the GPU (PSDGPU), or by the GTE (the other
 * buffer's origin, as the original indexes it). */
void GsSetDrawBuffOffset(void)
{
    s32 x, y;
    if (PSDGPU != 0)
    {
        s32 i = PSDIDX;
        GsORGOFSY = 0;
        GsORGOFSX = 0;
        GsDRAWENV.ofs[0] = (u16)POSITION.offx + (u16)PSDOFSX[i];
        GsDRAWENV.ofs[1] = (u16)POSITION.offy + (u16)PSDOFSY[i];
        PutDrawEnv(&GsDRAWENV);
        return;
    }
    x = POSITION.offx + PSDOFSX[PSDIDX != 0 ? 0 : 1];
    y = POSITION.offy + PSDOFSY[PSDIDX != 0 ? 0 : 1];
    SetGeomOffset(x, y);
    GsORGOFSX = x;
    GsORGOFSY = y;
}

void GsSetDrawBuffClip(void)
{
    s32 i = PSDIDX;
    GsDRAWENV.clip.h = CLIP2.h;
    GsDRAWENV.clip.w = CLIP2.w;
    GsDRAWENV.clip.x = CLIP2.x + PSDOFSX[i];
    GsDRAWENV.clip.y = CLIP2.y + PSDOFSY[i];
    PutDrawEnv(&GsDRAWENV);
}

void GsSwapDispBuff(void)
{
    u_long n;
    s32    i = PSDIDX;
    GsDISPENV.disp.x = PSDOFSX[i];
    GsDISPENV.disp.y = PSDOFSY[i];
    PutDispEnv(&GsDISPENV);
    SetDispMask(1);
    n      = PSDCNT + 1;
    PSDCNT = n;
    PSDCNT = n != 0 ? n : 1;
    PSDIDX = PSDIDX == 0;
    GsSetDrawBuffClip();
    GsSetDrawBuffOffset();
}

void GsDefDispBuff2(unsigned short x0, unsigned short y0, unsigned short x1, unsigned short y1)
{
    PSDOFSX[0] = x0;
    PSDOFSX[1] = x1;
    PSDOFSY[0] = y0;
    PSDOFSY[1] = y1;
    if (PSDGPU != 0)
    {
        PSDBASEX[0] = 0;
        PSDBASEX[1] = 0;
        PSDBASEY[0] = 0;
        PSDBASEY[1] = 0;
    }
    else
    {
        PSDBASEX[0] = x0;
        PSDBASEX[1] = x1;
        PSDBASEY[0] = y0;
        PSDBASEY[1] = y1;
    }
}

/* --- gs_007-009.o: vertical blank counter (root counter 1) --------------------------------------*/

void GsInitVcount(void)
{
    SetRCnt(RCNT_VBLANK, 0xFFFF, 0x2000);
    StartRCnt(RCNT_VBLANK);
}

long GsGetVcount(void)
{
    return GetRCnt(RCNT_VBLANK);
}

void GsClearVcount(void)
{
    ResetRCnt(RCNT_VBLANK);
}

/* --- gs_104-106.o, gs_121.o: 3D setup -----------------------------------------------------------*/

void GsInit3D(void)
{
    POSITION.offx = HWD0 / 2;
    POSITION.offy = VWD0 / 2;
    GsSetDrawBuffOffset();
    GsCLIP3near  = 10;
    GsLIGHT_MODE = 0;
    GsCLIP3far   = 16383;
}

void gte_init(void)
{
    InitGeom();
    SetFarColor(0, 0, 0);
    SetGeomOffset(0, 0);
    GsORGOFSY = 0;
    GsORGOFSX = 0;
}

/** TMD offsets to addresses (once: bit 0 of the flags marks it done). */
void GsMapModelingData(unsigned long* p)
{
    u32* obj;
    s32  i, n;
    if (p[0] & 1)
    {
        return;
    }
    p[0] |= 1;
    n   = (s32)p[1];
    obj = (u32*)(p + 2);
    for (i = 0; i < n; i++)
    {
        obj[i * 7 + 0] += (u32)(unsigned long)obj;
        obj[i * 7 + 4] += (u32)(unsigned long)obj;
        obj[i * 7 + 2] += (u32)(unsigned long)obj;
    }
}

void GsSetProjection(long h)
{
    SetGeomScreen(h);
}

/* --- gs_111.o, gs_113.o, gs_114.o: ordering tables ----------------------------------------------*/

void GsDrawOt(GsOT* ot)
{
    DrawOTag((u_long*)ot->tag);
}

void GsClearOt(unsigned short offset, unsigned short point, GsOT* ot)
{
    ot->offset = offset;
    ot->point  = point;
    ot->tag    = (GsOT_TAG*)((u8*)ot->org + (4 << ot->length) - 4);
    ClearOTagR((u_long*)ot->org, 1 << ot->length);
}

/** Links ot_src's list into ot_dest at ot_src->point, as the original walks it (the second to last
 * entry is the one relinked). */
GsOT* GsSortOt(GsOT* ot_src, GsOT* ot_dest)
{
    u32* p    = (u32*)ot_src->org;
    u32* prev = p;
    u32* last = p;
    u32* at   = (u32*)ot_dest->org + (s32)(ot_src->point - ot_dest->offset);

    if ((*p & 0xFFFFFF) != 0xFFFFFF)
    {
        do
        {
            last = prev;
            prev = p;
            p    = (u32*)(unsigned long)(*p & 0xFFFFFF);
        } while ((*p & 0xFFFFFF) != 0xFFFFFF);
    }
    *last = (*last & 0xFF000000) | (*at & 0xFFFFFF);
    *at   = (*at & 0xFF000000) | ((u32)(unsigned long)ot_src->tag & 0xFFFFFF);
    return ot_dest;
}
