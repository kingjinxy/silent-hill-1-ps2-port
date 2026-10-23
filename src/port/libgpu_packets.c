/** @brief Sony's libgpu packet builders in C for the port, from the library objects (lib/libgpu: the
 * primitive objects and the builders in sys.o). They only fill memory; the GPU system layer is
 * libgpu_port.c. Each stores what the original does, in its order. tools/port/lib_test compares them
 * with the originals.
 */

#include "common.h"
#include <psyq/libgte.h>
#include <psyq/libgpu.h>

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

/* --- tmd.o: TIM headers (OpenTMD/ReadTMD are still recompiled) ---------------------------------------*/

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
