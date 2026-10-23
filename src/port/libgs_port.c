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

extern MATRIX _LC; /* below */

extern long SetRCnt(unsigned long spec, unsigned short target, long mode);
extern long StartRCnt(unsigned long spec);
extern long GetRCnt(unsigned long spec);
extern long ResetRCnt(unsigned long spec);
extern int  GetVideoMode(void);

#define RCNT_VBLANK 0xF2000001 /* RCntCNT1 */

/* --- global.o, gs_123.o: libgs's variables --------------------------------------------------------*/

long    GsADIVZ;
short   GsADIVW = 1023, GsADIVH = 511; /* subdivision limits */
u_long  GsDISPON, GsLIOFF, GsLMODE, GsLIGNR, GsZOVER, GsBACKC, GsMATE_C, GsTRATE, GsTON, GsNDIV;
PACKET* GsOUT_PACKET_P;                /* where the next packet is built */
MATRIX  GsWSMATRIX_ORG, GsIDMATRIX, GsWSMATRIX, GsIDMATRIX2;

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

/* --- gs_107.o, gs_108.o, gs_110.o: lights ---------------------------------------------------------*/

MATRIX GsLIGHTWSMATRIX; /* light directions (rows), world to screen */
MATRIX _LC;             /* light colours (columns) */

extern int printf(const char* fmt, ...);

/** A whole matrix, as the originals' eight word moves (with the padding). */
static void copy_matrix(MATRIX* dst, const MATRIX* src)
{
    int i;
    for (i = 0; i < 8; i++)
    {
        ((u32*)dst)[i] = ((const u32*)src)[i];
    }
}

/** A colour 0-255 as 0-4096 (x << 12 / 255: the original's multiply by 0x80808081). */
static s32 colour12(u32 c)
{
    s32 x = (s32)(c << 12);
    return (s32)((((s64)x * (s32)0x80808081) >> 32) + x) >> 7;
}

/** Light `id` (0-2): its direction (negated, normalised) into a row of GsLIGHTWSMATRIX, its colour
 * into a column of the colour matrix. Returns -1 for a zero vector. */
int GsSetFlatLight(int id, GsF_LIGHT* lt)
{
    MATRIX l, c;
    u32    r = lt->r, g = lt->g, b = lt->b;
    s32    len;

    copy_matrix(&l, &GsLIGHTWSMATRIX);
    copy_matrix(&c, &_LC);
    len = SquareRoot0((u32)lt->vx * lt->vx + (u32)lt->vy * lt->vy + (u32)lt->vz * lt->vz);
    if (len == 0)
    {
        return -1;
    }
    if (id >= 0 && id <= 2)
    {
        l.m[id][0] = (s32)((u32)-lt->vx << 12) / len;
        l.m[id][1] = (s32)((u32)-lt->vy << 12) / len;
        c.m[0][id] = colour12(r);
        c.m[1][id] = colour12(g);
        c.m[2][id] = colour12(b);
        l.m[id][2] = (s32)((u32)-lt->vz << 12) / len;
    }
    copy_matrix(&GsLIGHTWSMATRIX, &l);
    copy_matrix(&_LC, &c);
    SetColorMatrix(&_LC);
    return 0;
}

void GsSetLightMode(int mode)
{
    switch (mode)
    {
        case 0:
            GsLIGHT_MODE = 0;
            break;
        case 1:
        case 2:
        case 3:
            GsLIGHT_MODE = mode;
            break;
        default:
            printf("not supported light mode %d\n", mode);
    }
}

void GsSetAmbient(long r, long g, long b)
{
    SetBackColor(r >> 4, g >> 4, b >> 4);
}

/* --- matrix.o: coordinate systems ----------------------------------------------------------------*/

void GsInitCoordinate2(GsCOORDINATE2* super, GsCOORDINATE2* base)
{
    copy_matrix(&base->coord, &GsIDMATRIX);
    base->super = super;
    base->flg   = 0;
    if ((unsigned long)super >= 2)
    {
        base->super->sub = base;
    }
}

void GsSetLsMatrix(MATRIX* mp)
{
    SetRotMatrix(mp);
    SetTransMatrix(mp);
}

/** The light matrix times mp into LLM; the GTE's rotation is saved around the product. */
void GsSetLightMatrix(MATRIX* mp)
{
    MATRIX l;
    copy_matrix(&l, &GsLIGHTWSMATRIX);
    PushMatrix();
    MulMatrix(&l, mp);
    PopMatrix();
    SetLightMatrix(&l);
}

/** As GsSetLightMatrix, but the product replaces mp. */
void GsSetLightMatrix2(MATRIX* mp)
{
    SetLightMatrix(MulMatrix2(&GsLIGHTWSMATRIX, mp));
}

/** m3 = m1 * m2, with translation m1 * m2.t + m1.t. */
void GsMulCoord0(MATRIX* m1, MATRIX* m2, MATRIX* m3)
{
    ApplyMatrixLV(m1, (VECTOR*)m2->t, (VECTOR*)m3->t);
    MulMatrix0(m1, m2, m3);
    m3->t[0] += m1->t[0];
    m3->t[1] += m1->t[1];
    m3->t[2] += m1->t[2];
}

/** m2 = m1 * m2. */
void GsMulCoord2(MATRIX* m1, MATRIX* m2)
{
    VECTOR v;
    ApplyMatrixLV(m1, (VECTOR*)m2->t, &v);
    MulMatrix2(m1, m2);
    m2->t[0] = v.vx + m1->t[0];
    m2->t[1] = v.vy + m1->t[1];
    m2->t[2] = v.vz + m1->t[2];
}

/** m1 = m1 * m2. */
void GsMulCoord3(MATRIX* m1, MATRIX* m2)
{
    VECTOR v;
    ApplyMatrixLV(m1, (VECTOR*)m2->t, &v);
    MulMatrix(m1, m2);
    m1->t[0] = v.vx + m1->t[0];
    m1->t[1] = v.vy + m1->t[1];
    m1->t[2] = v.vz + m1->t[2];
}

/* --- objt.o: GsLinkObject4 -------------------------------------------------------------------------*/

/** Bytes of a TMD primitive by (mode & 0xFD) - 0x20 (the original's jump table), for flag bit 2 clear
 * and set; 0 = not a GPU code libgs knows. */
static const u8 TMD_PRIM_SIZE[30][2] = {
    { 16, 24 }, { 16, 16 }, { 0, 0 }, { 0, 0 }, { 24, 24 }, { 28, 28 }, { 0, 0 }, { 0, 0 },
    { 20, 32 }, { 16, 16 }, { 0, 0 }, { 0, 0 }, { 32, 32 }, { 32, 32 }, { 0, 0 }, { 0, 0 },
    { 20, 28 }, { 24, 24 }, { 0, 0 }, { 0, 0 }, { 28, 28 }, { 36, 36 }, { 0, 0 }, { 0, 0 },
    { 24, 36 }, { 28, 28 }, { 0, 0 }, { 0, 0 }, { 36, 36 }, { 44, 44 },
};

/** Object `n` of a TMD to `objp`, and each run of primitives of the same mode and flags gets its
 * length in the first one's low halfword (for GsSortObject4). */
void GsLinkObject4(unsigned long tmd_base, GsDOBJ2* objp, int n)
{
    u32* obj  = (u32*)(unsigned long)(tmd_base + n * 28);
    u8*  p    = (u8*)(unsigned long)obj[4];
    u8*  run  = p;
    u32  nprim = obj[5], i, mode = 0, flag = 0, count = 0;

    objp->tmd = (unsigned long*)obj;
    for (i = 0; i < nprim; i++, count++)
    {
        u32 prevMode = mode & 0xFF, prevFlag = flag;
        u32 w        = *(u32*)p, k, size;
        mode = w >> 24;
        flag = w >> 16;
        if (prevMode != 0 && (mode != prevMode || (flag & 0xFF) != (prevFlag & 0xFF)))
        {
            *(u16*)run = count;
            count      = 0;
            run        = p;
        }
        k    = (mode & 0xFD) - 0x20;
        size = k < 30 ? TMD_PRIM_SIZE[k][(flag & 4) ? 1 : 0] : 0;
        if (size == 0)
        {
            printf("GPU CODE %02xH not assigned.\n", mode);
        }
        p += size;
    }
    *(u16*)run = count;
}

/* --- 2d_sp1.o: GsSortFastSprite --------------------------------------------------------------------*/

/** A 16-bit sprite (GP0 64h) with its draw mode packet, at GsOUT_PACKET_P, linked into `ot` at `pri`. */
void GsSortFastSprite(GsSPRITE* sp, GsOT* ot, unsigned short pri)
{
    u32  attr = sp->attribute;
    u32* p, *at;

    if ((s32)attr < 0 || sp->w == 0 || sp->h == 0)
    {
        return;
    }
    p    = (u32*)GsOUT_PACKET_P;
    p[1] = (sp->tpage & 0x1F) | (((attr >> 17) & 0x180) | 0xE1000200) | ((attr >> 23) & 0x60);
    p[3] = ((sp->x + (u16)GsORGOFSX) & 0xFFFF) | ((u32)(sp->y + (u16)GsORGOFSY) << 16);
    p[2] = ((attr >> 5) & 0x02000000) | (((attr << 18) & 0x01000000) | 0x64000000) | ((u32)sp->b << 16) |
           ((u32)sp->g << 8) | sp->r;
    p[4] = (sp->u | ((u32)sp->v << 8)) | ((u32)sp->cy << 22) | (((u32)sp->cx << 12) & 0x3F0000);
    p[5] = sp->w | ((u32)sp->h << 16);
    at   = (u32*)ot->org + pri - ot->offset;
    p[0] = *at + 0x05000000;
    GsOUT_PACKET_P = (PACKET*)(p + 6);
    *at  = (u32)(unsigned long)p & 0xFFFFFF;
}

/* --- objt3.o: GsSortObject4J -------------------------------------------------------------------------*/

_GsFCALL GsFCALL4; /* the renderers per primitive type, set by the game (ItemScreen_TmdGsFCallInit) */

/** The renderers, as GsSortObject4J calls them: lit types with the normals, the unlit ("n") ones
 * without (one argument fewer). */
typedef PACKET* (*GsRenderLit)(u32 prim, u32 vert, u32 normal, PACKET* out, u32 n, u32 shift, GsOT* ot, u32* scratch);
typedef PACKET* (*GsRenderUnlit)(u32 prim, u32 vert, PACKET* out, u32 n, u32 shift, GsOT* ot, u32* scratch);

/** Per (mode & 0xFD) - 0x20, as the original's jump table: the table entry (byte offset into
 * GsFCALL4; for F3, G3, F4 and G4 with flag bit 2 the gradation one, [light] only), whether it's lit,
 * and the primitive's bytes (plain / gradation). Offset 0xFFFF: not supported. */
typedef struct
{
    u16 at, atGrad; /* lit: [div][light] base / [light] base for flag bit 2; unlit: [div] base */
    u8  lit;
    u8  size, sizeGrad;
} SortCase;

static const SortCase SORT_CASES[30] = {
    { 0, 256, 1, 16, 24 },     /* 20h F3: f3 / f3g */
    { 24, 24, 0, 16, 16 },     /* 21h: nf3 */
    { 0xFFFF }, { 0xFFFF },
    { 64, 64, 1, 24, 24 },     /* 24h FT3: tf3 */
    { 88, 88, 0, 28, 28 },     /* 25h: ntf3 */
    { 0xFFFF }, { 0xFFFF },
    { 128, 280, 1, 20, 32 },   /* 28h F4: f4 / f4g */
    { 152, 152, 0, 16, 16 },   /* 29h: nf4 */
    { 0xFFFF }, { 0xFFFF },
    { 192, 192, 1, 32, 32 },   /* 2Ch FT4: tf4 */
    { 216, 216, 0, 32, 32 },   /* 2Dh: ntf4 */
    { 0xFFFF }, { 0xFFFF },
    { 32, 268, 1, 20, 28 },    /* 30h G3: g3 / g3g */
    { 56, 56, 0, 24, 24 },     /* 31h: ng3 */
    { 0xFFFF }, { 0xFFFF },
    { 96, 96, 1, 28, 28 },     /* 34h GT3: tg3 */
    { 120, 120, 0, 36, 36 },   /* 35h: ntg3 */
    { 0xFFFF }, { 0xFFFF },
    { 160, 292, 1, 24, 36 },   /* 38h G4: g4 / g4g */
    { 184, 184, 0, 28, 28 },   /* 39h: ng4 */
    { 0xFFFF }, { 0xFFFF },
    { 224, 224, 1, 36, 36 },   /* 3Ch GT4: tg4 */
    { 248, 248, 0, 44, 44 },   /* 3Dh: ntg4 */
};

/** Draws a TMD object (GsLinkObject4) through the GsFCALL4 renderers, one call per run of
 * primitives of the same type. The attribute's bits go to the libgs globals the renderers read. */
void GsSortObject4J(GsDOBJ2* objp, GsOT* ot, int shift, u_long* scratch)
{
    u32 attr = objp->attribute;
    u32 div, light, *obj, nprim, vert, normal;
    u8* p;

    if ((s32)attr < 0)
    {
        return;
    }
    GsMATE_C = attr & 7;
    GsLMODE  = (attr >> 3) & 3;
    GsLIGNR  = (attr >> 5) & 1;
    GsLIOFF  = (attr >> 6) & 1;
    GsNDIV   = (attr >> 9) & 7;
    GsTON    = (attr >> 30) & 1;
    div      = 0;
    if (GsNDIV != 0)
    {
        scratch[0] = GsNDIV;
        scratch[1] = HWD0;
        scratch[2] = VWD0;
        div        = 1;
    }
    if (GsLIOFF == 1)
    {
        light = 2;
    }
    else if (GsLIGNR != 0)
    {
        light = (GsLIGNR == 1 && (GsLMODE & 1)) ? 1 : 0;
    }
    else
    {
        light = GsLIGHT_MODE & 1;
    }

    obj    = (u32*)objp->tmd;
    p      = (u8*)(unsigned long)obj[4];
    nprim  = obj[5];
    vert   = obj[0];
    normal = obj[2];
    while (nprim != 0)
    {
        u32             code = p[3] & 0xFD, k = code - 0x20, n, grad;
        const SortCase* c;
        if (k >= 30 || SORT_CASES[k].at == 0xFFFF)
        {
            printf("non supported code %x %x\n", code, p); /* and as the original, no progress */
            continue;
        }
        c    = &SORT_CASES[k];
        n    = *(u16*)p;
        grad = c->lit && (*(u16*)(p + 2) & 4) && c->atGrad != c->at;
        if (c->lit)
        {
            u32 at = grad ? c->atGrad + light * 4 : c->at + div * 12 + light * 4;
            GsOUT_PACKET_P = (*(GsRenderLit*)((u8*)&GsFCALL4 + at))((u32)(unsigned long)p, vert, normal, GsOUT_PACKET_P, n,
                                                                    shift, ot, (u32*)scratch);
        }
        else
        {
            GsOUT_PACKET_P = (*(GsRenderUnlit*)((u8*)&GsFCALL4 + c->at + div * 4))((u32)(unsigned long)p, vert, GsOUT_PACKET_P,
                                                                                 n, shift, ot, (u32*)scratch);
        }
        nprim -= n;
        p += n * (grad ? c->sizeGrad : c->size);
    }
}
