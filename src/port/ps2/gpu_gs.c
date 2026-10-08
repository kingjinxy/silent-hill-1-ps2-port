/** @brief Hardware PS1 GPU on the GS: executes GP0 command packets with GS primitives, drawing into
 * an image of the PS1's VRAM kept in GS local memory.
 *
 * GS memory: the PS1's 1024x512 16-bit VRAM is a PSMCT16 buffer at VRAM_ADDR (top of the 4 MiB, so
 * display mode changes, which re-run gsKit's setup, don't move it), drawn to directly as the frame
 * buffer. PSMCT16 has the PS1's pixel layout (5:5:5, bit 15 = alpha MSB = PS1 mask bit). Below it,
 * CACHE_ADDR holds 4/8-bit texture pages; gsKit's display frame buffer is at the bottom.
 *
 * Mapping (written from psx-spx and the GS User's Manual; gpu_soft.c is the reference):
 *  - Textures: 15-bit pages are read straight from the VRAM buffer (so render-to-texture works).
 *    4/8-bit pages are uploaded as PSMT4/PSMT8 from the EE copy of VRAM (g_PortVram, kept up to date
 *    by CPU uploads), which packs indices the same way, and cached per page. Their CLUTs are copied
 *    from g_PortVram to a CLUT cache (one row each, loaded with CSM2): loading CLUTs from the VRAM
 *    buffer itself (the render target) made PCSX2's hardware renderer read the target back for each of
 *    the ~330 CLUT loads per frame (99% host GPU in-game). GP0 fills and VRAM copies also update
 *    g_PortVram, so it holds everything but what polygons draw.
 *  - Texel 0x0000 is transparent: TEXA AEM gives it alpha 0, dropped by the alpha test. Other texels
 *    get alpha TA0 = 0x40 (bit 15 clear) or TA1 = 0x80 (bit 15 set), so PABE blends only texels with
 *    bit 15 set (PS1 semi-transparency) and the frame buffer write stores bit 15 from the texel.
 *  - Modulation: GS MODULATE is (texel * colour) >> 7 like the PS1 (0x80 = 1.0); raw texture = DECAL.
 *  - Semi-transparency modes 0-3: ALPHA (A - B) * FIX / 128 + D with FIX 0x40/0x80/0x80/0x20, colour
 *    clamping on.
 *  - Mask bit: set = FBA, check = destination alpha test (DATE).
 *  - Dithering: DTHE with the PS1's matrix in DIMX, for gouraud and modulated textured polygons.
 *  - Texture window: CLAMP REGION_REPEAT, the same (u & mask) | offset formula.
 *  - Clip area: SCISSOR. Coordinates are offset by 2048 (XYOFFSET) so negative ones fit.
 * Known differences: interpolation and blending rounding, line rasterisation, rectangle flips.
 */

#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <dmaKit.h>
#include "port/prof.h"

typedef unsigned long long u64x;

#define VRAM_W 1024
#define VRAM_H 512

#define VRAM_ADDR  0x300000                /* bytes */
#define VRAM_TBP   (VRAM_ADDR / 256)        /* block address (64 words) */
#define VRAM_FBP   (VRAM_ADDR / 8192)       /* page address (2048 words) */
#define VRAM_BW    (VRAM_W / 64)
#define CACHE_ADDR 0x200000
#define CACHE_SLOT 0x10000
#define CACHE_SLOTS 28 /* 15 at CACHE_ADDR, 13 more from CACHE2_ADDR */
#define CACHE2_ADDR 0x130000 /* above gsKit's largest display buffer (640x480x4 = 0x12C000) */
/* CLUT cache: one 256-pixel PSMCT16 row per CLUT (256x128, the last 64 KiB of the page cache area). */
#define CLUT_ADDR  0x2F0000
#define CLUT_ROWS  128

#define PSM_CT32 0x00
#define PSM_CT16 0x02
#define PSM_T8   0x13
#define PSM_T4   0x14

/* GS registers (A+D addresses). */
enum
{
    R_PRIM = 0x00, R_RGBAQ = 0x01, R_UV = 0x03, R_XYZ2 = 0x05, R_TEX0 = 0x06, R_CLAMP = 0x08,
    R_TEX1 = 0x14, R_XYOFFSET = 0x18, R_PRMODECONT = 0x1A, R_TEXCLUT = 0x1C, R_TEXA = 0x3B,
    R_TEXFLUSH = 0x3F, R_SCISSOR = 0x40, R_ALPHA = 0x42, R_DIMX = 0x44, R_DTHE = 0x45,
    R_COLCLAMP = 0x46, R_TEST = 0x47, R_PABE = 0x49, R_FBA = 0x4A, R_FRAME = 0x4C, R_ZBUF = 0x4E,
    R_BITBLTBUF = 0x50, R_TRXPOS = 0x51, R_TRXREG = 0x52, R_TRXDIR = 0x53, R_FINISH = 0x61,
    REG_COUNT = 0x62
};

extern u16  g_PortVram[VRAM_H][VRAM_W];
extern s32  g_PortGsRenderer; /* libgpu_port.c: 1 = GS only (g_PortVram is then this file's copy) */
extern void Display_EnsureInit(void); /* display_ps2.c */

/* --- GIF packets ------------------------------------------------------------------------------*/

/* The packet is a sequence of GIF tags: A+D tags for register writes, and REGLIST tags for runs of
 * primitives of the same shape (PRIM and the vertex registers listed once in the tag, then 64 bits
 * per register: half the data of A+D). */
#define PKT_WORDS (8192 * 2) /* 64-bit words */
static u64x s_Pkt[PKT_WORDS] __attribute__((aligned(64)));
static int  s_Pos;            /* words used */
static int  s_TagAt = -1;     /* open tag (word index), -1 = none */
static u64x s_TagRegs;        /* REGLIST register list of the open tag, 0 = A+D */
static int  s_TagNreg;        /* registers per loop */
static int  s_TagLoops;
static int  s_LastTag = -1;   /* for EOP */

static u64x s_Reg[REG_COUNT];
/* Incremented whenever GS state may have changed outside prim_state() (reset, fills, display
 * copies, texture cache changes): its memo of the last primitive's state is then stale. */
static u32  s_Epoch;
/* Per-frame statistics (GpuGs_Stats): primitives, texture (TEX0) changes, CLUT loads, primitives
 * with destination alpha test (check mask), primitives sampling the VRAM buffer (15-bit pages). */
static u32 s_StPrims, s_StTex0, s_StClut, s_StDate, s_StRtTex, s_StUploads;
static u8   s_RegValid[REG_COUNT];

static u64x giftag(u32 nloop, u32 eop, u32 flg, u32 nreg)
{
    return (u64x)nloop | ((u64x)eop << 15) | ((u64x)flg << 58) | ((u64x)(nreg & 15) << 60);
}

static void dma_send_impl(void* data, u32 qwc);
static void dma_send(void* data, u32 qwc)
{
    PROF_BEGIN("gs: dma_send")
    dma_send_impl(data, qwc);
    PROF_END("gs: dma_send")
}

static void dma_send_impl(void* data, u32 qwc)
{
    FlushCache(0);
    dmaKit_send(DMA_CHANNEL_GIF, data, qwc);
    dmaKit_wait(DMA_CHANNEL_GIF, 0);
}

/** Finishes the open tag: loop count, and padding to a whole qword for REGLIST. */
static void tag_close(void)
{
    if (s_TagAt < 0)
    {
        return;
    }
    if (s_TagRegs)
    {
        s_Pkt[s_TagAt]     = giftag((u32)s_TagLoops, 0, 1, (u32)s_TagNreg);
        s_Pkt[s_TagAt + 1] = s_TagRegs;
        if ((s_TagLoops * s_TagNreg) & 1)
        {
            s_Pkt[s_Pos++] = 0;
        }
    }
    else
    {
        s_Pkt[s_TagAt]     = giftag((u32)s_TagLoops, 0, 0, 1);
        s_Pkt[s_TagAt + 1] = 0xE; /* A+D */
    }
    s_LastTag = s_TagAt;
    s_TagAt   = -1;
}

/** Sends the queued packet. */
void GpuGs_Flush(void) __attribute__((noinline));
void GpuGs_Flush(void)
{
    tag_close();
    if (s_Pos == 0)
    {
        return;
    }
    s_Pkt[s_LastTag] |= 1 << 15; /* EOP */
    dma_send(s_Pkt, (u32)(s_Pos / 2));
    s_Pos     = 0;
    s_LastTag = -1;
}

/** Room for `words` more data words (plus a tag and padding) in the packet. */
static inline void room(int words)
{
    if (s_Pos + words + 4 > PKT_WORDS)
    {
        GpuGs_Flush();
    }
}

static inline void ad(u32 reg, u64x val)
{
    room(2);
    if (s_TagAt < 0 || s_TagRegs)
    {
        tag_close();
        s_TagAt    = s_Pos;
        s_TagRegs  = 0;
        s_TagLoops = 0;
        s_Pos += 2;
    }
    s_Pkt[s_Pos]     = val;
    s_Pkt[s_Pos + 1] = reg;
    s_Pos += 2;
    s_TagLoops++;
}

/** Space for one loop of a REGLIST run with these registers (4 bits each, `nreg` of them). */
static inline u64x* reglist(u64x regs, int nreg)
{
    u64x* q;
    room(nreg);
    if (s_TagAt < 0 || s_TagRegs != regs || s_TagLoops >= 0x7FFF)
    {
        tag_close();
        s_TagAt    = s_Pos;
        s_TagRegs  = regs;
        s_TagNreg  = nreg;
        s_TagLoops = 0;
        s_Pos += 2;
    }
    q = &s_Pkt[s_Pos];
    s_Pos += nreg;
    s_TagLoops++;
    return q;
}

/** State register write, skipped when unchanged. */
static inline void set(u32 reg, u64x val)
{
    if (s_RegValid[reg] && s_Reg[reg] == val)
    {
        return;
    }
    s_Reg[reg]      = val;
    s_RegValid[reg] = 1;
    ad(reg, val);
}

/** The GS was reset (display mode change) or its state changed behind our back. */
void GpuGs_StateLost(void)
{
    s_Epoch++;
    memset(s_RegValid, 0, sizeof(s_RegValid));
}

/* --- Drawing state (GP0 E1-E6), as in gpu_soft.c ----------------------------------------------*/

static u32 s_TexPage;
static u32 s_TwMaskX, s_TwMaskY, s_TwOffX, s_TwOffY;
static s32 s_AreaX1, s_AreaY1, s_AreaX2 = VRAM_W - 1, s_AreaY2 = VRAM_H - 1;
static s32 s_OffX, s_OffY;
static u32 s_SetMask, s_CheckMask;

typedef struct
{
    s32 x, y;
    u32 r, g, b;
    u32 u, v;
} Vtx;

typedef struct
{
    u32 textured, raw, semi, gouraud, dither;
    u32 tpage, clut, semiMode;
} Prim;

static s32 sext11(u32 v)
{
    return ((s32)(v << 21)) >> 21;
}

/* --- Texture page cache (4/8-bit pages as PSMT4/PSMT8) ----------------------------------------*/

typedef struct
{
    s32 key; /* (depth << 5) | page index, -1 = free */
    u32 used;
} Slot;

static Slot s_Slots[CACHE_SLOTS];

/** GS block address of page cache slot i. */
static u32 slot_tbp(s32 i)
{
    return (u32)(i < 15 ? CACHE_ADDR + i * CACHE_SLOT : CACHE2_ADDR + (i - 15) * CACHE_SLOT) / 256;
}

typedef struct
{
    s32 key; /* CLUT attribute | depth << 16, -1 = free */
    u32 used;
} ClutSlot;

static ClutSlot s_Cluts[CLUT_ROWS];

/* Direct lookups (slot or row + 1, 0 = not cached): page cache by key, CLUT cache by
 * CLUT attribute | depth << 15. */
static u8 s_PageMap[64];
static u8 s_ClutMap[0x10000];
#define CLUT_MAP(key) (((key) & 0x7FFF) | (((key) >> 16) << 15))

static u32  s_UseClock;
static u8   s_Staging[256 * 256] __attribute__((aligned(64)));
static u64x s_ImgPkt[(256 * 256 / 16 + 8) * 2] __attribute__((aligned(64)));

static void page_rect(s32 key, s32* x, s32* y, s32* w)
{
    s32 page = key & 31;
    *x       = (page & 15) * 64;
    *y       = (page >> 4) * 256;
    *w       = (key >> 5) == 0 ? 64 : 128;
}

/** Drops cached pages overlapping a VRAM rectangle that changed. */
static void cache_invalidate(s32 x, s32 y, s32 w, s32 h)
{
    s32 i;
    s_Epoch++;
    for (i = 0; i < CACHE_SLOTS; i++)
    {
        s32 px, py, pw;
        if (s_Slots[i].key < 0)
        {
            continue;
        }
        page_rect(s_Slots[i].key, &px, &py, &pw);
        if (x < px + pw && px < x + w && y < py + 256 && py < y + h)
        {
            s_PageMap[s_Slots[i].key] = 0;
            s_Slots[i].key            = -1;
        }
    }
    for (i = 0; i < CLUT_ROWS; i++)
    {
        s32 cx, cy, cw;
        if (s_Cluts[i].key < 0)
        {
            continue;
        }
        cx = (s_Cluts[i].key & 0x3F) * 16;
        cy = (s_Cluts[i].key >> 6) & 0x1FF;
        cw = (s_Cluts[i].key >> 16) == 0 ? 16 : 256;
        if (x < cx + cw && cx < x + w && y <= cy && cy < y + h)
        {
            s_ClutMap[CLUT_MAP(s_Cluts[i].key)] = 0;
            s_Cluts[i].key                      = -1;
        }
    }
}

/** Host-to-local image transfer of `bytes` (a multiple of 16) from `src`. */
/** Host-to-local transfer of the `bytes` (a multiple of 16) already placed at &s_ImgPkt[2]. */
static void upload_staged(u32 dbp, u32 dbw, u32 psm, s32 x, s32 y, s32 w, s32 h, u32 bytes)
{
    u32 qwc = bytes / 16;
    s_StUploads++;
    GpuGs_Flush();
    ad(R_BITBLTBUF, ((u64x)dbp << 32) | ((u64x)dbw << 48) | ((u64x)psm << 56));
    ad(R_TRXPOS, ((u64x)x << 32) | ((u64x)y << 48));
    ad(R_TRXREG, (u64x)w | ((u64x)h << 32));
    ad(R_TRXDIR, 0);
    GpuGs_Flush();
    s_ImgPkt[0] = giftag(qwc, 1, 2, 0);
    s_ImgPkt[1] = 0;
    dma_send(s_ImgPkt, qwc + 1);
    ad(R_TEXFLUSH, 0);
}

/** Host-to-local image transfer of `bytes` (a multiple of 16) from `src`. */
static void upload(u32 dbp, u32 dbw, u32 psm, s32 x, s32 y, s32 w, s32 h, const void* src, u32 bytes)
{
    memcpy(&s_ImgPkt[2], src, bytes);
    upload_staged(dbp, dbw, psm, x, y, w, h, bytes);
}

static u32 cache_get(u32 depth, u32 tpage)
{
    s32 key = (s32)((depth << 5) | (tpage & 31));
    s32 i, best = -1, x, y, w, row;
    if (s_PageMap[key])
    {
        i               = s_PageMap[key] - 1;
        s_Slots[i].used = ++s_UseClock;
        return slot_tbp(i);
    }
    /* A free slot, else the least recently used one. */
    for (i = 0; i < CACHE_SLOTS; i++)
    {
        if (s_Slots[i].key < 0)
        {
            best = i;
            break;
        }
        if (best < 0 || s_Slots[i].used < s_Slots[best].used)
        {
            best = i;
        }
    }
    /* Upload the page's indices: 256 rows of 64 (4-bit) or 128 (8-bit) VRAM words. */
    /* Rows straight into the transfer packet (whole rows with memcpy unless the page wraps). */
    page_rect(key, &x, &y, &w);
    for (row = 0; row < 256; row++)
    {
        u16* dst = (u16*)&s_ImgPkt[2] + row * w;
        const u16* src = &g_PortVram[(y + row) & (VRAM_H - 1)][0];
        if (x + w <= VRAM_W)
        {
            memcpy(dst, src + x, (u32)w * 2);
        }
        else
        {
            s32 col;
            for (col = 0; col < w; col++)
            {
                dst[col] = src[(x + col) & (VRAM_W - 1)];
            }
        }
    }
    upload_staged(slot_tbp(best), 4, depth == 0 ? PSM_T4 : PSM_T8, 0, 0, 256, 256, (u32)(w * 2 * 256));
    if (s_Slots[best].key >= 0)
    {
        s_PageMap[s_Slots[best].key] = 0;
    }
    s_Slots[best].key  = key;
    s_Slots[best].used = ++s_UseClock;
    s_PageMap[key]     = (u8)(best + 1);
    return slot_tbp(best);
}

/** Row of the CLUT cache holding `clut` (PS1 CLUT attribute) for a 4- or 8-bit page. */
static u32 clut_get(u32 clut, u32 depth)
{
    s32 key = (s32)((clut & 0x7FFF) | (depth << 16));
    s32 i, best = -1, x, y, n;
    u16 row[256] __attribute__((aligned(16)));
    if (s_ClutMap[CLUT_MAP(key)])
    {
        i               = s_ClutMap[CLUT_MAP(key)] - 1;
        s_Cluts[i].used = ++s_UseClock;
        return (u32)i;
    }
    for (i = 0; i < CLUT_ROWS; i++)
    {
        if (s_Cluts[i].key < 0)
        {
            best = i;
            break;
        }
        if (best < 0 || s_Cluts[i].used < s_Cluts[best].used)
        {
            best = i;
        }
    }
    x = (s32)(clut & 0x3F) * 16;
    y = (s32)((clut >> 6) & 0x1FF);
    n = depth == 0 ? 16 : 256;
    for (i = 0; i < n; i++)
    {
        row[i] = g_PortVram[y][(x + i) & (VRAM_W - 1)];
    }
    upload(CLUT_ADDR / 256, 4, PSM_CT16, 0, best, n, 1, row, (u32)(n * 2));
    if (s_Cluts[best].key >= 0)
    {
        s_ClutMap[CLUT_MAP(s_Cluts[best].key)] = 0;
    }
    s_Cluts[best].key            = key;
    s_Cluts[best].used           = ++s_UseClock;
    s_ClutMap[CLUT_MAP(key)]     = (u8)(best + 1);
    s_RegValid[R_TEX0] = 0; /* this row's contents changed: load it again */
    return (u32)best;
}

/* --- Setup ------------------------------------------------------------------------------------*/

static int s_Ready;

static void init(void)
{
    s32 i;
    if (s_Ready)
    {
        return;
    }
    Display_EnsureInit();
    for (i = 0; i < CACHE_SLOTS; i++)
    {
        s_Slots[i].key = -1;
    }
    for (i = 0; i < CLUT_ROWS; i++)
    {
        s_Cluts[i].key = -1;
    }
    memset(s_PageMap, 0, sizeof(s_PageMap));
    memset(s_ClutMap, 0, sizeof(s_ClutMap));
    s_Ready = 1;
}

static u64x dimx(void)
{
    static u64x cached;
    static int  ready;
    if (ready)
    {
        return cached;
    }
    ready = 1;
    static const s32 M[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };
    u64x v = 0;
    s32  i, j;
    for (i = 0; i < 4; i++)
    {
        for (j = 0; j < 4; j++)
        {
            v |= (u64x)(M[i][j] & 7) << (i * 16 + j * 4);
        }
    }
    cached = v;
    return v;
}

/** Fixed per-frame-buffer state: drawing into the VRAM buffer. */
static void base_state(void)
{
    /* Unchanged since the last call unless something else rewrote GS state (s_Epoch). */
    static u32 doneEpoch = ~0u;
    if (doneEpoch == s_Epoch && s_RegValid[R_FRAME])
    {
        return;
    }
    doneEpoch = s_Epoch;
    set(R_FRAME, (u64x)VRAM_FBP | ((u64x)VRAM_BW << 16) | ((u64x)PSM_CT16 << 24));
    set(R_ZBUF, (u64x)(CACHE_ADDR / 8192) | (1ULL << 32)); /* Z writes masked, never tested */
    set(R_XYOFFSET, (u64x)(2048 << 4) | ((u64x)(2048 << 4) << 32));
    set(R_PRMODECONT, 1);
    set(R_COLCLAMP, 1);
    set(R_DIMX, dimx());
    set(R_TEXA, 0x40 | (1 << 15) | ((u64x)0x80 << 32));
    set(R_TEX1, 0);
}

static void scissor(s32 x1, s32 y1, s32 x2, s32 y2)
{
    if (x2 < x1 || y2 < y1)
    {
        x1 = y1 = 1;
        x2 = y2 = 0; /* empty: the GS draws nothing when max < min */
    }
    set(R_SCISSOR, (u64x)x1 | ((u64x)x2 << 16) | ((u64x)y1 << 32) | ((u64x)y2 << 48));
}

/** Per-primitive state; returns the PRIM register bits other than the type. */
static u64x prim_state_impl(const Prim* p);
/** GS state for a primitive; consecutive primitives with the same inputs (the usual case) reuse
 * the previous result instead of going through every register again. */
static u64x prim_state(const Prim* p)
{
    static u32  memoEpoch = ~0u, memoKey[4];
    static u64x memoPrim;
    u32         key[4];
    u64x        r;

    key[0] = p->textured | (p->raw << 1) | (p->semi << 2) | (p->semiMode << 3) | (p->gouraud << 5) |
             (p->dither << 6) | (s_SetMask << 7) | (s_CheckMask << 8) | ((p->tpage & 0x1FF) << 9) |
             (s_TwMaskX << 18) | (s_TwMaskY << 23);
    key[1] = (p->clut & 0xFFFF) | (s_TwOffX << 16) | (s_TwOffY << 21);
    key[2] = (u32)s_AreaX1 | ((u32)s_AreaY1 << 10) | ((u32)s_AreaX2 << 20);
    key[3] = (u32)s_AreaY2;
    if (memoEpoch == s_Epoch && key[0] == memoKey[0] && key[1] == memoKey[1] && key[2] == memoKey[2] &&
        key[3] == memoKey[3])
    {
        return memoPrim;
    }
    PROF_BEGIN("gs: prim_state")
    r = prim_state_impl(p);
    PROF_END("gs: prim_state")
    memoEpoch  = s_Epoch;
    memoKey[0] = key[0];
    memoKey[1] = key[1];
    memoKey[2] = key[2];
    memoKey[3] = key[3];
    memoPrim   = r;
    return r;
}

static u64x prim_state_impl(const Prim* p)
{
    u64x prim = (u64x)(p->gouraud << 3) | (1 << 8); /* IIP, FST (UV coordinates) */
    u32  test = (1 << 16) | (1 << 17);            /* ZTE on, ZTST always */

    base_state();
    scissor(s_AreaX1, s_AreaY1, s_AreaX2, s_AreaY2);

    if (p->textured)
    {
        u32 depth = (p->tpage >> 7) & 3;
        u64x tex0;
        prim |= 1 << 4; /* TME */
        if (depth >= 2)
        {
            u32 px = (p->tpage & 15) * 64, py = ((p->tpage >> 4) & 1) * 256;
            tex0   = (u64x)(VRAM_TBP + ((py / 64) * VRAM_BW + px / 64) * 32) | ((u64x)VRAM_BW << 14) |
                   ((u64x)PSM_CT16 << 20);
        }
        else
        {
            u32 tbp = cache_get(depth, p->tpage);
            u64x texclut = 4 | ((u64x)clut_get(p->clut, depth) << 12); /* CBW 4 (256), COU 0, COV = row */
            if (!s_RegValid[R_TEXCLUT] || s_Reg[R_TEXCLUT] != texclut)
            {
                s_RegValid[R_TEX0] = 0; /* the CLUT is loaded when TEX0 is written */
            }
            set(R_TEXCLUT, texclut);
            tex0 = (u64x)tbp | ((u64x)4 << 14) | ((u64x)(depth == 0 ? PSM_T4 : PSM_T8) << 20) |
                   ((u64x)(CLUT_ADDR / 256) << 37) | ((u64x)PSM_CT16 << 51) | (1ULL << 55) | (1ULL << 61); /* CSM2, CLD */
        }
        tex0 |= (8ULL << 26) | (8ULL << 30) | (1ULL << 34) | ((u64x)(p->raw ? 1 : 0) << 35); /* 256x256, TCC, TFX */
        if (!s_RegValid[R_TEX0] || s_Reg[R_TEX0] != tex0)
        {
            ad(R_TEXFLUSH, 0);
            s_StTex0++;
            if (depth < 2)
            {
                s_StClut++;
            }
        }
        set(R_TEX0, tex0);
        set(R_CLAMP, 3 | (3 << 2) | ((u64x)(0xFF & ~(s_TwMaskX * 8)) << 4) | ((u64x)((s_TwOffX & s_TwMaskX) * 8) << 14) |
                         ((u64x)(0xFF & ~(s_TwMaskY * 8)) << 24) | ((u64x)((s_TwOffY & s_TwMaskY) * 8) << 34));
        test |= 1 | (7 << 1); /* alpha test: drop alpha 0 (texel 0x0000) */
    }
    if (s_CheckMask)
    {
        test |= 1 << 14; /* DATE: only where the destination's bit 15 is clear */
    }
    set(R_TEST, test);
    set(R_FBA, s_SetMask);
    set(R_DTHE, p->dither);

    if (p->semi)
    {
        static const u64x MODES[4] = {
            0 | (1 << 2) | (2 << 4) | (1 << 6) | (0x40ULL << 32), /* (Cs - Cd) * 0.5 + Cd */
            0 | (2 << 2) | (2 << 4) | (1 << 6) | (0x80ULL << 32), /* Cs + Cd */
            1 | (0 << 2) | (2 << 4) | (2 << 6) | (0x80ULL << 32), /* Cd - Cs */
            0 | (2 << 2) | (2 << 4) | (1 << 6) | (0x20ULL << 32), /* Cs * 0.25 + Cd */
        };
        prim |= 1 << 6; /* ABE */
        set(R_ALPHA, MODES[p->semiMode]);
        set(R_PABE, p->textured);
    }
    return prim;
}

/* REGLIST register lists (4 bits per register, first in the low bits): PRIM, then per vertex
 * RGBAQ (1), [UV (3)], XYZ2 (5). */
static u64x vertex_regs(int textured, int n, int* nreg)
{
    u64x regs = 0; /* PRIM = 0 */
    int  i, k = 1;
    for (i = 0; i < n; i++)
    {
        regs |= (u64x)1 << (k++ * 4);
        if (textured)
        {
            regs |= (u64x)3 << (k++ * 4);
        }
        regs |= (u64x)5 << (k++ * 4);
    }
    *nreg = k;
    return regs;
}

/** A primitive (PRIM + n vertices) as one REGLIST loop. */
static void emit(const Prim* p, u64x prim, const Vtx* const* v, int n)
{
    static u64x regsTab[2][5];
    static int  nregTab[2][5];
    u64x*       q;
    u32         a = p->textured ? 0x80 : 0;
    int         i;
    if (!nregTab[p->textured][n])
    {
        regsTab[p->textured][n] = vertex_regs(p->textured, n, &nregTab[p->textured][n]);
    }
    s_StPrims++;
    s_StDate += s_CheckMask;
    s_StRtTex += p->textured && ((p->tpage >> 7) & 3) >= 2;
    q    = reglist(regsTab[p->textured][n], nregTab[p->textured][n]);
    *q++ = prim;
    for (i = 0; i < n; i++)
    {
        *q++ = v[i]->r | (v[i]->g << 8) | (v[i]->b << 16) | (a << 24) | (0x3F800000ULL << 32);
        if (p->textured)
        {
            *q++ = (u64x)(v[i]->u << 4) | ((u64x)(v[i]->v << 4) << 16);
        }
        *q++ = (u64x)((v[i]->x + 2048) << 4 & 0xFFFF) | ((u64x)((v[i]->y + 2048) << 4 & 0xFFFF) << 16);
    }
}

static s32 absd(s32 a, s32 b)
{
    return a > b ? a - b : b - a;
}

/* --- Commands ---------------------------------------------------------------------------------*/

static void read_xy(u32 w, Vtx* v)
{
    v->x = sext11(w) + s_OffX;
    v->y = sext11(w >> 16) + s_OffY;
}

static void read_rgb(u32 w, Vtx* v)
{
    v->r = w & 0xFF;
    v->g = (w >> 8) & 0xFF;
    v->b = (w >> 16) & 0xFF;
}

/** Flat, undithered colours to 5 bits: the PS1 blends with the colour already in frame buffer
 * precision, the GS in 8 bits (without dithering, the stored result is the same either way). */
static void quantize(Vtx* v, s32 n)
{
    s32 i;
    for (i = 0; i < n; i++)
    {
        v[i].r &= 0xF8;
        v[i].g &= 0xF8;
        v[i].b &= 0xF8;
    }
}

/** One REGLIST loop of PRIM and `nv` vertices from packed register values. */
static void emit_packed(int textured, u64x prim, const u64x* rgbaq, const u64x* uv, const u64x* xyz, int nv)
{
    static u64x regsTab[2][5];
    static int  nregTab[2][5];
    u64x*       q;
    int         i;
    if (!nregTab[textured][nv])
    {
        regsTab[textured][nv] = vertex_regs(textured, nv, &nregTab[textured][nv]);
    }
    s_StPrims++;
    q    = reglist(regsTab[textured][nv], nregTab[textured][nv]);
    *q++ = prim;
    for (i = 0; i < nv; i++)
    {
        *q++ = rgbaq[i];
        if (textured)
        {
            *q++ = uv[i];
        }
        *q++ = xyz[i];
    }
}

/** Whether the triangle a, b, c spans more than the GPU draws (1023x511). */
static int too_big_xy(const s32* x, const s32* y, int a, int b, int c)
{
    return absd(x[a], x[b]) > 1023 || absd(x[b], x[c]) > 1023 || absd(x[a], x[c]) > 1023 ||
           absd(y[a], y[b]) > 511 || absd(y[b], y[c]) > 511 || absd(y[a], y[c]) > 511;
}

static u32 polygon(const u32* w, s32 n)
{
    u32  cmd      = w[0] >> 24;
    s32  nv       = (cmd & 0x08) ? 4 : 3;
    u32  gouraud  = (cmd >> 4) & 1;
    u32  textured = (cmd >> 2) & 1;
    u32  raw      = (cmd & 1) && textured;
    s32  words    = nv * (1 + (s32)textured) + (gouraud ? nv : 1);
    u32  rgb[4], clut = 0, tpage = s_TexPage, colorMask = 0xFFFFFF;
    s32  x[4], y[4], i, k = 0;
    u64x rgbaq[4], uv[4], xyz[4], prim, a;
    Prim p;

    if (n < words)
    {
        return (u32)n;
    }
    for (i = 0; i < nv; i++)
    {
        u32 xy;
        rgb[i] = (i == 0 || gouraud) ? w[k++] & 0xFFFFFF : rgb[0];
        xy     = w[k++];
        x[i]   = sext11(xy) + s_OffX;
        y[i]   = sext11(xy >> 16) + s_OffY;
        if (textured)
        {
            u32 t = w[k++];
            uv[i] = (u64x)((t & 0xFF) << 4) | ((u64x)(((t >> 8) & 0xFF) << 4) << 16);
            if (i == 0) clut = t >> 16;
            if (i == 1) tpage = (s_TexPage & ~0x1FFu) | ((t >> 16) & 0x1FF);
        }
    }
    p.gouraud  = gouraud;
    p.textured = textured;
    p.raw      = raw;
    p.semi     = (cmd >> 1) & 1;
    p.tpage    = tpage;
    p.clut     = clut;
    if (textured)
    {
        s_TexPage = tpage; /* textured polygons also set the current texture page */
    }
    p.semiMode = (tpage >> 5) & 3;
    p.dither   = ((s_TexPage >> 9) & 1) && (gouraud || (textured && !raw));
    if (!textured && !gouraud && !p.dither)
    {
        colorMask = 0xF8F8F8; /* as quantize() */
    }
    a = textured ? 0x80ULL << 24 : 0;
    for (i = 0; i < nv; i++)
    {
        rgbaq[i] = (u64x)(raw ? 0x808080 : (rgb[i] & colorMask)) | a | (0x3F800000ULL << 32);
        xyz[i]   = (u64x)((x[i] + 2048) << 4 & 0xFFFF) | ((u64x)((y[i] + 2048) << 4 & 0xFFFF) << 16);
    }
    prim = prim_state(&p);
    s_StDate += s_CheckMask * (nv == 4 ? 2 : 1);
    s_StRtTex += (textured && ((tpage >> 7) & 3) >= 2) * (nv == 4 ? 2 : 1);
    if (nv == 4)
    {
        int big0 = too_big_xy(x, y, 0, 1, 2), big1 = too_big_xy(x, y, 1, 2, 3);
        if (!big0 && !big1)
        {
            emit_packed((int)textured, prim | 4, rgbaq, uv, xyz, 4); /* both halves as one strip */
        }
        else
        {
            if (!big0)
            {
                emit_packed((int)textured, prim | 3, rgbaq, uv, xyz, 3);
            }
            if (!big1)
            {
                emit_packed((int)textured, prim | 3, rgbaq + 1, uv + 1, xyz + 1, 3);
            }
        }
    }
    else if (!too_big_xy(x, y, 0, 1, 2))
    {
        emit_packed((int)textured, prim | 3, rgbaq, uv, xyz, 3);
    }
    return (u32)k;
}

static u32 rectangle(const u32* w, s32 n)
{
    u32  cmd = w[0] >> 24;
    u32  siz = (cmd >> 3) & 3;
    Prim p;
    Vtx  a, b;
    s32  k = 0, rw, rh;
    u64x prim;

    p.gouraud  = 0;
    p.textured = (cmd >> 2) & 1;
    p.raw      = (cmd & 1) && p.textured;
    p.semi     = (cmd >> 1) & 1;
    p.tpage    = s_TexPage;
    p.semiMode = (s_TexPage >> 5) & 3;
    p.dither   = 0;
    p.clut     = 0;

    read_rgb(w[k++], &a);
    if (k >= n) return n;
    read_xy(w[k++], &a);
    a.u = a.v = 0;
    if (p.textured)
    {
        if (k >= n) return n;
        a.u    = w[k] & 0xFF;
        a.v    = (w[k] >> 8) & 0xFF;
        p.clut = w[k] >> 16;
        k++;
    }
    if (siz == 0)
    {
        if (k >= n) return n;
        rw = w[k] & 0x3FF;
        rh = (w[k] >> 16) & 0x1FF;
        k++;
    }
    else
    {
        rw = rh = siz == 1 ? 1 : siz == 2 ? 8 : 16;
    }
    if (rw == 0 || rh == 0)
    {
        return (u32)k;
    }
    if (p.raw)
    {
        a.r = a.g = a.b = 0x80;
    }
    if (!p.textured)
    {
        quantize(&a, 1);
    }
    b   = a;
    b.x = a.x + rw;
    b.y = a.y + rh;
    b.u = a.u + rw;
    b.v = a.v + rh;
    prim = prim_state(&p);
    {
        const Vtx* v[2];
        v[0] = &a;
        v[1] = &b;
        emit(&p, prim | 6, v, 2);
    }
    return (u32)k;
}

static u32 lines(const u32* w, s32 n)
{
    u32  cmd = w[0] >> 24;
    Prim p;
    Vtx  prev, cur;
    s32  k = 0;
    u64x prim;

    p.gouraud  = (cmd >> 4) & 1;
    p.textured = 0;
    p.raw      = 0;
    p.semi     = (cmd >> 1) & 1;
    p.semiMode = (s_TexPage >> 5) & 3;
    p.dither   = (s_TexPage >> 9) & 1;
    p.tpage    = s_TexPage;
    p.clut     = 0;
    prim       = prim_state(&p);

    read_rgb(w[k++], &prev);
    if (k >= n) return n;
    read_xy(w[k++], &prev);
    prev.u = prev.v = 0;
    for (;;)
    {
        if (k >= n) return n;
        if ((cmd & 0x08) && (w[k] & 0xF000F000) == 0x50005000)
        {
            return (u32)(k + 1);
        }
        cur = prev;
        if (p.gouraud)
        {
            read_rgb(w[k++], &cur);
            if (k >= n) return n;
        }
        read_xy(w[k++], &cur);
        if (absd(prev.x, cur.x) <= 1023 && absd(prev.y, cur.y) <= 511)
        {
            const Vtx* v[2];
            v[0] = &prev;
            v[1] = &cur;
            emit(&p, prim | 1, v, 2);
        }
        prev = cur;
        if (!(cmd & 0x08))
        {
            return (u32)k;
        }
    }
}

/** Solid rectangle in VRAM coordinates, ignoring the drawing offset, clip area and mask settings
 * (GP0 02 fill, ClearImage). */
static void fill_rect(s32 x, s32 y, s32 w, s32 h, u32 rgb)
{
    s_Epoch++;
    Vtx a, b;
    Prim p;
    memset(&p, 0, sizeof(p));
    if (w <= 0 || h <= 0)
    {
        return;
    }
    base_state();
    scissor(0, 0, VRAM_W - 1, VRAM_H - 1);
    set(R_TEST, (1 << 16) | (1 << 17));
    set(R_FBA, 0);
    set(R_DTHE, 0);
    read_rgb(rgb, &a);
    a.x = x;
    a.y = y;
    a.u = a.v = 0;
    b   = a;
    b.x = x + w;
    b.y = y + h;
    {
        const Vtx* v[2];
        v[0] = &a;
        v[1] = &b;
        emit(&p, 6 | (1 << 8), v, 2);
    }
    cache_invalidate(x, y, w, h);
    if (g_PortGsRenderer == 1)
    {
        /* Keep g_PortVram (the source of the texture and CLUT caches) in step. */
        u16 c = (u16)(((rgb & 0xFF) >> 3) | ((((rgb >> 8) & 0xFF) >> 3) << 5) | ((((rgb >> 16) & 0xFF) >> 3) << 10));
        s32 yy, xx;
        for (yy = 0; yy < h; yy++)
        {
            for (xx = 0; xx < w; xx++)
            {
                g_PortVram[(y + yy) & (VRAM_H - 1)][(x + xx) & (VRAM_W - 1)] = c;
            }
        }
    }
}

static void fill(const u32* w)
{
    s32 x0 = w[1] & 0x3F0, y0 = (w[1] >> 16) & 0x1FF;
    s32 fw = ((w[2] & 0x3FF) + 15) & ~15, fh = (w[2] >> 16) & 0x1FF;
    fill_rect(x0, y0, fw > VRAM_W - x0 ? VRAM_W - x0 : fw, fh > VRAM_H - y0 ? VRAM_H - y0 : fh, w[0] & 0xFFFFFF);
}

/** GS local-to-local copy inside the VRAM buffer. */
static void move(s32 sx, s32 sy, s32 dx, s32 dy, s32 w, s32 h)
{
    if (sx + w > VRAM_W || dx + w > VRAM_W || sy + h > VRAM_H || dy + h > VRAM_H)
    {
        return; /* TODO: wrapping copies */
    }
    ad(R_BITBLTBUF, (u64x)VRAM_TBP | ((u64x)VRAM_BW << 16) | ((u64x)PSM_CT16 << 24) | ((u64x)VRAM_TBP << 32) |
                        ((u64x)VRAM_BW << 48) | ((u64x)PSM_CT16 << 56));
    ad(R_TRXPOS, (u64x)sx | ((u64x)sy << 16) | ((u64x)dx << 32) | ((u64x)dy << 48));
    ad(R_TRXREG, (u64x)w | ((u64x)h << 32));
    ad(R_TRXDIR, 2);
    ad(R_TEXFLUSH, 0);
    cache_invalidate(dx, dy, w, h);
    if (g_PortGsRenderer == 1)
    {
        /* Same copy in g_PortVram (rows in an order that handles overlap). */
        s32 r;
        for (r = 0; r < h; r++)
        {
            s32 row = dy > sy ? h - 1 - r : r;
            memmove(&g_PortVram[dy + row][dx], &g_PortVram[sy + row][sx], (u32)w * 2);
        }
    }
}

static void vram_copy(const u32* w)
{
    s32 sx = w[1] & 0x3FF, sy = (w[1] >> 16) & 0x1FF;
    s32 dx = w[2] & 0x3FF, dy = (w[2] >> 16) & 0x1FF;
    s32 cw = ((w[3] - 1) & 0x3FF) + 1, ch = (((w[3] >> 16) - 1) & 0x1FF) + 1;
    move(sx, sy, dx, dy, cw, ch);
}

static void settings(u32 w)
{
    switch (w >> 24)
    {
        case 0xE1:
            s_TexPage = w & 0x3FFF;
            break;
        case 0xE2:
            s_TwMaskX = w & 31;
            s_TwMaskY = (w >> 5) & 31;
            s_TwOffX  = (w >> 10) & 31;
            s_TwOffY  = (w >> 15) & 31;
            break;
        case 0xE3:
            s_AreaX1 = w & 0x3FF;
            s_AreaY1 = (w >> 10) & 0x1FF;
            break;
        case 0xE4:
            s_AreaX2 = w & 0x3FF;
            s_AreaY2 = (w >> 10) & 0x1FF;
            break;
        case 0xE5:
            s_OffX = sext11(w);
            s_OffY = sext11(w >> 11);
            break;
        case 0xE6:
            s_SetMask   = w & 1;
            s_CheckMask = (w >> 1) & 1;
            break;
    }
}

/** @brief Executes a stream of GP0 words (one OT packet or DrawPrim). */
void GpuGs_Commands(const u32* w, s32 n)
{
    init();
    while (n > 0)
    {
        u32 cmd  = w[0] >> 24;
        u32 used = 1;

        if (cmd >= 0x20 && cmd < 0x40)
        {
            PROF_BEGIN("gs: polygon (incl. state)")
            used = polygon(w, n);
            PROF_END("gs: polygon (incl. state)")
        }
        else if (cmd >= 0x40 && cmd < 0x60)
        {
            used = lines(w, n);
        }
        else if (cmd >= 0x60 && cmd < 0x80)
        {
            PROF_BEGIN("gs: rectangle (incl. state)")
            used = rectangle(w, n);
            PROF_END("gs: rectangle (incl. state)")
        }
        else if (cmd == 0x02)
        {
            if (n >= 3) fill(w);
            used = 3;
        }
        else if (cmd >= 0x80 && cmd < 0xA0)
        {
            if (n >= 4) vram_copy(w);
            used = 4;
        }
        else if (cmd >= 0xE1 && cmd <= 0xE6)
        {
            settings(w[0]);
        }
        if (used == 0)
        {
            used = 1;
        }
        w += used;
        n -= (s32)used;
    }
}

/* --- VRAM transfers (libgpu LoadImage/StoreImage/ClearImage/MoveImage) ------------------------*/

/** g_PortVram's rectangle was written by the CPU: copy it to the GS. */
void GpuGs_LoadImage(s32 x, s32 y, s32 w, s32 h)
{
    s32 row, col;
    init();
    if (w <= 0 || h <= 0)
    {
        return;
    }
    cache_invalidate(x, y, w, h);
    if (x + w > VRAM_W || y + h > VRAM_H)
    {
        /* Wrapping uploads: split at the VRAM edges. */
        s32 w1 = x + w > VRAM_W ? VRAM_W - x : w, h1 = y + h > VRAM_H ? VRAM_H - y : h;
        GpuGs_LoadImage(x, y, w1, h1);
        if (w1 < w) GpuGs_LoadImage(0, y, w - w1, h1);
        if (h1 < h) GpuGs_LoadImage(x, 0, w1, h - h1);
        if (w1 < w && h1 < h) GpuGs_LoadImage(0, 0, w - w1, h - h1);
        return;
    }
    if ((w & 7) == 0)
    {
        /* Whole rows of 8 pixels (16 bytes): in chunks that fit the staging buffer. */
        s32 rows = (s32)sizeof(s_Staging) / (w * 2);
        for (row = 0; row < h; row += rows)
        {
            s32 n = h - row < rows ? h - row : rows, r;
            for (r = 0; r < n; r++)
            {
                memcpy(&s_Staging[r * w * 2], &g_PortVram[y + row + r][x], w * 2);
            }
            upload(VRAM_TBP, VRAM_BW, PSM_CT16, x, y + row, w, n, s_Staging, (u32)(w * 2 * n));
        }
        return;
    }
    /* Other widths: a row at a time, padded to 16 bytes (the GS drops the excess). */
    for (row = 0; row < h; row++)
    {
        u16* dst = (u16*)s_Staging;
        for (col = 0; col < w; col++)
        {
            dst[col] = g_PortVram[y + row][x + col];
        }
        upload(VRAM_TBP, VRAM_BW, PSM_CT16, x, y + row, w, 1, s_Staging, (u32)((w * 2 + 15) & ~15));
    }
}

void GpuGs_ClearImage(s32 x, s32 y, s32 w, s32 h, u32 rgb)
{
    init();
    fill_rect(x, y, w, h, rgb);
}

void GpuGs_MoveImage(s32 sx, s32 sy, s32 dx, s32 dy, s32 w, s32 h)
{
    init();
    move(sx, sy, dx, dy, w, h);
}

#define VIF1_STAT  ((volatile u32*)0x10003C00)
#define GS_BUSDIR  ((volatile u64x*)0x12001040)
#define GS_CSR     ((volatile u64x*)0x12001000)
#define D1_CHCR    ((volatile u32*)0x10009000)
#define VIF1_FBRST ((volatile u32*)0x10003C10)
#define READBACK_SPINS 20000000 /* far beyond a 256-row transfer: the GS or VIF1 is wedged */
#define D1_MADR    ((volatile u32*)0x10009010)
#define D1_QWC     ((volatile u32*)0x10009020)

static u16 s_Readback[256 * VRAM_W] __attribute__((aligned(64)));

/** Local-to-host transfer of VRAM rows [y, y + h) into dst (h <= 256). */
static void download_rows(u16 (*dst)[VRAM_W], s32 y, s32 h)
{
    u32 qwc = (u32)(VRAM_W * 2 * h / 16);
    s32 spins;
    GpuGs_Flush();
    *GS_CSR = 2; /* clear FINISH, then wait for this transfer's FINISH before turning the bus around */
    ad(R_BITBLTBUF, (u64x)VRAM_TBP | ((u64x)VRAM_BW << 16) | ((u64x)PSM_CT16 << 24));
    ad(R_TRXPOS, (u64x)0 | ((u64x)y << 16));
    ad(R_TRXREG, (u64x)VRAM_W | ((u64x)h << 32));
    ad(R_FINISH, 0);
    ad(R_TRXDIR, 1);
    GpuGs_Flush();

    /* Real hardware: the bus may only be turned around once the GS has finished drawing (PCSX2
     * doesn't care); doing it earlier sometimes wedged the download forever (attract demo water). */
    for (spins = 0; !(*GS_CSR & 2) && spins < READBACK_SPINS; spins++)
    {
    }
    FlushCache(0);
    *VIF1_STAT = 0x00800000; /* FDR: VIF1 FIFO direction GS -> EE */
    *GS_BUSDIR = 1;
    *D1_MADR   = (u32)s_Readback & 0x0FFFFFFF;
    *D1_QWC    = qwc;
    *D1_CHCR   = 0x100; /* start, to memory, normal mode */
    for (spins = 0; (*D1_CHCR & 0x100) && spins < READBACK_SPINS; spins++)
    {
    }
    if (*D1_CHCR & 0x100)
    {
        /* Wedged: stop the channel and reset VIF1 rather than freezing (this frame's copy is stale). */
        printf("gpu_gs: VRAM download stuck (rows %d-%d, qwc left %u), recovered\n", y, y + h - 1,
               (unsigned)*D1_QWC);
        *D1_CHCR   = 0;
        *VIF1_FBRST = 1;
    }
    *GS_BUSDIR = 0;
    *VIF1_STAT = 0;
    *GS_CSR    = 2; /* clear FINISH */
    /* Read through the uncached segment: the DMA wrote memory behind the data cache. */
    memcpy(&dst[y][0], (const void*)((u32)s_Readback | 0x20000000), VRAM_W * 2 * h);
}

/** Copies the GS's VRAM into dst (a 1024x512 array). */
void GpuGs_Download(u16 (*dst)[VRAM_W])
{
    init();
    download_rows(dst, 0, 256);
    download_rows(dst, 256, 256);
}

/** Copies the GS's VRAM into g_PortVram (StoreImage). */
void GpuGs_StoreAll(void)
{
    GpuGs_Download(g_PortVram);
}

/* --- Display ----------------------------------------------------------------------------------*/

/** Copies the PS1 display area 1:1 into the GS display frame buffer (fbp: page address, fbw: width / 64,
 * psm: its format). */
void GpuGs_DisplayCopy(u32 fbp, u32 fbw, u32 psm, s32 x, s32 y, s32 w, s32 h)
{
    s_Epoch++;
    init();
    set(R_FRAME, (u64x)fbp | ((u64x)fbw << 16) | ((u64x)psm << 24));
    set(R_XYOFFSET, (u64x)(2048 << 4) | ((u64x)(2048 << 4) << 32));
    set(R_PRMODECONT, 1);
    scissor(0, 0, (s32)fbw * 64 - 1, 1023);
    set(R_TEST, (1 << 16) | (1 << 17));
    set(R_FBA, 0);
    set(R_DTHE, 0);
    set(R_TEX1, 0);
    set(R_CLAMP, 0);
    ad(R_TEXFLUSH, 0);
    set(R_TEX0, (u64x)VRAM_TBP | ((u64x)VRAM_BW << 14) | ((u64x)PSM_CT16 << 20) | (10ULL << 26) | (9ULL << 30) |
                    (1ULL << 35)); /* 1024x512, DECAL */
    ad(R_PRIM, 6 | (1 << 4) | (1 << 8));
    ad(R_RGBAQ, 0x80808080ULL | (0x3F800000ULL << 32));
    ad(R_UV, (u64x)(x << 4) | ((u64x)(y << 4) << 16));
    ad(R_XYZ2, (u64x)(2048 << 4) | ((u64x)(2048 << 4) << 16));
    ad(R_RGBAQ, 0x80808080ULL | (0x3F800000ULL << 32));
    ad(R_UV, (u64x)((x + w) << 4) | ((u64x)((y + h) << 4) << 16));
    ad(R_XYZ2, (u64x)((2048 + w) << 4) | ((u64x)((2048 + h) << 4) << 16));
    GpuGs_Flush();
}

/** Prints and clears the statistics (averaged over `frames`). */
void GpuGs_Stats(u32 frames)
{
    if (!frames)
    {
        return;
    }
    printf("gs stats per frame: %u prims, %u TEX0 changes, %u CLUT loads, %u check-mask prims, %u prims "
           "texturing from VRAM, %u uploads\n", s_StPrims / frames, s_StTex0 / frames, s_StClut / frames,
           s_StDate / frames, s_StRtTex / frames, s_StUploads / frames);
    s_StPrims = s_StTex0 = s_StClut = s_StDate = s_StRtTex = s_StUploads = 0;
}
