/** @brief Sony's libgpu packet builders in C for the port, from the library objects (lib/libgpu: the
 * primitive objects and the builders in sys.o). They only fill memory; the GPU system layer is
 * libgpu_port.c. Each stores what the original does, in its order. tools/port/lib_test compares them
 * with the originals.
 */

#include "common.h"
#include <psyq/libgte.h>
#include <psyq/libgpu.h>

extern void* memset(void* p, int c, unsigned int n);

/** sys.o's `GEnv` (296 bytes of its .data). ResetGraph (libgpu_port.c) sets +4: the VRAM width and
 * height, which the area builders clamp to; byte 2 is the debug level (SetGraphDebug). */
u32 GEnv[74];

#define GENV_W  (((s16*)GEnv)[2])
#define GENV_H  (((s16*)GEnv)[3])
#define LEN(p)  (((u8*)(p))[3])
#define WORD(p, i) (((u32*)(p))[i])

int GetGraphDebug(void)
{
    return ((u8*)GEnv)[2];
}

/* --- Primitives (p06-p34.o) ---------------------------------------------------------------------*/

void AddPrim(void* ot, void* p)
{
    WORD(p, 0)  = (WORD(p, 0) & 0xFF000000) | (WORD(ot, 0) & 0x00FFFFFF);
    WORD(ot, 0) = (WORD(ot, 0) & 0xFF000000) | ((u32)(unsigned long)p & 0x00FFFFFF);
}

void TermPrim(void* p)
{
    WORD(p, 0) |= 0x00FFFFFF;
}

/** Length (words after the tag) and GP0 command. */
static void set_prim(void* p, u8 len, u8 code)
{
    LEN(p)          = len;
    ((u8*)p)[7]     = code;
}

void SetPolyG3(POLY_G3* p)   { set_prim(p, 6, 0x30); }
void SetPolyF4(POLY_F4* p)   { set_prim(p, 5, 0x28); }
void SetPolyFT4(POLY_FT4* p) { set_prim(p, 9, 0x2C); }
void SetPolyG4(POLY_G4* p)   { set_prim(p, 8, 0x38); }
void SetTile(TILE* p)        { set_prim(p, 3, 0x60); }

/* --- Drawing settings (sys.o) -------------------------------------------------------------------*/

/** GP0 E1: draw mode. */
static u32 get_mode(int dfe, int dtd, int tpage)
{
    return 0xE1000000 | (dtd ? 0x200 : 0) | (tpage & 0x9FF) | (dfe ? 0x400 : 0);
}

/** A coordinate clamped to [0, size - 1] (size from GEnv). */
static s32 clamp_to(s32 v, s16 size)
{
    v = (s16)v;
    if (v < 0)
    {
        return 0;
    }
    return size - 1 < v ? (u16)size - 1 : v;
}

/** GP0 E3/E4: drawing area corner. */
static u32 get_area(u32 cmd, int x, int y)
{
    x = clamp_to(x, GENV_W);
    y = clamp_to(y, GENV_H);
    return cmd | ((y & 0x3FF) << 10) | (x & 0x3FF);
}

/** GP0 E5: drawing offset. */
static u32 get_ofs(int x, int y)
{
    return 0xE5000000 | ((y & 0x7FF) << 11) | (x & 0x7FF);
}

/** GP0 E2: texture window, from a RECT (x and y in 8-pixel units, w and h as masks). */
static u32 get_tw(RECT* tw)
{
    u32 x, y, mx, my;
    if (tw == NULL)
    {
        return 0;
    }
    x  = ((u8*)tw)[0] >> 3;
    mx = (-(s32)tw->w & 0xFF) >> 3;
    y  = ((u8*)tw)[2] >> 3;
    my = (-(s32)tw->h & 0xFF) >> 3;
    return 0xE2000000 | (x << 10) | (y << 15) | (my << 5) | mx;
}

void SetTexWindow(DR_TWIN* p, RECT* tw)
{
    LEN(p)     = 2;
    WORD(p, 1) = get_tw(tw);
    WORD(p, 2) = 0;
}

void SetDrawArea(DR_AREA* p, RECT* r)
{
    LEN(p)     = 2;
    WORD(p, 1) = get_area(0xE3000000, r->x, r->y);
    WORD(p, 2) = get_area(0xE4000000, (s16)(r->x + r->w - 1), (s16)(r->y + r->h - 1));
}

void SetDrawOffset(DR_OFFSET* p, u_short* ofs)
{
    LEN(p)     = 2;
    WORD(p, 1) = get_ofs((s16)ofs[0], (s16)ofs[1]);
    WORD(p, 2) = 0;
}

void SetPriority(void* p, int pbc, int pbw) /* (PACKET* in gpu.h; not in libgpu.h) */
{
    LEN(p)     = 2;
    WORD(p, 1) = 0xE6000000 | (pbc ? 2 : 0) | (pbw != 0);
    WORD(p, 2) = 0;
}

void SetDrawStp(DR_STP* p, int pbw)
{
    LEN(p)     = 2;
    WORD(p, 1) = 0xE6000000 | (pbw ? 1 : 0);
    WORD(p, 2) = 0;
}

void SetDrawMode(DR_MODE* p, int dfe, int dtd, int tpage, RECT* tw)
{
    LEN(p)     = 2;
    WORD(p, 1) = get_mode(dfe, dtd, tpage & 0xFFFF);
    WORD(p, 2) = get_tw(tw);
}

void SetDrawTPage(DR_TPAGE* p, int dfe, int dtd, int tpage) /* p33.o */
{
    LEN(p)     = 1;
    WORD(p, 1) = get_mode(dfe, dtd, tpage);
}

/** A VRAM copy (GP0 80h) of `rect` to (x, y); an empty packet if the rectangle is empty. */
void SetDrawMove(DR_MOVE* p, RECT* rect, int x, int y) /* p34.o */
{
    u8 len = rect->w != 0 && rect->h != 0 ? 5 : 0;
    WORD(p, 1) = 0x01000000;
    WORD(p, 2) = 0x80000000;
    LEN(p)     = len;
    WORD(p, 4) = ((u32)y << 16) | (x & 0xFFFF);
    WORD(p, 3) = WORD(rect, 0);
    WORD(p, 5) = WORD(rect, 1);
}

/** The drawing environment as a packet: area, offset, mode, window, mask, and with `isbg` a fill of
 * the area (clamped to VRAM, relative to the offset) in the background colour. */
void SetDrawEnv(DR_ENV* p, DRAWENV* env)
{
    u32 n = 7;
    WORD(p, 1) = get_area(0xE3000000, env->clip.x, env->clip.y);
    WORD(p, 2) = get_area(0xE4000000, (s16)(env->clip.x + env->clip.w - 1), (s16)(env->clip.y + env->clip.h - 1));
    WORD(p, 3) = get_ofs(env->ofs[0], env->ofs[1]);
    WORD(p, 4) = get_mode(env->dfe, env->dtd, env->tpage);
    WORD(p, 5) = get_tw(&env->tw);
    WORD(p, 6) = 0xE6000000;
    if (env->isbg)
    {
        RECT r = env->clip;
        r.w = clamp_to(r.w, GENV_W);
        r.h = clamp_to(r.h, GENV_H);
        r.x -= env->ofs[0];
        r.y -= env->ofs[1];
        WORD(p, 7) = 0x60000000 | ((u32)env->b0 << 16) | ((u32)env->g0 << 8) | env->r0;
        WORD(p, 8) = WORD(&r, 0);
        WORD(p, 9) = WORD(&r, 1);
        n          = 10;
    }
    LEN(p) = n - 1;
}

/* --- tmd.o: TIM and TMD readers ---------------------------------------------------------------------*/

extern int printf(const char* fmt, ...);

static u_long* s_TimNext; /* OpenTIM: the next TIM to read */

int OpenTIM(u_long* addr)
{
    s_TimNext = addr;
    return 0;
}

/** One TIM's header into `tim`; its length in words, or -1 if it isn't a TIM (ID 0x10). The CLUT
 * block's length is rounded down to a word, as the original adds it. */
static s32 get_tim(u32* p, TIM_IMAGE* tim)
{
    u32 clut = 0, len;
    if (*p++ != 0x10)
    {
        return -1;
    }
    tim->mode = *p++;
    if (GetGraphDebug() == 2)
    {
        printf("id  =%08x\n", 0x10);
    }
    if (GetGraphDebug() == 2)
    {
        printf("mode=%08x\n", tim->mode);
    }
    if (GetGraphDebug() == 2)
    {
        printf("timaddr=%08x\n", p);
    }
    if (tim->mode & 8)
    {
        len        = p[0];
        tim->crect = (RECT*)(p + 1);
        tim->caddr = (u_long*)(p + 3);
        clut       = len >> 2;
        p          = (u32*)((u8*)p + (clut << 2));
    }
    else
    {
        tim->crect = NULL;
        tim->caddr = NULL;
    }
    len        = p[0];
    tim->prect = (RECT*)(p + 1);
    tim->paddr = (u_long*)(p + 3);
    return clut + (len >> 2) + 2;
}

TIM_IMAGE* ReadTIM(TIM_IMAGE* timimg)
{
    s32 n = get_tim((u32*)s_TimNext, timimg);
    if (n == -1)
    {
        return NULL;
    }
    s_TimNext += n;
    return timimg;
}

/* OpenTMD's state: the open object's vertex, normal and next primitive pointers, and its primitive
 * count (tmd.o's .bss + 4 to + 16). */
static u32* s_TmdVert;
static u32* s_TmdNormal;
static u8*  s_TmdPrim;
static s32  s_TmdCount;

/** A TMD primitive type the reader knows: the header (mode, flag, ilen, olen; flag bit 1 ignored),
 * its debug name, its bytes, and the byte and halfword copies into the TMD_PRIM. Generated from the
 * original's switch (two headers per type: with and without flag bit 0). */
typedef struct
{
    u32         header;
    const char* name;
    u8          size;
    u8          count;
    const u8    (*copies)[3]; /* bytes (1 or 2), from (primitive offset), to (TMD_PRIM offset) */
} TmdCase;

static const u8 TMD_F3[][3] = { { 1, 4, 4 }, { 1, 5, 5 }, { 1, 6, 6 }, { 1, 4, 8 }, { 1, 5, 9 }, { 1, 6, 10 }, { 1, 4, 12 }, { 1, 5, 13 }, { 1, 6, 14 }, { 2, 8, 104 }, { 2, 10, 106 }, { 2, 12, 108 } };
static const u8 TMD_F3L[][3] = { { 1, 4, 4 }, { 1, 5, 5 }, { 1, 6, 6 }, { 1, 4, 8 }, { 1, 5, 9 }, { 1, 6, 10 }, { 1, 4, 12 }, { 1, 5, 13 }, { 1, 6, 14 }, { 2, 10, 104 }, { 2, 12, 106 }, { 2, 14, 108 }, { 2, 8, 112 }, { 2, 8, 114 }, { 2, 8, 116 } };
static const u8 TMD_F4[][3] = { { 1, 4, 4 }, { 1, 5, 5 }, { 1, 6, 6 }, { 1, 4, 8 }, { 1, 5, 9 }, { 1, 6, 10 }, { 1, 4, 12 }, { 1, 5, 13 }, { 1, 6, 14 }, { 1, 4, 16 }, { 1, 5, 17 }, { 1, 6, 18 }, { 2, 8, 104 }, { 2, 10, 106 }, { 2, 12, 108 }, { 2, 14, 110 } };
static const u8 TMD_F4L[][3] = { { 1, 4, 4 }, { 1, 5, 5 }, { 1, 6, 6 }, { 1, 4, 8 }, { 1, 5, 9 }, { 1, 6, 10 }, { 1, 4, 12 }, { 1, 5, 13 }, { 1, 6, 14 }, { 1, 4, 16 }, { 1, 5, 17 }, { 1, 6, 18 }, { 2, 10, 104 }, { 2, 12, 106 }, { 2, 14, 108 }, { 2, 16, 110 }, { 2, 8, 112 }, { 2, 8, 114 }, { 2, 8, 116 }, { 2, 8, 118 } };
static const u8 TMD_FT3[][3] = { { 2, 10, 20 }, { 2, 6, 22 }, { 1, 4, 24 }, { 1, 5, 25 }, { 1, 8, 26 }, { 1, 9, 27 }, { 1, 12, 28 }, { 1, 13, 29 }, { 1, 16, 4 }, { 1, 17, 5 }, { 1, 18, 6 }, { 1, 16, 8 }, { 1, 17, 9 }, { 1, 18, 10 }, { 1, 16, 12 }, { 1, 17, 13 }, { 1, 18, 14 }, { 2, 20, 104 }, { 2, 22, 106 }, { 2, 24, 108 } };
static const u8 TMD_FT3L[][3] = { { 2, 10, 20 }, { 2, 6, 22 }, { 1, 4, 24 }, { 1, 5, 25 }, { 1, 8, 26 }, { 1, 9, 27 }, { 1, 12, 28 }, { 1, 13, 29 }, { 2, 18, 104 }, { 2, 20, 106 }, { 2, 22, 108 }, { 2, 16, 112 }, { 2, 16, 114 }, { 2, 16, 116 } };
static const u8 TMD_FT4[][3] = { { 2, 10, 20 }, { 2, 6, 22 }, { 1, 4, 24 }, { 1, 5, 25 }, { 1, 8, 26 }, { 1, 9, 27 }, { 1, 12, 28 }, { 1, 13, 29 }, { 1, 16, 30 }, { 1, 17, 31 }, { 1, 20, 4 }, { 1, 21, 5 }, { 1, 22, 6 }, { 1, 20, 8 }, { 1, 21, 9 }, { 1, 22, 10 }, { 1, 20, 12 }, { 1, 21, 13 }, { 1, 22, 14 }, { 1, 20, 16 }, { 1, 21, 17 }, { 1, 22, 18 }, { 2, 24, 104 }, { 2, 26, 106 }, { 2, 28, 108 }, { 2, 30, 110 } };
static const u8 TMD_FT4L[][3] = { { 2, 10, 20 }, { 2, 6, 22 }, { 1, 4, 24 }, { 1, 5, 25 }, { 1, 8, 26 }, { 1, 9, 27 }, { 1, 12, 28 }, { 1, 13, 29 }, { 1, 16, 30 }, { 1, 17, 31 }, { 2, 22, 104 }, { 2, 24, 106 }, { 2, 26, 108 }, { 2, 28, 110 }, { 2, 20, 112 }, { 2, 20, 114 }, { 2, 20, 116 }, { 2, 20, 118 } };
static const u8 TMD_G3[][3] = { { 1, 4, 4 }, { 1, 5, 5 }, { 1, 6, 6 }, { 1, 8, 8 }, { 1, 9, 9 }, { 1, 10, 10 }, { 1, 12, 12 }, { 1, 13, 13 }, { 1, 14, 14 }, { 2, 16, 104 }, { 2, 18, 106 }, { 2, 20, 108 } };
static const u8 TMD_G3L[][3] = { { 1, 4, 4 }, { 1, 5, 5 }, { 1, 6, 6 }, { 1, 4, 8 }, { 1, 5, 9 }, { 1, 6, 10 }, { 1, 4, 12 }, { 1, 5, 13 }, { 1, 6, 14 }, { 2, 10, 104 }, { 2, 14, 106 }, { 2, 18, 108 }, { 2, 8, 112 }, { 2, 12, 114 }, { 2, 16, 116 } };
static const u8 TMD_G4[][3] = { { 1, 4, 4 }, { 1, 5, 5 }, { 1, 6, 6 }, { 1, 8, 8 }, { 1, 9, 9 }, { 1, 10, 10 }, { 1, 12, 12 }, { 1, 13, 13 }, { 1, 14, 14 }, { 1, 16, 16 }, { 1, 17, 17 }, { 1, 18, 18 }, { 2, 20, 104 }, { 2, 22, 106 }, { 2, 24, 108 }, { 2, 26, 110 } };
static const u8 TMD_G4L[][3] = { { 1, 4, 4 }, { 1, 5, 5 }, { 1, 6, 6 }, { 1, 4, 8 }, { 1, 5, 9 }, { 1, 6, 10 }, { 1, 4, 12 }, { 1, 5, 13 }, { 1, 6, 14 }, { 1, 4, 16 }, { 1, 5, 17 }, { 1, 6, 18 }, { 2, 10, 104 }, { 2, 14, 106 }, { 2, 18, 108 }, { 2, 22, 110 }, { 2, 8, 112 }, { 2, 12, 114 }, { 2, 16, 116 }, { 2, 20, 118 } };
static const u8 TMD_GT3[][3] = { { 2, 10, 20 }, { 2, 6, 22 }, { 1, 4, 24 }, { 1, 5, 25 }, { 1, 8, 26 }, { 1, 9, 27 }, { 1, 12, 28 }, { 1, 13, 29 }, { 2, 28, 104 }, { 2, 30, 106 }, { 2, 32, 108 }, { 1, 16, 4 }, { 1, 17, 5 }, { 1, 18, 6 }, { 1, 20, 8 }, { 1, 21, 9 }, { 1, 22, 10 }, { 1, 24, 12 }, { 1, 25, 13 }, { 1, 26, 14 } };
static const u8 TMD_GT3L[][3] = { { 2, 10, 20 }, { 2, 6, 22 }, { 1, 4, 24 }, { 1, 5, 25 }, { 1, 8, 26 }, { 1, 9, 27 }, { 1, 12, 28 }, { 1, 13, 29 }, { 2, 18, 104 }, { 2, 22, 106 }, { 2, 26, 108 }, { 2, 16, 112 }, { 2, 20, 114 }, { 2, 24, 116 } };
static const u8 TMD_GT4[][3] = { { 2, 10, 20 }, { 2, 6, 22 }, { 1, 4, 24 }, { 1, 5, 25 }, { 1, 8, 26 }, { 1, 9, 27 }, { 1, 12, 28 }, { 1, 13, 29 }, { 1, 16, 30 }, { 1, 17, 31 }, { 2, 36, 104 }, { 2, 38, 106 }, { 2, 40, 108 }, { 2, 42, 110 }, { 1, 20, 4 }, { 1, 21, 5 }, { 1, 22, 6 }, { 1, 24, 8 }, { 1, 25, 9 }, { 1, 26, 10 }, { 1, 28, 12 }, { 1, 29, 13 }, { 1, 30, 14 }, { 1, 32, 16 }, { 1, 33, 17 }, { 1, 34, 18 } };
static const u8 TMD_GT4L[][3] = { { 2, 10, 20 }, { 2, 6, 22 }, { 1, 4, 24 }, { 1, 5, 25 }, { 1, 8, 26 }, { 1, 9, 27 }, { 1, 12, 28 }, { 1, 13, 29 }, { 1, 16, 30 }, { 1, 17, 31 }, { 2, 22, 104 }, { 2, 26, 106 }, { 2, 30, 108 }, { 2, 34, 110 }, { 2, 20, 112 }, { 2, 24, 114 }, { 2, 28, 116 }, { 2, 32, 118 } };

static const TmdCase TMD_CASES[32] = {
    { 0x20000304, "F3L ", 16, 15, TMD_F3L },
    { 0x20020304, "F3L ", 16, 15, TMD_F3L },
    { 0x21010304, "F3 ", 16, 12, TMD_F3 },
    { 0x21030304, "F3 ", 16, 12, TMD_F3 },
    { 0x24000507, "FT3L ", 24, 14, TMD_FT3L },
    { 0x24020507, "FT3L ", 24, 14, TMD_FT3L },
    { 0x25010607, "FT3 ", 28, 20, TMD_FT3 },
    { 0x25030607, "FT3 ", 28, 20, TMD_FT3 },
    { 0x28000405, "F4L ", 20, 20, TMD_F4L },
    { 0x28020405, "F4L ", 20, 20, TMD_F4L },
    { 0x29010305, "F4 ", 16, 16, TMD_F4 },
    { 0x29030305, "F4 ", 16, 16, TMD_F4 },
    { 0x2C000709, "FT4L ", 32, 18, TMD_FT4L },
    { 0x2C020709, "FT4L ", 32, 18, TMD_FT4L },
    { 0x2D010709, "FT4 ", 32, 26, TMD_FT4 },
    { 0x2D030709, "FT4 ", 32, 26, TMD_FT4 },
    { 0x30000406, "G3L ", 20, 15, TMD_G3L },
    { 0x30020406, "G3L ", 20, 15, TMD_G3L },
    { 0x31010506, "G3 ", 24, 12, TMD_G3 },
    { 0x31030506, "G3 ", 24, 12, TMD_G3 },
    { 0x34000609, "GT3L ", 28, 14, TMD_GT3L },
    { 0x34020609, "GT3L ", 28, 14, TMD_GT3L },
    { 0x35010809, "GT3 ", 36, 20, TMD_GT3 },
    { 0x35030809, "GT3 ", 36, 20, TMD_GT3 },
    { 0x38000508, "G4L ", 24, 20, TMD_G4L },
    { 0x38020508, "G4L ", 24, 20, TMD_G4L },
    { 0x39010608, "G4 ", 28, 16, TMD_G4 },
    { 0x39030608, "G4 ", 28, 16, TMD_G4 },
    { 0x3C00080C, "GT4L ", 36, 18, TMD_GT4L },
    { 0x3C02080C, "GT4L ", 36, 18, TMD_GT4L },
    { 0x3D010A0C, "GT4 ", 44, 26, TMD_GT4 },
    { 0x3D030A0C, "GT4 ", 44, 26, TMD_GT4 },
};

/** One primitive into `out` (cleared first); its bytes, or -1 for a type the reader doesn't know. */
static s32 get_tmd_prim(const u8* prim, u8* out)
{
    u32 h, i;
    memset(out, 0, 120);
    *(u32*)out = *(const u32*)prim;
    h          = *(const u32*)prim & 0xFDFFFFFF;
    for (i = 0; i < sizeof(TMD_CASES) / sizeof(TMD_CASES[0]); i++)
    {
        const TmdCase* c = &TMD_CASES[i];
        u32            k;
        if (c->header != h)
        {
            continue;
        }
        if (GetGraphDebug() == 2)
        {
            printf(c->name);
        }
        for (k = 0; k < c->count; k++)
        {
            const u8* x = c->copies[k];
            if (x[0] == 1)
            {
                out[x[2]] = prim[x[1]];
            }
            else
            {
                *(u16*)(out + x[2]) = *(const u16*)(prim + x[1]);
            }
        }
        return c->size;
    }
    printf("unsupported type (%08x)\n", *(u32*)out & 0xFDFFFFFF);
    return -1;
}

/** Object `obj_no` of a TMD: its pointers to OpenTMD's state; returns its primitive count. */
static s32 open_tmd(u32* tmd, s32 obj_no, u8** prim, u32** vert, u32** normal)
{
    u8*  base = (u8*)(tmd + 3);
    u32* o    = (u32*)(base + obj_no * 28);
    if (GetGraphDebug() == 2)
    {
        printf("analizing TMD...\n");
    }
    if (GetGraphDebug() == 2)
    {
        printf("\tid=%08X, flags=%d, nobj=%d, objid=%d\n", tmd[0], tmd[1], tmd[2], obj_no);
    }
    if (GetGraphDebug() == 2)
    {
        printf("\tvert=%08X, nvert=%d\n", o[0], o[1]);
    }
    if (GetGraphDebug() == 2)
    {
        printf("\tnorm=%08X, nnorm=%d\n", o[2], o[3]);
    }
    if (GetGraphDebug() == 2)
    {
        printf("\tprim=%08X, nprim=%d\n", o[4], o[5]);
    }
    *vert   = (u32*)(base + o[0]);
    *normal = (u32*)(base + o[2]);
    *prim   = base + o[4];
    return o[5];
}

int OpenTMD(u_long* tmd, int obj_no)
{
    s_TmdCount = open_tmd((u32*)tmd, obj_no, &s_TmdPrim, &s_TmdVert, &s_TmdNormal);
    return s_TmdCount;
}

/** The next primitive, with copies of its normals (TMD_PRIM + 64) and vertices (+ 32) by index. */
TMD_PRIM* ReadTMD(TMD_PRIM* tmdprim)
{
    u8* out = (u8*)tmdprim;
    s32 n   = get_tmd_prim(s_TmdPrim, out);
    int i, k;
    if (n < 0)
    {
        return NULL;
    }
    *(u32**)(out + 96)  = s_TmdVert;
    *(u32**)(out + 100) = s_TmdNormal;
    for (i = 0; i < 4; i++)
    {
        const u16* nv = (const u16*)((u8*)s_TmdNormal + *(u16*)(out + 112 + i * 2) * 8);
        for (k = 0; k < 3; k++)
        {
            *(u16*)(out + 64 + i * 8 + k * 2) = nv[k];
        }
    }
    s_TmdPrim += n;
    for (i = 0; i < 4; i++)
    {
        const u16* vv = (const u16*)((u8*)s_TmdVert + *(u16*)(out + 104 + i * 2) * 8);
        for (k = 0; k < 3; k++)
        {
            *(u16*)(out + 32 + i * 8 + k * 2) = vv[k];
        }
    }
    return tmdprim;
}
