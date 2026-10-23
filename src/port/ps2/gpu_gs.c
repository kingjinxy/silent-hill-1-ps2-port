/** @brief Hardware PS1 GPU on the GS: executes GP0 command packets with GS primitives, drawing into
 * an image of the PS1's VRAM kept in GS local memory.
 *
 * GS memory: the PS1's 1024x512 16-bit VRAM is a PSMCT16 buffer at VRAM_ADDR (top of the 4 MiB, so
 * display mode changes, which re-run gsKit's setup, don't move it), drawn to directly as the frame
 * buffer. PSMCT16 has the PS1's pixel layout (5:5:5, bit 15 = alpha MSB = PS1 mask bit). Below it,
 * CACHE_ADDR holds 4/8-bit texture pages; gsKit's display frame buffer is at the bottom.
 *
 * The VRAM buffer is the only copy of the PS1's VRAM (no EE copy since 2026-10-17): uploads go
 * straight to it, StoreImage reads it back.
 *
 * Mapping (written from psx-spx and the GS User's Manual; compared against DuckStation):
 *  - Textures: 15-bit pages are read straight from the VRAM buffer (so render-to-texture works).
 *    4/8-bit pages are read back from the VRAM buffer on a cache miss (download_rect) and uploaded
 *    as PSMT4/PSMT8, which packs indices the same way, and cached per page. Their CLUTs are read back
 *    the same way into a CLUT cache (one row each, loaded with CSM2): loading CLUTs from the VRAM
 *    buffer itself (the render target) made PCSX2's hardware renderer read the target back for each of
 *    the ~330 CLUT loads per frame (99% host GPU in-game). Uploads, fills and VRAM copies that touch a
 *    cached page or CLUT drop it (cache_invalidate).
 *  - Texel 0x0000 is transparent: TEXA AEM gives it alpha 0, dropped by the alpha test. Other texels
 *    get alpha TA0 = 0x40 (bit 15 clear) or TA1 = 0x80 (bit 15 set), so PABE blends only texels with
 *    bit 15 set (PS1 semi-transparency) and the frame buffer write stores bit 15 from the texel.
 *  - Modulation: GS MODULATE is (texel * colour) >> 7 like the PS1 (0x80 = 1.0); raw texture = DECAL.
 *  - Semi-transparency modes 0-3: ALPHA (A - B) * FIX / 128 + D with FIX 0x40/0x80/0x80/0x20, colour
 *    clamping on.
 *  - Mask bit: set = FBA, check = destination alpha test (DATE).
 *  - Dithering: DTHE with the PS1's matrix in DIMX, for gouraud and modulated textured polygons (the
 *    GS dithers after blending, the PS1 before: faint semi-transparent ones stay undithered).
 *  - Texture window: CLAMP REGION_REPEAT, the same (u & mask) | offset formula.
 *  - Clip area: SCISSOR. Coordinates are offset by 2048 (XYOFFSET) so negative ones fit.
 * Known differences: interpolation and blending rounding, line rasterisation, rectangle flips.
 */

#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <dmaKit.h>
#include "port/prof.h"
#include "port/spr.h"

typedef unsigned long long u64x;

#define VRAM_W 1024
#define VRAM_H 512

#define VRAM_ADDR  0x300000                /* bytes */
#define VRAM_TBP   (VRAM_ADDR / 256)        /* block address (64 words) */
#define VRAM_FBP   (VRAM_ADDR / 8192)       /* page address (2048 words) */
#define VRAM_BW    (VRAM_W / 64)
#define CACHE_ADDR 0x200000
#define CACHE_SLOT 0x10000
#define CACHE_SLOTS 37 /* 15 at CACHE_ADDR, 22 more from CACHE2_ADDR */
#define CACHE2_ADDR 0xA0000 /* above SNAP_ADDR's 640x480x2 = 0x96000 */
/* With a shadow buffer (GpuGs_HiresLayout), the page cache slots after the first 15 move above it:
 * 2x 16-bit (640x1024x2, 1.25 MiB): 27 slots, 12 from 0x140000; 1x 32-bit (320x512x4, 640 KiB): all 37,
 * as without; 2x 32-bit (640x1024x4, 2.5 MiB, over the first slots too): 7 slots from 0x280000. */
/* 2x renderer (the Extra Options "Resolution" setting; SH1_HIRES=1 forces it on): the PS1's frame
 * buffer area, VRAM x 0-319, y 0-511, drawn at twice the resolution into a 640x1024 PSMCT16 buffer
 * (shared with SNAP_ADDR and gsKit's frame buffer, neither used while it is on). On while the option
 * is and the display is 320 wide and progressive (gameplay, cutscenes; GpuGs_Hires):
 * - primitives drawn with a clip area inside the frame buffer area go to the hires buffer, with
 *   doubled coordinates (s_Sh);
 * - 15-bit texture pages over the area (feedback effects: motion blur, load screen) sample it with
 *   doubled UVs (Prim.hr);
 * - uploads, fills and VRAM copies into the area are made in VRAM and scaled up into it; reads of
 *   the area (readbacks for the texture/CLUT caches, StoreImage, copies from it) scale it down into
 *   VRAM first, so VRAM is only brought up to date when something reads it;
 * - the display scans the hires buffer out in 480i or 480p (display_ps2.c). */
#define HIRES_ADDR 0
#define HIRES_FBP  (HIRES_ADDR / 8192)
#define HIRES_TBP  (HIRES_ADDR / 256)
/* The shadow buffer's scale and format (the Debug Options "Resolution" and "Color Depth"): 2x and/or
 * 32-bit (PSMCT32 drawn with dithering off: 8 bits a channel in shading and blending, as DuckStation's
 * True Color; the mask bit is alpha's bit 7, which FBA and DATE use for PSMCT32 as bit 15 for PSMCT16). */
#define HIRES_BW   ((HR_W << s_HrSh) / 64)
#define HR_PAGE_H  (s_HrPsm == PSM_CT32 ? 32 : 64) /* GS page height in the shadow format */
#define HR_W       320 /* the area mirrored, in VRAM pixels */
#define HR_H       512
/* Snapshot of single-buffered interlaced screens (GpuGs_DisplayShow), 640 wide, 16-bit; shared with
 * gsKit's frame buffer, which only its setup uses. */
#define SNAP_ADDR  0
/* CLUT cache: one 256-pixel PSMCT16 row per CLUT (256x128, the last 64 KiB of the page cache area). */
#define CLUT_ADDR  0x2F0000
#define CLUT_ROWS  104 /* rows 104-127: filtered CLUTs (filt_get) */
#define FILT_ROW0  104 /* 8-bit: rows 104-111 */
#define FILT_ROWS8 8
#define FILT4_ROW0 112 /* 4-bit: 16 a row in rows 112-127 */
#define FILT4_SETS 128 /* two ways each */
#define FILT_RAWS  16

#define PSM_CT32 0x00
#define PSM_CT16 0x02
#define PSM_T8   0x13
#define PSM_T4   0x14

/* GS registers (A+D addresses). */
enum
{
    R_PRIM = 0x00, R_RGBAQ = 0x01, R_UV = 0x03, R_XYZ2 = 0x05, R_TEX0 = 0x06, R_CLAMP = 0x08,
    R_TEX1 = 0x14, R_XYOFFSET = 0x18, R_PRMODECONT = 0x1A, R_TEXCLUT = 0x1C, R_TEXA = 0x3B,
    R_TEXFLUSH = 0x3F, R_SCISSOR = 0x40, R_SCANMSK = 0x22, R_ALPHA = 0x42, R_DIMX = 0x44, R_DTHE = 0x45,
    R_COLCLAMP = 0x46, R_TEST = 0x47, R_PABE = 0x49, R_FBA = 0x4A, R_FRAME = 0x4C, R_ZBUF = 0x4E,
    R_BITBLTBUF = 0x50, R_TRXPOS = 0x51, R_TRXREG = 0x52, R_TRXDIR = 0x53, R_FINISH = 0x61,
    REG_COUNT = 0x62
};

extern void Display_EnsureInit(void); /* display_ps2.c */

/* --- GIF packets ------------------------------------------------------------------------------*/

/* The packet is a sequence of GIF tags: A+D tags for register writes, and REGLIST tags for runs of
 * primitives of the same shape (PRIM and the vertex registers listed once in the tag, then 64 bits
 * per register: half the data of A+D). */
/* Two packet buffers, used in turn (for asynchronous sends; see dma_send_impl). */
#define PKT_WORDS 8192 /* 64-bit words per buffer */
static u64x s_PktBuf[2][PKT_WORDS] __attribute__((aligned(64)));
/* Written through the uncached-accelerated mirror of RAM (0x30000000 + address): the packets (~128 KB a
 * frame) only go to the GS by DMA, so building them through the data cache only evicted the game's data
 * and loaded lines about to be overwritten. Writes are combined in the EE's UCAB; `sync.l` drains it
 * before a send. The DMA gets the plain address (s_PktBuf). */
#define UCAB(p) ((u64x*)(((u32)(p) & 0x0FFFFFFF) | 0x30000000))
/* The packet builder's state, the register shadow, the texture caches' tables and the memos are in
 * the EE's scratchpad (include/port/spr.h): DrawOTag touches them for every primitive. */
static SPR u64x* s_Pkt; /* UCAB(s_PktBuf[s_PktCur]), set by init() */
static SPR int   s_PktCur;
static SPR int  s_Pos;            /* words used */
static int  s_TagAt = -1;     /* open tag (word index), -1 = none */
static SPR u64x s_TagRegs;        /* REGLIST register list of the open tag, 0 = A+D */
static SPR int  s_TagNreg;        /* registers per loop */
static SPR int  s_TagLoops;
static int  s_LastTag = -1;   /* for EOP */

static SPR u64x s_Reg[REG_COUNT];
#ifdef SH_PORT_LITEPROF
unsigned int g_LiteProf[LP_COUNT][2];
#endif
/* Incremented whenever GS state may have changed outside prim_state() (reset, fills, display
 * copies, texture cache changes): its memo of the last primitive's state is then stale. */
static SPR u32  s_Epoch;
/* Per-frame statistics (GpuGs_Stats): primitives, texture (TEX0) changes, CLUT loads, primitives
 * with destination alpha test (check mask), primitives sampling the VRAM buffer (15-bit pages). */
static u32 s_StPrims, s_StTex0, s_StClut, s_StDate, s_StRtTex, s_StUploads, s_StReadbacks;
#ifdef SH_PORT_LITEPROF
/* Texture cache diagnostics: misses, and entries dropped because their VRAM changed (with the
 * rectangles that dropped them, a few a report). */
static u32 s_StFiltCalls, s_StFiltMiss, s_StPageMiss, s_StClutMiss, s_StPageDrop, s_StClutDrop, s_StDropRects;
static s16 s_DropRect[8][4];
#define ST_DROP_RECT(x, y, w, h) do { if (s_StDropRects < 8) { s16* r_ = s_DropRect[s_StDropRects]; r_[0] = x; r_[1] = y; r_[2] = w; r_[3] = h; } s_StDropRects++; } while (0)
#define ST_COUNT(v) ((v)++)
#else
#define ST_DROP_RECT(x, y, w, h) do { } while (0)
#define ST_COUNT(v) do { } while (0)
#endif
static SPR u8   s_RegValid[REG_COUNT];

/* Interlaced 480-line display with drawing to the display area off (GP0 E1 bit 10, DFE = 0): the
 * game draws into the area on screen (single-buffered: title, inventory). On the PS1 (Mednafen's
 * LineSkipTest), nothing is drawn on the VRAM lines of the field being scanned out, so the screen
 * only ever shows complete fields.
 * - Default: the GS draws every line, and each frame is shown as a snapshot (display_copy into
 *   SNAP_ADDR), so the half-redrawn area is never on screen.
 * - SH_PORT_INTERLACE_FIELDS: as on the PS1, SCANMSK masks the lines of the field being scanned out
 *   (CSR.FIELD, looked at again after each vertical blank and after DFE or mode changes), and the
 *   area is shown straight from VRAM. Broken in PCSX2 (menu text missing in its output although
 *   VRAM holds it at every swap, 2026-10-17); to test on hardware. */
static s32 s_Interlace480, s_DispY;
static s32 s_MaskDirty = 1;
static u32 s_MaskSeq;
extern volatile unsigned int g_PortVBlankSeq; /* libetc_ps2.c: vertical blanks */

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

/* GIF DMA (channel 2) registers. Sends are asynchronous: a send starts the channel and returns, and
 * the EE goes on building the next packet while the GS takes this one. A send waits for the one
 * before it; GpuGs_Sync waits for the last. Polling only (no DMA interrupt), so it works with
 * interrupts off. (The 2026-10-09 attempt "hung at the logos" on hardware; the boot freeze found the
 * same day, ResetCallback dropping the vertical blank callback, is the likelier cause: Planning Doc.md.) */
#define D2_CHCR (*(volatile u32*)0x1000A000)
#define D2_MADR (*(volatile u32*)0x1000A010)
#define D2_QWC  (*(volatile u32*)0x1000A020)

static void pend_flush(void); /* fog pairs, below */

static inline void dma_wait(void)
{
    while (D2_CHCR & 0x100) /* STR */
    {
    }
}

/** For the callers that need the last send finished: uploads reusing s_ImgPkt, readback, display,
 * DrawSync. */
void GpuGs_Sync(void)
{
    pend_flush();
    dma_wait();
}

/** `data`: in main RAM, its last writes already in memory (the packet buffers are written through
 * the uncached-accelerated mirror and drained with sync.l; dma_send_cached writes back cached data). */
static void dma_send_impl(void* data, u32 qwc)
{
    dma_wait();
    D2_MADR = (u32)data & 0x0FFFFFFF;
    D2_QWC  = qwc;
    __asm__ volatile("sync.l" ::: "memory");
    D2_CHCR = 0x101; /* from memory, normal mode, STR */
}

/** As dma_send, for data written through the data cache: writes back only its own lines. */
static void dma_send_cached(void* data, u32 qwc)
{
    SyncDCache(data, (u8*)data + qwc * 16);
    dma_send(data, qwc);
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
static void GpuGs_FlushImpl(void);
void GpuGs_Flush(void)
{
    LP_BEGIN(LP_FLUSH)
    GpuGs_FlushImpl();
    LP_END(LP_FLUSH);
}

static void GpuGs_FlushImpl(void)
{
    pend_flush();
    tag_close();
    if (s_Pos == 0)
    {
        return;
    }
    s_Pkt[s_LastTag] |= 1 << 15; /* EOP */
    __asm__ volatile("sync.l" ::: "memory"); /* the UCAB's last writes reach RAM before the DMA reads */
    dma_send(s_PktBuf[s_PktCur], (u32)(s_Pos / 2));
    /* Build the next packet in the other buffer (whose own send finished before this one started). */
    s_PktCur ^= 1;
    s_Pkt     = UCAB(s_PktBuf[s_PktCur]);
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
    pend_flush();
    s_Epoch++;
    s_MaskDirty = 1; /* SCANMSK sent again */
    memset(s_RegValid, 0, sizeof(s_RegValid));
}

/* --- Drawing state (GP0 E1-E6), as in gpu_soft.c ----------------------------------------------*/

static SPR u32 s_TexPage;
static SPR u32 s_TwMaskX, s_TwMaskY, s_TwOffX, s_TwOffY;
static s32 s_AreaX1, s_AreaY1, s_AreaX2 = VRAM_W - 1, s_AreaY2 = VRAM_H - 1;
/* Incremented when a drawing setting prim_state depends on changes (GP0 E2-E4, E6): its memo then
 * only compares this, s_Epoch and the primitive's own flags. */
static u32 s_StateGen = 1;
static s32 s_OffX, s_OffY;
static u32 s_SetMask, s_CheckMask;

static s32 s_Hires;    /* the hires buffer is on */
static s32 s_HrLayout; /* GS memory laid out for it: the option's bits (1: 2x, 2: 32-bit), 0 none (GpuGs_HiresLayout) */
static u32 s_HrSh;     /* its scale: coordinates << s_HrSh */
static u32 s_HrPsm = PSM_CT16;
static u32 s_Sh;       /* 1: primitives are drawn into the hires buffer (coordinates doubled) */
#define HR_SH s_Sh
/* Drawing into a 32-bit shadow buffer: colours keep 8 bits (no 5-bit quantization, no dithering). */
#define HR_TRUECOLOR (s_Sh && s_HrPsm == PSM_CT32)
/* GS vertex coordinate (12.4, before << 4) of a PS1 one. At 2x, relative to the drawing offset (E5)
 * and doubled, with XYOFFSET moved to match (state_other): the PS1's vertex range is +-1024 around
 * the offset, which doubled still fits the GS's 0-4095 (absolute coordinates doubled didn't: room
 * walls with far-off vertices wrapped into flat bands). */
#define HR_X(x) (s_Sh ? (((x) - s_OffX) << s_HrSh) + 2048 : (x) + 2048)
#define HR_Y(y) (s_Sh ? (((y) - s_OffY) << s_HrSh) + 2048 : (y) + 2048)
/** Whether a 15-bit page `tpage` is sampled from the hires buffer. */
#define HR_TEX(depth, tpage) (s_Hires && (depth) >= 2 && ((tpage) & 15) * 64 < HR_W)
static void hr_target(void);
static void hr_blit(int up, s32 x, s32 y, s32 w, s32 h);
static void hr_mark(s32 x0, s32 y0, s32 x1, s32 y1);

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
    u32 hr; /* a 15-bit page sampled from the hires buffer (UVs doubled) */
    u32 filt; /* dithered semi-transparent 4/8-bit texture: its CLUT filtered (filt_get), 0 none */
} Prim;

static s32 sext11(u32 v)
{
    return ((s32)(v << 21)) >> 21;
}

/* --- Texture page cache (4/8-bit pages as PSMT4/PSMT8) ----------------------------------------*/

static void download_rect(s32 x, s32 y, s32 w, s32 h, u16* dst, s32 stride); /* VRAM readback, below */

typedef struct
{
    s32 key; /* (depth << 5) | page index, -1 = free */
    u32 used;
} Slot;

static SPR Slot s_Slots[CACHE_SLOTS];
static s32  s_CacheSlots = CACHE_SLOTS - 1; /* in use (fewer with a 2x shadow buffer; the last is blend_exact's) */

/** GS block address of page cache slot i. */
static u32 slot_tbp(s32 i)
{
    if (s_HrLayout == 3)
    {
        return (u32)(0x280000 + i * CACHE_SLOT) / 256; /* 2x 32-bit: 7 slots up to CLUT_ADDR */
    }
    return (u32)(i < 15 ? CACHE_ADDR + i * CACHE_SLOT
                        : (s_HrLayout == 1 ? 0x140000 : CACHE2_ADDR) + (i - 15) * CACHE_SLOT) / 256;
}

typedef struct
{
    s32 key; /* CLUT attribute | depth << 16, -1 = free */
    u32 used;
} ClutSlot;

static SPR ClutSlot s_Cluts[CLUT_ROWS];

/* Direct lookups (slot or row + 1, 0 = not cached): page cache by key, CLUT cache by
 * CLUT attribute | depth << 15. */
static SPR u8 s_PageMap[64];
static u8 s_ClutMap[0x10000];
#define CLUT_MAP(key) (((key) & 0x7FFF) | (((key) >> 16) << 15))

static SPR u32  s_UseClock;
/* Incremented whenever a page or CLUT changes cache slot (s_PageMap/s_ClutMap entries set or
 * dropped): tex_state's memo of TEX0/TEXCLUT values is then stale. */
static u32  s_TexGen = 1;
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
static void filt_invalidate(s32 x, s32 y, s32 w, s32 h);

static void cache_invalidate(s32 x, s32 y, s32 w, s32 h)
{
    s32 i;
    s_Epoch++;
    filt_invalidate(x, y, w, h);
    for (i = 0; i < s_CacheSlots; i++)
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
            ST_COUNT(s_StPageDrop);
            ST_DROP_RECT(x, y, w, h);
            s_TexGen++;
            s_RegValid[R_TEX0]        = 0; /* prim_state's texture memo: look the page up again */
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
            ST_COUNT(s_StClutDrop);
            ST_DROP_RECT(x, y, w, h);
            s_TexGen++;
            s_RegValid[R_TEX0]                  = 0;
        }
    }
}

/** Host-to-local image transfer of `bytes` (a multiple of 16) from `src`. */
/** Host-to-local transfer of the `bytes` (a multiple of 16) already placed at &s_ImgPkt[2]. */
static void upload_staged_impl(u32 dbp, u32 dbw, u32 psm, s32 x, s32 y, s32 w, s32 h, u32 bytes);
static void upload_staged(u32 dbp, u32 dbw, u32 psm, s32 x, s32 y, s32 w, s32 h, u32 bytes)
{
    PROF_BEGIN("gs: upload (texture page, CLUT, LoadImage)")
    upload_staged_impl(dbp, dbw, psm, x, y, w, h, bytes);
    PROF_END("gs: upload (texture page, CLUT, LoadImage)")
}

static void upload_staged_impl(u32 dbp, u32 dbw, u32 psm, s32 x, s32 y, s32 w, s32 h, u32 bytes)
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
    dma_send_cached(s_ImgPkt, qwc + 1);
    ad(R_TEXFLUSH, 0);
}

/** Host-to-local image transfer of `bytes` (a multiple of 16) from `src`. */
static void upload(u32 dbp, u32 dbw, u32 psm, s32 x, s32 y, s32 w, s32 h, const void* src, u32 bytes)
{
    GpuGs_Sync(); /* s_ImgPkt may still be on its way to the GS */
    memcpy(&s_ImgPkt[2], src, bytes);
    upload_staged(dbp, dbw, psm, x, y, w, h, bytes);
}

/** A fill of the 16-bit value `c` over (x, y, w, h) (fill_rect). Cached pages and CLUTs it touches get
 * the same value written into their cached copy instead of being dropped: 4/8-bit indices pack into
 * the same bytes as the 16-bit words, so the copy stays equal to VRAM with no readback. (The game's
 * screen clear covers rows of an 8-bit page whose textures sit above the frame buffer: 2-4 re-reads
 * a frame otherwise, each waiting for the GS.) Pages that wrap at the right edge are dropped. */
static void cache_fill(s32 x, s32 y, s32 w, s32 h, u16 c)
{
    s32 i, k;
    filt_invalidate(x, y, w, h);
#ifdef SH_PORT_INTERLACE_FIELDS
    if (s_Reg[R_SCANMSK])
    {
        cache_invalidate(x, y, w, h); /* masked fills skip every other line */
        return;
    }
#endif
    for (i = 0; i < s_CacheSlots; i++)
    {
        s32 px, py, pw, x0, x1, y0, y1, depth, scale;
        if (s_Slots[i].key < 0)
        {
            continue;
        }
        page_rect(s_Slots[i].key, &px, &py, &pw);
        if (!(x < px + pw && px < x + w && y < py + 256 && py < y + h))
        {
            continue;
        }
        if (px + pw > VRAM_W)
        {
            s_PageMap[s_Slots[i].key] = 0;
            s_Slots[i].key            = -1;
            s_TexGen++;
            s_RegValid[R_TEX0]        = 0;
            continue;
        }
        x0    = x > px ? x : px;
        x1    = x + w < px + pw ? x + w : px + pw;
        y0    = y > py ? y : py;
        y1    = y + h < py + 256 ? y + h : py + 256;
        depth = s_Slots[i].key >> 5;
        scale = depth == 0 ? 4 : 2; /* indices per VRAM word */
        GpuGs_Sync();               /* s_ImgPkt may still be on its way to the GS */
        for (k = 0; k < (x1 - x0) * (y1 - y0); k++)
        {
            ((u16*)&s_ImgPkt[2])[k] = c;
        }
        upload_staged(slot_tbp(i), 4, depth == 0 ? PSM_T4 : PSM_T8, (x0 - px) * scale, y0 - py, (x1 - x0) * scale,
                      y1 - y0, (u32)(((x1 - x0) * (y1 - y0) * 2 + 15) & ~15));
    }
    for (i = 0; i < CLUT_ROWS; i++)
    {
        s32 cx, cy, cw, x0, x1;
        u16 row[256] __attribute__((aligned(16)));
        if (s_Cluts[i].key < 0)
        {
            continue;
        }
        cx = (s_Cluts[i].key & 0x3F) * 16;
        cy = (s_Cluts[i].key >> 6) & 0x1FF;
        cw = (s_Cluts[i].key >> 16) == 0 ? 16 : 256;
        if (!(x < cx + cw && cx < x + w && y <= cy && cy < y + h))
        {
            continue;
        }
        if (cx + cw > VRAM_W)
        {
            s_ClutMap[CLUT_MAP(s_Cluts[i].key)] = 0;
            s_Cluts[i].key                      = -1;
            s_TexGen++;
            s_RegValid[R_TEX0]                  = 0;
            continue;
        }
        x0 = x > cx ? x : cx;
        x1 = x + w < cx + cw ? x + w : cx + cw;
        for (k = 0; k < x1 - x0; k++)
        {
            row[k] = c;
        }
        upload(CLUT_ADDR / 256, 4, PSM_CT16, x0 - cx, i, x1 - x0, 1, row, (u32)(((x1 - x0) * 2 + 15) & ~15));
        s_RegValid[R_TEX0] = 0; /* this row's contents changed: load it again */
    }
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
    ST_COUNT(s_StPageMiss);
    /* A free slot, else the least recently used one. */
    for (i = 0; i < s_CacheSlots; i++)
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
    /* The page's indices, 256 rows of 64 (4-bit) or 128 (8-bit) VRAM words, read back from the VRAM
     * buffer straight into the transfer packet (in two parts when the page wraps at the right edge). */
    page_rect(key, &x, &y, &w);
    GpuGs_Sync(); /* s_ImgPkt may still be on its way to the GS */
    if (x + w <= VRAM_W)
    {
        download_rect(x, y, w, 256, (u16*)&s_ImgPkt[2], w);
    }
    else
    {
        download_rect(x, y, VRAM_W - x, 256, (u16*)&s_ImgPkt[2], w);
        download_rect(0, y, x + w - VRAM_W, 256, (u16*)&s_ImgPkt[2] + (VRAM_W - x), w);
    }
    upload_staged(slot_tbp(best), 4, depth == 0 ? PSM_T4 : PSM_T8, 0, 0, 256, 256, (u32)(w * 2 * 256));
    if (s_Slots[best].key >= 0)
    {
        s_PageMap[s_Slots[best].key] = 0;
    }
    s_Slots[best].key  = key;
    s_Slots[best].used = ++s_UseClock;
    s_PageMap[key]     = (u8)(best + 1);
    s_TexGen++;
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
    ST_COUNT(s_StClutMiss);
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
    if (x + n <= VRAM_W)
    {
        download_rect(x, y, n, 1, row, n);
    }
    else
    {
        download_rect(x, y, VRAM_W - x, 1, row, n);
        download_rect(0, y, x + n - VRAM_W, 1, row + (VRAM_W - x), n);
    }
    upload(CLUT_ADDR / 256, 4, PSM_CT16, 0, best, n, 1, row, (u32)(n * 2));
    if (s_Cluts[best].key >= 0)
    {
        s_ClutMap[CLUT_MAP(s_Cluts[best].key)] = 0;
    }
    s_Cluts[best].key            = key;
    s_Cluts[best].used           = ++s_UseClock;
    s_ClutMap[CLUT_MAP(key)]     = (u8)(best + 1);
    s_TexGen++;
    s_RegValid[R_TEX0] = 0; /* this row's contents changed: load it again */
    return (u32)best;
}

/* Filtered CLUTs: the PS1 dithers a semi-transparent primitive's colour before blending, clamped at
 * 0, the GS the blended result. For adding and subtracting blend modes they agree, except where the
 * modulated texel is too faint to change the frame buffer on the PS1 (under 4; under 16 for the
 * quarter-adding mode 3): there the GS's negative dither cells take a level off (darker flare edges,
 * a checkered rectangle where the flashlight draws its black quad). Such primitives use a copy of
 * their CLUT with those semi-transparent entries made transparent (0x0000, dropped by the alpha
 * test). `filt`: filt_canon of the primitive's largest vertex colour per channel | mode 3 << 24. The
 * CLUT as read from VRAM is kept, so another colour only filters it again. */
/* The CLUTs as read from VRAM, kept so another colour only filters them again. */
typedef struct
{
    s32 key; /* as s_Cluts' (CLUT attribute | depth << 16), -1 = free */
    u32 used;
    u16 raw[256] __attribute__((aligned(16)));
} FiltRaw;
static FiltRaw s_FiltRaw[FILT_RAWS];

/* Filtered CLUTs in GS memory: 8-bit ones a whole row each (FILT_ROW0, FILT_ROWS8, least recently
 * used), 4-bit ones 16 entries each, 16 to a row from FILT4_ROW0 (TEXCLUT's COU picks the entry), in a
 * two-way cache by CLUT and filter: map4_s02's effects use up to ~110 pairs a frame (7 CLUTs, 20
 * filters). A new one never replaces the one drawn with last (the GS reads a CLUT row after the
 * upload that would replace it: measured): the least recently used of two ways never is. */
typedef struct
{
    s32 key; /* as FiltRaw's, -1 = free */
    u32 filt, used;
} FiltSlot;
static FiltSlot s_Filt8[FILT_ROWS8];
static FiltSlot s_Filt4[FILT4_SETS * 2];
static u32      s_FiltDrawn; /* the polygon just drawn used a filtered CLUT */
static u32      s_FiltPass2; /* drawing its second pass (the left-out texels) */

/** The filter's inputs from a colour (largest per channel, `mode3`: quarter-adding): per channel (8
 * bits each), the smallest texel channel kept (texel * colour >= 64, or 256 for mode 3; 32: none) and
 * bit 6, adding only: that value is the faint channel's raise (it times the colour stays under 80).
 * Colours with the same thresholds filter a CLUT identically, so they share its row (map4_s02's
 * effects: ~330 filtered polygons a frame in ~170 colours, but far fewer thresholds). */
static u32 filt_canon(u32 rgb, u32 mode3)
{
    u32 ch, out = mode3 << 24;
    u32 lim = mode3 ? 16 * 16 : 4 * 16;
    for (ch = 0; ch < 3; ch++)
    {
        u32 c = (rgb >> (ch * 8)) & 0xFF;
        u32 kmin = c ? (lim + c - 1) / c : 32;
        if (kmin > 32)
        {
            kmin = 32;
        }
        out |= kmin << (ch * 8);
        if (!mode3 && c && kmin * c < 80 && kmin <= 31)
        {
            out |= 0x40u << (ch * 8);
        }
    }
    return out;
}

/** The CLUT `key` (`n` entries) as read from VRAM, kept in s_FiltRaw. */
static const u16* filt_raw(s32 key, u32 clut, s32 n)
{
    s32      i, best = 0;
    FiltRaw* r;
    for (i = 0; i < FILT_RAWS; i++)
    {
        if (s_FiltRaw[i].key == key)
        {
            s_FiltRaw[i].used = ++s_UseClock;
            return s_FiltRaw[i].raw;
        }
        if (s_FiltRaw[best].key >= 0 && (s_FiltRaw[i].key < 0 || s_FiltRaw[i].used < s_FiltRaw[best].used))
        {
            best = i;
        }
    }
    r = &s_FiltRaw[best];
    {
        s32 x = (s32)(clut & 0x3F) * 16, y = (s32)((clut >> 6) & 0x1FF);
        if (x + n <= VRAM_W)
        {
            download_rect(x, y, n, 1, r->raw, n);
        }
        else
        {
            download_rect(x, y, VRAM_W - x, 1, r->raw, n);
            download_rect(0, y, x + n - VRAM_W, 1, r->raw + (VRAM_W - x), n);
        }
    }
    r->key  = key;
    r->used = ++s_UseClock;
    return r->raw;
}

/** TEXCLUT for `clut` (a 4- or 8-bit page's) filtered by `filt`. */
static u64x filt_get(u32 clut, u32 depth, u32 filt)
{
    s32        key = (s32)((clut & 0x7FFF) | (depth << 16));
    s32        i, best = -1, n = depth == 0 ? 16 : 256, k, cou, cov;
    const u16* raw;
    FiltSlot*  s;
    u16        row[256] __attribute__((aligned(16)));
    ST_COUNT(s_StFiltCalls);
    if (depth == 0)
    {
        u32 h = ((u32)key * 0x9E3779B1u) ^ (filt * 0x85EBCA77u);
        h     = ((h >> 15) ^ h) & (FILT4_SETS - 1);
        s     = &s_Filt4[h * 2];
        for (i = 0; i < 2; i++)
        {
            if (s[i].key == key && s[i].filt == filt)
            {
                s[i].used = ++s_UseClock;
                best      = (s32)h * 2 + i;
                return 4 | ((u64x)(best & 15) << 6) | ((u64x)(FILT4_ROW0 + best / 16) << 12);
            }
        }
        best = (s32)h * 2 + (s[0].key < 0 ? 0 : s[1].key < 0 ? 1 : s[1].used < s[0].used ? 1 : 0);
        s    = &s_Filt4[best];
        cou  = best & 15;
        cov  = FILT4_ROW0 + best / 16;
    }
    else
    {
        for (i = 0; i < FILT_ROWS8; i++)
        {
            if (s_Filt8[i].key == key && s_Filt8[i].filt == filt)
            {
                s_Filt8[i].used = ++s_UseClock;
                return 4 | ((u64x)(FILT_ROW0 + i) << 12);
            }
        }
        for (i = 0; i < FILT_ROWS8; i++)
        {
            if (s_Filt8[i].key < 0)
            {
                best = i;
                break;
            }
            if (best < 0 || s_Filt8[i].used < s_Filt8[best].used)
            {
                best = i;
            }
        }
        s   = &s_Filt8[best];
        cou = 0;
        cov = FILT_ROW0 + best;
    }
    ST_COUNT(s_StFiltMiss);
    raw = filt_raw(key, clut, n);
    for (k = 0; k < n; k++)
    {
        u32 t = raw[k], ch, out = t;
        /* (texel << 3) * colour >> 7 >= 4 (16): texel * colour >= 64 (256), as filt_canon's thresholds */
        u32 keep = !(t & 0x8000) || (t & 31) >= (filt & 0x3F) || ((t >> 5) & 31) >= ((filt >> 8) & 0x3F) ||
                   ((t >> 10) & 31) >= ((filt >> 16) & 0x3F);
        if (keep && (t & 0x8000) && !((filt >> 24) & 1))
        {
            /* Adding: a faint channel of a kept entry raised to exactly 4 after modulation, which the
             * dither never takes over or under a level (adds 0, as the PS1's faint channel). */
            for (ch = 0; ch < 3; ch++)
            {
                u32 v = (filt >> (ch * 8)) & 0x3F;
                if (((filt >> (ch * 8)) & 0x40) && ((t >> (ch * 5)) & 31) < v)
                {
                    out = (out & ~(31u << (ch * 5))) | (v << (ch * 5));
                }
            }
        }
        if (filt & (1u << 27))
        {
            row[k] = (!keep && (t & 0x8000)) ? 0x8000 : 0; /* second pass: what the first left out, black */
        }
        else
        {
            row[k] = keep ? (u16)out : 0;
        }
    }
    upload(CLUT_ADDR / 256, 4, PSM_CT16, cou * 16, cov, n, 1, row, (u32)(n * 2));
    s->key             = key;
    s->filt            = filt;
    s->used            = ++s_UseClock;
    s_RegValid[R_TEX0] = 0; /* this row's contents changed: load it again */
    return 4 | ((u64x)cou << 6) | ((u64x)cov << 12);
}

/** Whether a VRAM write at (x, y, w, h) changes the CLUT `key` (as s_Cluts'; -1: none). */
static int filt_touched(s32 key, s32 x, s32 y, s32 w, s32 h)
{
    s32 cx = (key & 0x3F) * 16, cy = (key >> 6) & 0x1FF;
    s32 cw = (key >> 16) == 0 ? 16 : 256;
    return key >= 0 && ((x < cx + cw && cx < x + w) || cx + cw > VRAM_W) && y <= cy && cy < y + h;
}

/** Drops the filtered CLUTs (and their VRAM copies) a VRAM write at (x, y, w, h) touches. */
static void filt_invalidate(s32 x, s32 y, s32 w, s32 h)
{
    s32 i;
    for (i = 0; i < FILT_RAWS; i++)
    {
        if (filt_touched(s_FiltRaw[i].key, x, y, w, h))
        {
            s_FiltRaw[i].key = -1;
        }
    }
    for (i = 0; i < FILT_ROWS8; i++)
    {
        if (filt_touched(s_Filt8[i].key, x, y, w, h))
        {
            s_Filt8[i].key     = -1;
            s_RegValid[R_TEX0] = 0;
        }
    }
    for (i = 0; i < FILT4_SETS * 2; i++)
    {
        if (filt_touched(s_Filt4[i].key, x, y, w, h))
        {
            s_Filt4[i].key     = -1;
            s_RegValid[R_TEX0] = 0;
        }
    }
}


/* --- Setup ------------------------------------------------------------------------------------*/

static int s_Ready;

static void init(void)
{
    s32 i;
    if (!s_Pkt)
    {
        s_Pkt = UCAB(s_PktBuf[s_PktCur]);
    }
    if (s_Ready)
    {
        return;
    }
    Display_EnsureInit();
    for (i = 0; i < s_CacheSlots; i++)
    {
        s_Slots[i].key = -1;
    }
    for (i = 0; i < CLUT_ROWS; i++)
    {
        s_Cluts[i].key = -1;
    }
    for (i = 0; i < FILT_RAWS; i++)
    {
        s_FiltRaw[i].key = -1;
    }
    for (i = 0; i < FILT_ROWS8; i++)
    {
        s_Filt8[i].key = -1;
    }
    for (i = 0; i < FILT4_SETS * 2; i++)
    {
        s_Filt4[i].key = -1;
    }
    memset(s_PageMap, 0, sizeof(s_PageMap));
    memset(s_ClutMap, 0, sizeof(s_ClutMap));
    s_TexGen++;
    s_Ready = 1;
}

static u64x dimx(void)
{
    static SPR u64x cached;
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
    static u32 doneEpoch = ~0u, doneSh;
    if (doneEpoch == s_Epoch && doneSh == HR_SH && s_RegValid[R_FRAME])
    {
        return;
    }
    doneEpoch = s_Epoch;
    doneSh    = HR_SH;
    if (s_Sh)
    {
        set(R_FRAME, (u64x)HIRES_FBP | ((u64x)HIRES_BW << 16) | ((u64x)s_HrPsm << 24));
    }
    else
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

/** SCANMSK for the field being scanned out now (see s_Interlace480): 2 masks even lines, 3 odd. */
static void scanmsk_update(void) __attribute__((noinline));
static void scanmsk_update(void)
{
    u64x m = 0;
    s_MaskSeq   = g_PortVBlankSeq;
    s_MaskDirty = 0;
#ifdef SH_PORT_INTERLACE_FIELDS
    if (s_Interlace480 && !(s_TexPage & 0x400))
#else
    if (0)
#endif
    {
        /* CSR.FIELD 0 scans out the odd lines from DBY (measured: the mask the other way round let
         * drawing hit the lines on screen), so field 0 masks odd lines (3), field 1 even ones (2). */
        u32 field = (u32)(*(volatile u64x*)0x12001000 >> 13) & 1;
        m         = 2 + (field ^ 1u ^ (u32)(s_DispY & 1));
    }
    set(R_SCANMSK, m);
}
#define SCANMSK_CHECK()                                     \
    do                                                      \
    {                                                       \
        if (s_MaskDirty || s_MaskSeq != g_PortVBlankSeq)    \
        {                                                   \
            scanmsk_update();                               \
        }                                                   \
    } while (0)

/** Per-primitive state; returns the PRIM register bits other than the type. */
static u64x state_other(const Prim* p) __attribute__((noinline));
static void tex_state(const Prim* p) __attribute__((noinline));

/** GS state for a primitive; returns the PRIM register bits other than the type. Two parts, each
 * memoized on its own inputs, so a change of texture page or CLUT between neighbouring primitives
 * (530 a frame in the Kaufmann cutscene) only redoes the texture registers:
 * - everything but the texture: base state, scissor, test, blending, dither, mask, texture window;
 * - the texture: TEX0/TEXCLUT (with the texture page and CLUT caches). */
static u64x prim_state(const Prim* p)
{
    static u32  memoEpoch = ~0u, memoGen = ~0u, memoKey = ~0u;
    static SPR u64x memoPrim;
    u32         key;

    SCANMSK_CHECK();

    /* The drawing settings are covered by s_StateGen. */
    key = p->textured | (p->raw << 1) | (p->semi << 2) | (p->semiMode << 3) | (p->gouraud << 5) | (p->dither << 6) |
          (p->hr << 7);
    if (memoEpoch != s_Epoch || memoGen != s_StateGen || memoKey != key)
    {
        LP_BEGIN(LP_STATE_OTHER)
        PROF_BEGIN("gs: prim_state (other)")
        memoPrim = state_other(p);
        PROF_END("gs: prim_state (other)")
        LP_END(LP_STATE_OTHER);
        memoEpoch = s_Epoch;
        memoGen   = s_StateGen;
        memoKey   = key;
    }
    if (p->textured)
    {
        static u32 texEpoch = ~0u, texTpage = ~0u, texClut = ~0u, texRaw = ~0u, texFilt = ~0u;
        if (texEpoch != s_Epoch || texTpage != (p->tpage & 0x19F) || texClut != (p->clut & 0xFFFF) ||
            texRaw != p->raw || texFilt != p->filt || !s_RegValid[R_TEX0])
        {
            LP_BEGIN(LP_STATE_TEX)
            PROF_BEGIN("gs: prim_state (texture)")
            tex_state(p);
            PROF_END("gs: prim_state (texture)")
            LP_END(LP_STATE_TEX);
            texEpoch = s_Epoch;
            texTpage = p->tpage & 0x19F;
            texClut  = p->clut & 0xFFFF;
            texRaw   = p->raw;
            texFilt  = p->filt;
            if (memoEpoch != s_Epoch)
            {
                /* A cache miss read the hires area: its scale-down (hr_blit) rewrote the frame buffer,
                 * scissor and test registers. */
                memoPrim  = state_other(p);
                memoEpoch = s_Epoch;
                tex_state(p); /* state_other leaves TEX0 alone; this re-sets it (CLAMP included above) */
                texEpoch = s_Epoch;
            }
        }
    }
    return memoPrim;
}

/** TEX0/TEXCLUT for a textured primitive (tpage: page and depth; clut; raw). The register values
 * are memoized by page, CLUT and raw until a cache slot changes (s_TexGen): a hit skips the page and
 * CLUT lookups (s_ClutMap is 64 KB, a cache miss each time) but touches the same LRU clocks, so the
 * caches evict as before. */
static void tex_state(const Prim* p)
{
    static struct
    {
        u32  key, gen;
        s32  slot, row; /* page cache slot and CLUT row (4/8-bit pages) */
        u64x tex0, texclut;
    } memo[64] SPR;
    u32  depth = (p->tpage >> 7) & 3;
    u32  clut  = depth >= 2 ? 0 : p->clut & 0xFFFF;
    u32  key   = (p->tpage & 0x19F) | (clut << 9) | (p->raw << 25);
    u64x tex0;
    __typeof__(&memo[0]) e = &memo[(clut ^ (clut >> 6) ^ ((p->tpage & 0x19F) * 5)) & 63];

    if (e->gen == s_TexGen && e->key == key)
    {
        if (depth < 2)
        {
            s_Slots[e->slot].used = ++s_UseClock; /* as cache_get and clut_get on a hit */
            s_Cluts[e->row].used  = ++s_UseClock;
        }
    }
    else if (depth >= 2)
    {
        u32 px = (p->tpage & 15) * 64, py = ((p->tpage >> 4) & 1) * 256;
        e->tex0 = (u64x)(VRAM_TBP + ((py / 64) * VRAM_BW + px / 64) * 32) | ((u64x)VRAM_BW << 14) |
                  ((u64x)PSM_CT16 << 20);
        e->tex0 |= (8ULL << 26) | (8ULL << 30) | (1ULL << 34) | ((u64x)(p->raw ? 1 : 0) << 35); /* 256x256, TCC, TFX */
        if (p->hr)
        {
            /* The page's 256 << s_HrSh square in the shadow buffer (GS pages: 64 wide; 64 or 32 high). A
             * 32-bit buffer is read as PSMCT24, alpha from TEXA as a 16-bit page's: black is transparent
             * (AEM), the mask bit (semi-transparency) is lost. */
            u32 sx = px << s_HrSh, sy = py << s_HrSh;
            e->tex0 = (u64x)(HIRES_TBP + ((sy / HR_PAGE_H) * HIRES_BW + sx / 64) * 32) | ((u64x)HIRES_BW << 14) |
                      ((u64x)(s_HrPsm == PSM_CT32 ? 0x01 : PSM_CT16) << 20) | ((u64x)(8 + s_HrSh) << 26) |
                      ((u64x)(8 + s_HrSh) << 30) | (1ULL << 34) | ((u64x)(p->raw ? 1 : 0) << 35);
        }
        e->key = key;
        e->gen = s_TexGen;
    }
    else
    {
        u32 tbp = cache_get(depth, p->tpage);
        e->slot    = s_PageMap[(depth << 5) | (p->tpage & 31)] - 1;
        e->row     = (s32)clut_get(p->clut, depth);
        e->texclut = 4 | ((u64x)e->row << 12); /* CBW 4 (256), COU 0, COV = row */
        e->tex0 = (u64x)tbp | ((u64x)4 << 14) | ((u64x)(depth == 0 ? PSM_T4 : PSM_T8) << 20) |
                  ((u64x)(CLUT_ADDR / 256) << 37) | ((u64x)PSM_CT16 << 51) | (1ULL << 55) | (1ULL << 61); /* CSM2, CLD */
        e->tex0 |= (8ULL << 26) | (8ULL << 30) | (1ULL << 34) | ((u64x)(p->raw ? 1 : 0) << 35); /* 256x256, TCC, TFX */
        e->key = key;
        e->gen = s_TexGen; /* after the lookups, which may have changed slots */
    }
    tex0 = e->tex0;
    if (depth < 2)
    {
        u64x texclut = p->filt ? filt_get(p->clut, depth, p->filt) : e->texclut;
        if (!s_RegValid[R_TEXCLUT] || s_Reg[R_TEXCLUT] != texclut)
        {
            s_RegValid[R_TEX0] = 0; /* the CLUT is loaded when TEX0 is written */
        }
        set(R_TEXCLUT, texclut);
    }
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
}

/** Everything but the texture registers. */
static u64x state_other(const Prim* p)
{
    u64x prim = (u64x)(p->gouraud << 3) | (1 << 8); /* IIP, FST (UV coordinates) */
    u32  test = (1 << 16) | (1 << 17);            /* ZTE on, ZTST always */

    base_state();
    if (s_Sh)
    {
        scissor(s_AreaX1 << s_HrSh, s_AreaY1 << s_HrSh, ((s_AreaX2 + 1) << s_HrSh) - 1, ((s_AreaY2 + 1) << s_HrSh) - 1);
        set(R_XYOFFSET, (u64x)(((2048 - (s_OffX << s_HrSh)) << 4) & 0xFFFF) |
                            ((u64x)(((2048 - (s_OffY << s_HrSh)) << 4) & 0xFFFF) << 32));
        hr_mark(s_AreaX1, s_AreaY1, s_AreaX2, s_AreaY2); /* VRAM's copy of the clip area is stale from here */
    }
    else
    scissor(s_AreaX1, s_AreaY1, s_AreaX2, s_AreaY2);

    if (p->textured)
    {
        /* Texture window: REGION_REPEAT, (u & mask) | offset; for hires pages in doubled texels,
         * (2u + f & 2 mask + 1) | 2 offset. */
        u32 mu = 0xFF & ~(s_TwMaskX * 8), fu = (s_TwOffX & s_TwMaskX) * 8;
        u32 mv = 0xFF & ~(s_TwMaskY * 8), fv = (s_TwOffY & s_TwMaskY) * 8;
        if (p->hr && s_HrSh)
        {
            mu = mu * 2 + 1, fu *= 2, mv = mv * 2 + 1, fv *= 2;
        }
        prim |= 1 << 4; /* TME */
        set(R_CLAMP, 3 | (3 << 2) | ((u64x)mu << 4) | ((u64x)fu << 14) | ((u64x)mv << 24) | ((u64x)fv << 34));
        test |= 1 | (7 << 1); /* alpha test: drop alpha 0 (texel 0x0000) */
    }
    if (s_CheckMask)
    {
        test |= 1 << 14; /* DATE: only where the destination's bit 15 is clear */
    }
    set(R_TEST, test);
    set(R_FBA, s_SetMask);
    set(R_DTHE, p->dither && !(s_Sh && s_HrPsm == PSM_CT32)); /* 32-bit: no dithering (true colour) */

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
    static SPR u64x regsTab[2][5];
    static SPR int  nregTab[2][5];
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
        *q++ = (u64x)(HR_X(v[i]->x) << 4 & 0xFFFF) | ((u64x)(HR_Y(v[i]->y) << 4 & 0xFFFF) << 16);
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
    static SPR u64x regsTab[2][5];
    static SPR int  nregTab[2][5];
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

/* Fog pair counts (GpuGs_Stats), a diagnostic of the pair fusion: SH1_PROF builds only, as the
 * per-polygon comparison costs ~5% of DrawOTag. */
#ifdef SH_PORT_PROF
#define GS_FOG_PAIR_STATS 1
#endif
static u32 s_StFogPair[2][4];

/** A polygon's GS primitives: a quad as one strip, or its halves (or a triangle) when they fit what
 * the GPU draws (1023x511). */
static void emit_poly(int textured, u64x prim, const u64x* rgbaq, const u64x* uv, const u64x* xyz, const s32* x,
                      const s32* y, s32 nv)
{
    if (nv == 4)
    {
        int big0, big1;
        big0 = too_big_xy(x, y, 0, 1, 2);
        big1 = too_big_xy(x, y, 1, 2, 3);
        if (!big0 && !big1)
        {
            emit_packed(textured, prim | 4, rgbaq, uv, xyz, 4); /* both halves as one strip */
        }
        else
        {
            if (!big0)
            {
                emit_packed(textured, prim | 3, rgbaq, uv, xyz, 3);
            }
            if (!big1)
            {
                emit_packed(textured, prim | 3, rgbaq + 1, uv + 1, xyz + 1, 3);
            }
        }
    }
    else if (!too_big_xy(x, y, 0, 1, 2))
    {
        emit_packed(textured, prim | 3, rgbaq, uv, xyz, 3);
    }
}

static void emit_poly(int textured, u64x prim, const u64x* rgbaq, const u64x* uv, const u64x* xyz, const s32* x,
                      const s32* y, s32 nv);

/** Space in the packet for one quad strip (PRIM and 4 vertices: RGBAQ, [UV], XYZ2). */
static inline u64x* quad_loop(int textured)
{
    static SPR u64x regs[2];
    static SPR int  nreg[2];
    if (!nreg[textured])
    {
        regs[textured] = vertex_regs(textured, 4, &nreg[textured]);
    }
    s_StPrims++;
    return reglist(regs[textured], nreg[textured]);
}

/** Whether the quad's bounding box fits what the GPU draws (1023x511): then both halves do. */
static inline int quad_fits(const s32* x, const s32* y)
{
    s32 x0 = x[0] < x[1] ? x[0] : x[1], x1 = x[0] < x[1] ? x[1] : x[0];
    s32 y0 = y[0] < y[1] ? y[0] : y[1], y1 = y[0] < y[1] ? y[1] : y[0];
    x0 = x[2] < x0 ? x[2] : x0, x1 = x[2] > x1 ? x[2] : x1, x0 = x[3] < x0 ? x[3] : x0, x1 = x[3] > x1 ? x[3] : x1;
    y0 = y[2] < y0 ? y[2] : y0, y1 = y[2] > y1 ? y[2] : y1, y0 = y[3] < y0 ? y[3] : y0, y1 = y[3] > y1 ? y[3] : y1;
    return x1 - x0 <= 1023 && y1 - y0 <= 511;
}

#ifdef SH_PORT_SCREENREAD
/* Screen-read inventory (SH1_SCREENREAD=1): every use of the screen's contents, logged once per
 * distinct case with a count, to find the code that a 2x render buffer would have to handle: textured
 * primitives whose texels come from a screen area (their UV range, not the whole page), VRAM copies
 * from or into one, and LoadImage/StoreImage on one (with the game's caller). Screen areas: the last
 * two display areas (GpuGs_DisplayShow), i.e. the double-buffered frames. */
unsigned int g_PortGpuCaller; /* libgpu_port.c: the game's caller of LoadImage/StoreImage */
unsigned int g_PortSrContext; /* libgpu_port.c: gameState << 16 | gameStateSteps[0], at each DrawOTag */
static s32   s_ScrRect[2][4];
static int   s_ScrCount;
static u32   s_SrFrame;

typedef struct
{
    u32 kind, a, b, c, count;
} SrKey;

static SrKey s_SrKeys[1024];
static int   s_SrN;
static const char* const SR_KIND[] = { "", "polygon texels", "sprite texels", "copy from screen", "copy into screen",
                                       "LoadImage into screen", "StoreImage of screen" };

static int sr_hit(s32 x0, s32 y0, s32 x1, s32 y1)
{
    int i;
    for (i = 0; i < s_ScrCount; i++)
    {
        const s32* r = s_ScrRect[i];
        if (x0 < r[0] + r[2] && r[0] < x1 && y0 < r[1] + r[3] && r[1] < y1)
        {
            return 1;
        }
    }
    return 0;
}

static void sr_note(u32 kind, u32 a, u32 b, u32 c, s32 x0, s32 y0, s32 x1, s32 y1)
{
    int i;
    for (i = 0; i < s_SrN; i++)
    {
        SrKey* k = &s_SrKeys[i];
        if (k->kind == kind && k->a == a && k->b == b && k->c == c)
        {
            k->count++;
            return;
        }
    }
    if (s_SrN >= (int)(sizeof(s_SrKeys) / sizeof(s_SrKeys[0])))
    {
        static int full;
        if (!full++)
        {
            printf("screenread: case table full (%d cases); later new cases aren't logged\n", s_SrN);
        }
        return;
    }
    {
        SrKey* k = &s_SrKeys[s_SrN++];
        k->kind  = kind;
        k->a     = a;
        k->b     = b;
        k->c     = c;
        k->count = 1;
    }
    printf("screenread: %s: %08x %08x %08x, VRAM (%d,%d)-(%d,%d); frame %u; screen %d,%d %dx%d; game state %u step %u\n",
           SR_KIND[kind], a, b, c, x0, y0, x1, y1, s_SrFrame, s_ScrRect[0][0], s_ScrRect[0][1], s_ScrRect[0][2],
           s_ScrRect[0][3], g_PortSrContext >> 16, g_PortSrContext & 0xFFFF);
}

/** A textured primitive's texel rectangle: page `tpage`, UVs u0-u1, v0-v1 (inclusive). */
static void sr_texels(u32 kind, u32 cmd, u32 tpage, u32 clut, u32 u0, u32 u1, u32 v0, u32 v1)
{
    u32 depth = (tpage >> 7) & 3, div = depth == 0 ? 4 : depth == 1 ? 2 : 1;
    s32 px = (s32)(tpage & 15) * 64, py = (s32)((tpage >> 4) & 1) * 256;
    s32 x0 = px + (s32)(u0 / div), x1 = px + (s32)(u1 / div) + 1, y0 = py + (s32)v0, y1 = py + (s32)v1 + 1;
    if (sr_hit(x0, y0, x1, y1))
    {
        sr_note(kind, cmd, tpage & 0x1FF, clut, x0, y0, x1, y1);
    }
}

static void sr_screen(s32 x, s32 y, s32 w, s32 h)
{
    s_SrFrame++;
    if (s_ScrCount && s_ScrRect[0][0] == x && s_ScrRect[0][1] == y && s_ScrRect[0][2] == w && s_ScrRect[0][3] == h)
    {
        return;
    }
    s_ScrRect[1][0] = s_ScrRect[0][0];
    s_ScrRect[1][1] = s_ScrRect[0][1];
    s_ScrRect[1][2] = s_ScrRect[0][2];
    s_ScrRect[1][3] = s_ScrRect[0][3];
    s_ScrRect[0][0] = x;
    s_ScrRect[0][1] = y;
    s_ScrRect[0][2] = w;
    s_ScrRect[0][3] = h;
    if (s_ScrCount < 2)
    {
        s_ScrCount++;
    }
}

/** Every 30 s of frames: the cases seen so far and how often. */
static void sr_summary(void)
{
    int i;
    if ((s_SrFrame % 1800) != 0 || !s_SrN)
    {
        return;
    }
    printf("screenread summary (frame %u): %d cases\n", s_SrFrame, s_SrN);
    for (i = 0; i < s_SrN; i++)
    {
        SrKey* k = &s_SrKeys[i];
        printf("screenread summary:   %-22s %08x %08x %08x  x%u\n", SR_KIND[k->kind], k->a, k->b, k->c, k->count);
    }
}
#endif

/* Exact blending for semi-transparency modes 0, 2 and 3 (blend_exact). The PS1 dithers a polygon's
 * colour down to 5 bits and blends in 5 bits; the GS blends in 8 bits and dithers the result (1-2
 * levels off in these modes: gpu_test.py). The source is drawn first into a scratch area (the last
 * page cache slot, 256x128 PSMCT16), dithered as on the PS1, then composited onto the frame buffer:
 * a source in exact multiples of 8 makes the GS's blend equal the PS1's in all four modes. Mode 1
 * keeps the direct path (exact but for faint colours: filt_get). */
#define BX_W 256
#define BX_H 128
#define BX_ADDR ((CACHE2_ADDR + (CACHE_SLOTS - 1 - 15) * CACHE_SLOT))

/** Whether the polygon's clipped bounding box fits the scratch area (origin aligned to 4 pixels, so
 * the dither pattern lines up); its box. */
static int blend_exact_box(const s32* x, const s32* y, s32 nv, s32* bx, s32* by, s32* bw, s32* bh)
{
    s32 i, x0 = x[0], x1 = x[0], y0 = y[0], y1 = y[0];
    for (i = 1; i < nv; i++)
    {
        x0 = x[i] < x0 ? x[i] : x0;
        x1 = x[i] > x1 ? x[i] : x1;
        y0 = y[i] < y0 ? y[i] : y0;
        y1 = y[i] > y1 ? y[i] : y1;
    }
    x0 = x0 > s_AreaX1 ? x0 : s_AreaX1;
    y0 = y0 > s_AreaY1 ? y0 : s_AreaY1;
    x1 = x1 < s_AreaX2 ? x1 : s_AreaX2;
    y1 = y1 < s_AreaY2 ? y1 : s_AreaY2;
    if (x1 < x0 || y1 < y0)
    {
        return 0;
    }
    *bx = x0 & ~3;
    *by = y0 & ~3;
    *bw = x1 + 1 - *bx;
    *bh = y1 + 1 - *by;
    return *bw <= BX_W && *bh <= BX_H;
}

static void blend_exact(const Prim* p, u64x prim, const u64x* rgbaq, const u64x* uv, const u64x* xyz, const s32* x,
                        const s32* y, s32 nv, s32 bx, s32 by, s32 bw, s32 bh)
{
    static const u64x MODES[4] = {
        0 | (1 << 2) | (2 << 4) | (1 << 6) | (0x40ULL << 32), /* (Cs - Cd) * 0.5 + Cd */
        0 | (2 << 2) | (2 << 4) | (1 << 6) | (0x80ULL << 32), /* Cs + Cd */
        1 | (0 << 2) | (2 << 4) | (2 << 6) | (0x80ULL << 32), /* Cd - Cs */
        0 | (2 << 2) | (2 << 4) | (1 << 6) | (0x20ULL << 32), /* Cs * 0.25 + Cd */
    };
    u64x test  = s_Reg[R_TEST] & ~(u64x)0xFFFF; /* the depth test bits, no alpha or destination test */
    u64x plain = prim & ~(u64x)(1 << 6);        /* no ABE */
    s32  sx0, sy0, sx1, sy1;

    if (p->textured)
    {
        /* 1. Texels without the semi-transparency bit (alpha 0x40): drawn as they are. */
        set(R_TEST, test | 1 | (4 << 1) | (0x40 << 4) | ((u64x)(s_CheckMask != 0) << 14));
        emit_poly(1, plain, rgbaq, uv, xyz, x, y, nv);
    }

    /* 2. The rest into the scratch area, dithered as on the PS1, bit 15 set where drawn. */
    sx0 = (s_AreaX1 > bx ? s_AreaX1 : bx) - bx;
    sy0 = (s_AreaY1 > by ? s_AreaY1 : by) - by;
    sx1 = (s_AreaX2 < bx + bw - 1 ? s_AreaX2 : bx + bw - 1) - bx;
    sy1 = (s_AreaY2 < by + bh - 1 ? s_AreaY2 : by + bh - 1) - by;
    set(R_FRAME, (u64x)(BX_ADDR / 8192) | ((u64x)(BX_W / 64) << 16) | ((u64x)PSM_CT16 << 24));
    set(R_XYOFFSET, (u64x)((2048 + bx) << 4) | ((u64x)((2048 + by) << 4) << 32));
    scissor(0, 0, bw - 1, bh - 1);
    set(R_TEST, test);
    set(R_FBA, 0);
    set(R_DTHE, 0);
    ad(R_PRIM, 6);
    ad(R_RGBAQ, 0x3F800000ULL << 32);
    ad(R_XYZ2, (u64x)((2048 + bx) << 4) | ((u64x)((2048 + by) << 4) << 16));
    ad(R_XYZ2, (u64x)((2048 + bx + bw) << 4) | ((u64x)((2048 + by + bh) << 4) << 16));
    scissor(sx0, sy0, sx1, sy1);
    set(R_TEST, p->textured ? test | 1 | (4 << 1) | (0x80 << 4) : test);
    set(R_FBA, 1);
    set(R_DTHE, p->dither);
    emit_poly((int)p->textured, plain, rgbaq, uv, xyz, x, y, nv);

    /* 3. The scratch area composited onto the frame buffer: the PS1's blend, exact. */
    ad(R_TEXFLUSH, 0);
    set(R_FRAME, (u64x)VRAM_FBP | ((u64x)VRAM_BW << 16) | ((u64x)PSM_CT16 << 24));
    set(R_XYOFFSET, (u64x)(2048 << 4) | ((u64x)(2048 << 4) << 32));
    scissor(s_AreaX1, s_AreaY1, s_AreaX2, s_AreaY2);
    set(R_TEST, test | 1 | (7 << 1) | ((u64x)(s_CheckMask != 0) << 14)); /* drop alpha 0: not drawn */
    set(R_FBA, s_SetMask);
    set(R_DTHE, 0);
    set(R_ALPHA, MODES[p->semiMode]);
    set(R_PABE, 0);
    set(R_TEX1, 0);
    set(R_CLAMP, 0);
    set(R_TEX0, (u64x)(BX_ADDR / 256) | ((u64x)(BX_W / 64) << 14) | ((u64x)PSM_CT16 << 20) | (8ULL << 26) | (7ULL << 30) |
                    (1ULL << 34) | (1ULL << 35)); /* 256x128, TCC, DECAL */
    if (!p->textured)
    {
        set(R_TEXA, 0x40 | (1 << 15) | ((u64x)0x40 << 32)); /* untextured: bit 15 not written (as the PS1) */
    }
    ad(R_PRIM, 6 | (1 << 4) | (1 << 6) | (1 << 8));
    ad(R_RGBAQ, 0x80808080ULL | (0x3F800000ULL << 32));
    ad(R_UV, 0);
    ad(R_XYZ2, (u64x)((2048 + bx) << 4) | ((u64x)((2048 + by) << 4) << 16));
    ad(R_UV, (u64x)(bw << 4) | ((u64x)(bh << 4) << 16));
    ad(R_XYZ2, (u64x)((2048 + bx + bw) << 4) | ((u64x)((2048 + by + bh) << 4) << 16));
    set(R_TEXA, 0x40 | (1 << 15) | ((u64x)0x80 << 32)); /* back to init()'s */
    s_Epoch++; /* every memo of the registers set above */
    s_StateGen++;
    s_RegValid[R_TEX0] = 0;
}

/** A GP0 polygon; `under`: a fog pair's underlay to draw first (GpuGs_Commands), or NULL. `shape`:
 * the command's gouraud, quad and textured bits (cmd & 0x1C), a constant in each copy (polygon()), so
 * the vertex loops unroll and the per-vertex branches go. */
static inline __attribute__((always_inline)) u32 polygon_t(const u32* w, s32 n, const u32* under, u32 shape)
{
    u32  cmd      = (w[0] >> 24 & ~0x1Cu) | shape;
    s32  nv       = (shape & 0x08) ? 4 : 3;
    u32  gouraud  = (shape >> 4) & 1;
    u32  textured = (shape >> 2) & 1;
    u32  raw      = (cmd & 1) && textured;
    s32  words    = nv * (1 + (s32)textured) + (gouraud ? nv : 1);
    u32  rgb[4], clut = 0, tpage = s_TexPage, colorMask = 0xFFFFFF;
    s32  x[4], y[4], i, k = 0, bxx, byy, bww, bhh;
    u64x rgbaq[4], uv[4], xyz[4], prim, a;
    Prim p;
#ifdef SH_PORT_SCREENREAD
    u32 srU0 = 255, srU1 = 0, srV0 = 255, srV1 = 0;
#endif
    LP_BEGIN(LP_POLY_PARSE)

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
#ifdef SH_PORT_SCREENREAD
            srU0 = (t & 0xFF) < srU0 ? (t & 0xFF) : srU0;
            srU1 = (t & 0xFF) > srU1 ? (t & 0xFF) : srU1;
            srV0 = ((t >> 8) & 0xFF) < srV0 ? ((t >> 8) & 0xFF) : srV0;
            srV1 = ((t >> 8) & 0xFF) > srV1 ? ((t >> 8) & 0xFF) : srV1;
#endif
            /* + half a texel: the PS1 rounds its interpolated texture coordinates to the nearest texel,
             * the GS truncates (the same texels at 1:1 and magnified; minified or rotated, the GS
             * otherwise took the texel before half the time: gpu_test.py --uv). */
            uv[i] = (u64x)(((t & 0xFF) << 4) + 8) | ((u64x)((((t >> 8) & 0xFF) << 4) + 8) << 16);
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
        s_TexPage = (tpage & ~0x400u) | (s_TexPage & 0x400); /* textured polygons also set the current texture page (DFE stays: E1 only) */
    }
    p.semiMode = (tpage >> 5) & 3;
    p.hr       = textured && HR_TEX((tpage >> 7) & 3, tpage);
    if (p.hr)
    {
        for (i = 0; i < nv; i++)
        {
            uv[i] <<= s_HrSh; /* both fields: u fits its 14 bits doubled */
        }
    }
#ifdef SH_PORT_SCREENREAD
    if (textured)
    {
        sr_texels(1, cmd, tpage, clut, srU0, srU1, srV0, srV1);
    }
#endif
#ifdef GS_FOG_PAIR_STATS
    {

/* Fog pairs (PS2 Optimizations.md, option 1): a plain gouraud polygon followed by a
         * semi-transparent textured one on the same corners, counted by the textured one's blend
         * mode and whether the plain one was itself semi-transparent. */
        static s32 px[4], py[4], pnv, pplain, psemi;
        if (textured && p.semi && pplain && pnv == nv && !memcmp(px, x, nv * 4) && !memcmp(py, y, nv * 4))
        {
            s_StFogPair[psemi][p.semiMode]++;
        }
        pplain = !textured && gouraud;
        psemi  = p.semi;
        pnv    = nv;
        memcpy(px, x, nv * 4);
        memcpy(py, y, nv * 4);
    }
#endif
    p.dither   = ((s_TexPage >> 9) & 1) && (gouraud || (textured && !raw));
    if (p.dither && p.semi)
    {
        /* The PS1 dithers a semi-transparent primitive's colour before blending (clamped at 0), the
         * GS the blended result: they agree unless the colour is under 4, where the PS1 adds nothing
         * but the GS drops the matrix's negative cells a level (the flashlight's black quad in front
         * of Harry: a checkered rectangle). Such faint primitives are drawn undithered: below 4
         * (modulated texels: up to 1.94x, so 2), nothing to add either way. */
        u32 low = textured ? 3 : 4, faint = 1;
        for (i = 0; i < nv; i++)
        {
            faint &= (rgb[i] & 0xFF) < low && ((rgb[i] >> 8) & 0xFF) < low && (rgb[i] >> 16) < low;
        }
        p.dither = !faint;
    }
    p.filt = 0;
    if (p.dither && p.semi && textured && p.semiMode && ((tpage >> 7) & 3) < 2)
    {
        u32 m = 0;
        for (i = 0; i < nv; i++)
        {
            m = ((rgb[i] & 0xFF) > (m & 0xFF) ? rgb[i] & 0xFF : m & 0xFF) |
                ((rgb[i] & 0xFF00) > (m & 0xFF00) ? rgb[i] & 0xFF00 : m & 0xFF00) |
                ((rgb[i] & 0xFF0000) > (m & 0xFF0000) ? rgb[i] & 0xFF0000 : m & 0xFF0000);
        }
        p.filt = filt_canon(m, p.semiMode == 3) | (1u << 25); /* bit 25: never 0 */
        if (s_FiltPass2)
        {
            p.dither = 0;
            p.filt |= 1u << 27; /* the complement: only the left-out texels, black (polygon()) */
        }
        else
        {
            s_FiltDrawn = 1;
        }
    }
    if (!textured && !gouraud && !p.dither)
    {
        colorMask = HR_TRUECOLOR ? 0xFFFFFF : 0xF8F8F8; /* as quantize() */
    }
    a = (textured ? 0x80ULL << 24 : 0) | (0x3F800000ULL << 32);
    if (raw)
    {
        colorMask = 0; /* RGBAQ 0x808080 (DECAL ignores it) */
        a |= 0x808080;
    }
    LP_END(LP_POLY_PARSE);
    {
        LP_BEGIN(LP_POLY_STATE)
        prim = prim_state(&p);
        LP_END(LP_POLY_STATE);
    }
    {
    LP_BEGIN(LP_POLY_EMIT)
    s_StDate += s_CheckMask * (nv == 4 ? 2 : 1);
    s_StRtTex += (textured && ((tpage >> 7) & 3) >= 2) * (nv == 4 ? 2 : 1);
    if (p.semi && p.semiMode != 1 && !under && s_HrLayout == 0 && !s_Sh &&
        blend_exact_box(x, y, nv, &bxx, &byy, &bww, &bhh))
    {
        for (i = 0; i < nv; i++)
        {
            rgbaq[i] = (u64x)(rgb[i] & colorMask) | a;
            xyz[i]   = (u64x)(HR_X(x[i]) << 4 & 0xFFFF) | ((u64x)(HR_Y(y[i]) << 4 & 0xFFFF) << 16);
        }
        if (nv == 3)
        {
            blend_exact(&p, prim, rgbaq, uv, xyz, x, y, 3, bxx, byy, bww, bhh);
        }
        else
        {
            /* A quad as the PS1 draws it, triangle (0, 1, 2) then (1, 2, 3), each blended on its own:
             * where they overlap, the PS1 blends twice. */
            for (i = 0; i < 2; i++)
            {
                if (blend_exact_box(x + i, y + i, 3, &bxx, &byy, &bww, &bhh))
                {
                    prim = prim_state(&p); /* the composite left the scratch area as the texture */
                    blend_exact(&p, prim, rgbaq + i, uv + i, xyz + i, x + i, y + i, 3, bxx, byy, bww, bhh);
                }
            }
        }
    }
    else if (nv == 4 && quad_fits(x, y))
    {
        /* The common case: the quad as one strip, written straight into the packet; a fog pair's
         * underlay first, in the same (textured) register layout so both share one GIF tag. The
         * same register values as emit_poly below. */
        u64x* o;
        for (i = 0; i < 4; i++)
        {
            xyz[i] = (u64x)(HR_X(x[i]) << 4 & 0xFFFF) | ((u64x)(HR_Y(y[i]) << 4 & 0xFFFF) << 16);
        }
        if (under)
        {
            s_StDate += s_CheckMask * 2;
            o    = quad_loop(1);
            *o++ = (prim & ~(u64x)((1 << 4) | (1 << 6))) | 4;
            for (i = 0; i < 4; i++)
            {
                *o++ = (u64x)(under[i * 2] & 0xFFFFFF) | (0x40ULL << 24) | (0x3F800000ULL << 32);
                *o++ = uv[i];
                *o++ = xyz[i];
            }
        }
        o    = quad_loop((int)textured);
        *o++ = prim | 4;
        if (textured)
        {
            for (i = 0; i < 4; i++)
            {
                *o++ = (u64x)(rgb[i] & colorMask) | a;
                *o++ = uv[i];
                *o++ = xyz[i];
            }
        }
        else
        {
            for (i = 0; i < 4; i++)
            {
                *o++ = (u64x)(rgb[i] & colorMask) | a;
                *o++ = xyz[i];
            }
        }
    }
    else
    {
    for (i = 0; i < nv; i++)
    {
        rgbaq[i] = (u64x)(rgb[i] & colorMask) | a;
        xyz[i]   = (u64x)(HR_X(x[i]) << 4 & 0xFFFF) | ((u64x)(HR_Y(y[i]) << 4 & 0xFFFF) << 16);
    }
    if (under)
    {
        /* The fog pair's underlay (GpuGs_Commands): an opaque gouraud quad on the same corners, drawn
         * first under this one's state. Only the PRIM bits differ (no TME, no ABE), so nothing is
         * switched between the two. Its alpha is 0x40 rather than 0: it passes the textured state's
         * alpha test (alpha != 0), and the frame buffer still stores bit 15 = 0 (alpha's top bit), as
         * an untextured polygon does (with set-mask, FBA sets it either way). Same picture as two
         * separate polygons. */
        u64x urgbaq[4];
        for (i = 0; i < 4; i++)
        {
            urgbaq[i] = (u64x)(under[i * 2] & 0xFFFFFF) | (0x40ULL << 24) | (0x3F800000ULL << 32);
        }
        s_StDate += s_CheckMask * 2;
        /* In the textured register layout (UV written, ignored without TME), so the pair shares
         * one GIF tag instead of switching layouts twice a pair. */
        emit_poly(1, prim & ~(u64x)((1 << 4) | (1 << 6)), urgbaq, uv, xyz, x, y, 4);
    }
    emit_poly((int)textured, prim, rgbaq, uv, xyz, x, y, nv);
    }
    LP_END(LP_POLY_EMIT);
    }
    return (u32)k;
}

static u32 polygon_sw(const u32* w, s32 n, const u32* under)
{
    switch ((w[0] >> 24) & 0x1C)
    {
        case 0x00: return polygon_t(w, n, under, 0x00);
        case 0x04: return polygon_t(w, n, under, 0x04);
        case 0x08: return polygon_t(w, n, under, 0x08);
        case 0x0C: return polygon_t(w, n, under, 0x0C);
        case 0x10: return polygon_t(w, n, under, 0x10);
        case 0x14: return polygon_t(w, n, under, 0x14);
        case 0x18: return polygon_t(w, n, under, 0x18);
        default:   return polygon_t(w, n, under, 0x1C);
    }
}

/** A GP0 polygon. One drawn with a filtered CLUT (filt_get) is drawn again, undithered, with the
 * texels the filter left out as black semi-transparent ones: they add nothing, as on the PS1, but
 * the pixels are written (their mask bit, bit 15, set from the texel as the PS1 does). */
static u32 polygon(const u32* w, s32 n, const u32* under) __attribute__((noinline));
static u32 polygon(const u32* w, s32 n, const u32* under)
{
    u32 r;
    s_FiltDrawn = 0;
    r           = polygon_sw(w, n, under);
    if (s_FiltDrawn)
    {
        s_FiltPass2 = 1;
        polygon_sw(w, n, NULL);
        s_FiltPass2 = 0;
    }
    return r;
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
    p.hr       = 0;
    p.filt     = 0;

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
        if (!HR_TRUECOLOR)
        {
            quantize(&a, 1);
        }
    }
    b   = a;
    b.x = a.x + rw;
    b.y = a.y + rh;
    b.u = a.u + rw;
    b.v = a.v + rh;
    p.hr = p.textured && HR_TEX((p.tpage >> 7) & 3, p.tpage);
    if (p.hr)
    {
        a.u <<= s_HrSh, a.v <<= s_HrSh, b.u <<= s_HrSh, b.v <<= s_HrSh;
    }
#ifdef SH_PORT_SCREENREAD
    if (p.textured)
    {
        sr_texels(2, cmd, p.tpage, p.clut, a.u, (a.u + rw - 1) & 0xFF, a.v, (a.v + rh - 1) & 0xFF);
    }
#endif
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
    p.hr       = 0;
    p.filt     = 0;
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
    u32 sh = s_Sh; /* restored at the end */
    if (s_Hires && x < HR_W && y < HR_H)
    {
        /* The hires buffer's part, then VRAM's (base_state's FRAME follows s_Sh). */
        s32 x1 = x + w < HR_W ? x + w : HR_W, y1 = y + h < HR_H ? y + h : HR_H;
        s_Sh   = 1;
        base_state();
        scissor(0, 0, (HR_W << s_HrSh) - 1, (HR_H << s_HrSh) - 1);
        set(R_TEST, (1 << 16) | (1 << 17));
        set(R_FBA, 0);
        set(R_DTHE, 0);
        SCANMSK_CHECK();
        read_rgb(rgb, &a);
        a.x = x, a.y = y, a.u = a.v = 0;
        b   = a;
        b.x = x1, b.y = y1;
        {
            /* VRAM coordinates: no drawing offset (HR_X), XYOFFSET as base_state set it. */
            const Vtx* v[2] = { &a, &b };
            s32        ox = s_OffX, oy = s_OffY;
            s_OffX = s_OffY = 0;
            emit(&p, 6 | (1 << 8), v, 2);
            s_OffX = ox, s_OffY = oy;
        }
    }
    s_Sh = 0;
    base_state();
    scissor(0, 0, VRAM_W - 1, VRAM_H - 1);
    set(R_TEST, (1 << 16) | (1 << 17));
    set(R_FBA, 0);
    set(R_DTHE, 0);
    SCANMSK_CHECK();
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
    cache_fill(x, y, w, h, (u16)(((rgb & 0xFF) >> 3) | ((((rgb >> 8) & 0xFF) >> 3) << 5) | ((((rgb >> 16) & 0xFF) >> 3) << 10)));
    s_Sh = sh;
    s_Epoch++; /* base_state's memo: FRAME is VRAM's, whatever s_Sh says */
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
#ifdef SH_PORT_SCREENREAD
    if (sr_hit(sx, sy, sx + w, sy + h))
    {
        sr_note(3, 0, (u32)dx | ((u32)dy << 16), (u32)w | ((u32)h << 16), sx, sy, sx + w, sy + h); /* by destination: a moving source (the lens flare test) is one case */
    }
    if (sr_hit(dx, dy, dx + w, dy + h))
    {
        sr_note(4, (u32)sx | ((u32)sy << 16), (u32)dx | ((u32)dy << 16), (u32)w | ((u32)h << 16), dx, dy, dx + w, dy + h);
    }
#endif
    if (sx + w > VRAM_W || dx + w > VRAM_W || sy + h > VRAM_H || dy + h > VRAM_H)
    {
        return; /* TODO: wrapping copies */
    }
    hr_blit(0, sx, sy, w, h); /* a source in the hires area: VRAM brought up to date first */
    ad(R_BITBLTBUF, (u64x)VRAM_TBP | ((u64x)VRAM_BW << 16) | ((u64x)PSM_CT16 << 24) | ((u64x)VRAM_TBP << 32) |
                        ((u64x)VRAM_BW << 48) | ((u64x)PSM_CT16 << 56));
    ad(R_TRXPOS, (u64x)sx | ((u64x)sy << 16) | ((u64x)dx << 32) | ((u64x)dy << 48));
    ad(R_TRXREG, (u64x)w | ((u64x)h << 32));
    ad(R_TRXDIR, 2);
    ad(R_TEXFLUSH, 0);
    cache_invalidate(dx, dy, w, h);
    hr_blit(1, dx, dy, w, h);
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
            if ((s_TexPage ^ w) & 0x400)
            {
                s_MaskDirty = 1; /* DFE */
            }
            s_TexPage = w & 0x3FFF;
            break;
        case 0xE2:
            if (s_TwMaskX != (w & 31) || s_TwMaskY != ((w >> 5) & 31) || s_TwOffX != ((w >> 10) & 31) ||
                s_TwOffY != ((w >> 15) & 31))
            {
                s_TwMaskX = w & 31;
                s_TwMaskY = (w >> 5) & 31;
                s_TwOffX  = (w >> 10) & 31;
                s_TwOffY  = (w >> 15) & 31;
                s_StateGen++;
            }
            break;
        case 0xE3:
            if (s_AreaX1 != (s32)(w & 0x3FF) || s_AreaY1 != (s32)((w >> 10) & 0x1FF))
            {
                s_AreaX1 = w & 0x3FF;
                s_AreaY1 = (w >> 10) & 0x1FF;
                s_StateGen++;
                hr_target();
            }
            break;
        case 0xE4:
            if (s_AreaX2 != (s32)(w & 0x3FF) || s_AreaY2 != (s32)((w >> 10) & 0x1FF))
            {
                s_AreaX2 = w & 0x3FF;
                s_AreaY2 = (w >> 10) & 0x1FF;
                s_StateGen++;
                hr_target();
            }
            break;
        case 0xE5:
            if (s_OffX != sext11(w) || s_OffY != sext11(w >> 11))
            {
                s_OffX = sext11(w);
                s_OffY = sext11(w >> 11);
                if (s_Sh)
                {
                    s_StateGen++; /* XYOFFSET follows the offset at 2x (HR_X) */
                }
            }
            break;
        case 0xE6:
            if (s_SetMask != (w & 1) || s_CheckMask != ((w >> 1) & 1))
            {
                s_SetMask   = w & 1;
                s_CheckMask = (w >> 1) & 1;
                s_StateGen++;
            }
            break;
    }
}

/* Fog pairs (PS2 Optimizations.md, option 1, "pair fusion"): the world mesh draws each fogged face
 * as an opaque gouraud quad (GP0 0x38) and, over it, a semi-transparent textured gouraud quad (0x3E)
 * in additive mode on the same corners, ~460-560 pairs a frame. An opaque gouraud quad is held back
 * until the next command: if that is its textured partner, both are drawn together under one state
 * (polygon()'s `under`), otherwise the held quad is drawn first, as it was. Anything else that draws
 * or reads the GS (pend_flush in the other entry points) draws a held quad first. SH_PORT_FOG_TEST
 * (SH1_FOG_TEST=1) instead drops the underlay: a wrong picture, measuring the most a fog collapse
 * could save. */
static SPR u32 s_FogPend[8];
static SPR s32 s_FogPending;

static int fog_pair(const u32* w, s32 n)
{
    return n >= 12 && (w[0] >> 24) == 0x3E && ((w[5] >> 21) & 3) == 1 && w[1] == s_FogPend[1] && w[4] == s_FogPend[3] &&
           w[7] == s_FogPend[5] && w[10] == s_FogPend[7];
}

/** Draws a held quad (before anything else reaches the GS). */
static void pend_flush(void)
{
    if (s_FogPending)
    {
        s_FogPending = 0;
        polygon(s_FogPend, 8, NULL);
    }
}

/** @brief Executes a stream of GP0 words (one OT packet or DrawPrim). */
static void GpuGs_CommandsImpl(const u32* w, s32 n);
void GpuGs_Commands(const u32* w, s32 n)
{
    LP_BEGIN(LP_COMMANDS)
    GpuGs_CommandsImpl(w, n);
    LP_END(LP_COMMANDS);
}

static void GpuGs_CommandsImpl(const u32* w, s32 n)
{
    init();
    while (n > 0)
    {
        u32 cmd  = w[0] >> 24;
        u32 used = 1;

        if (s_FogPending)
        {
            s_FogPending = 0;
            if (fog_pair(w, n))
            {
#ifdef SH_PORT_FOG_TEST
                s_StFogPair[1][1]++; /* the underlay dropped */
                used = polygon(w, n, NULL);
#else
                PROF_BEGIN("gs: fog pair (both polygons)")
                used = polygon(w, n, s_FogPend);
                PROF_END("gs: fog pair (both polygons)")
#endif
                w += used;
                n -= (s32)used;
                continue;
            }
            polygon(s_FogPend, 8, NULL);
        }
#ifndef SH_PORT_NO_FOG_PAIRS
        if (cmd == 0x38 && n >= 8)
#else
        if (0)
#endif
        {
            memcpy(s_FogPend, w, 32);
            s_FogPending = 1;
            w += 8;
            n -= 8;
            continue;
        }
        if (cmd >= 0x20 && cmd < 0x40)
        {
            PROF_BEGIN("gs: polygon (incl. state)")
            used = polygon(w, n, NULL);
            PROF_END("gs: polygon (incl. state)")
        }
        else if (cmd >= 0x40 && cmd < 0x60)
        {
            LP_BEGIN(LP_LINES)
            used = lines(w, n);
            LP_END(LP_LINES);
        }
        else if (cmd >= 0x60 && cmd < 0x80)
        {
            PROF_BEGIN("gs: rectangle (incl. state)")
            LP_BEGIN(LP_RECT)
            used = rectangle(w, n);
            LP_END(LP_RECT);
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

/** LoadImage: `src` (rows of `stride` pixels) to the VRAM buffer's rectangle. */
void GpuGs_LoadImage(s32 x, s32 y, s32 w, s32 h, const u16* src, s32 stride)
{
#ifdef SH_PORT_SCREENREAD
    if (sr_hit(x, y, x + w, y + h))
    {
        sr_note(5, (u32)x | ((u32)y << 16), (u32)w | ((u32)h << 16), g_PortGpuCaller, x, y, x + w, y + h);
    }
#endif
    pend_flush();
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
        GpuGs_LoadImage(x, y, w1, h1, src, stride);
        if (w1 < w) GpuGs_LoadImage(0, y, w - w1, h1, src + w1, stride);
        if (h1 < h) GpuGs_LoadImage(x, 0, w1, h - h1, src + h1 * stride, stride);
        if (w1 < w && h1 < h) GpuGs_LoadImage(0, 0, w - w1, h - h1, src + h1 * stride + w1, stride);
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
                memcpy(&s_Staging[r * w * 2], &src[(row + r) * stride], w * 2);
            }
            upload(VRAM_TBP, VRAM_BW, PSM_CT16, x, y + row, w, n, s_Staging, (u32)(w * 2 * n));
        }
    }
    else
    {
        /* Other widths: a row at a time, padded to 16 bytes (the GS drops the excess). */
        for (row = 0; row < h; row++)
        {
            u16* dst = (u16*)s_Staging;
            for (col = 0; col < w; col++)
            {
                dst[col] = src[row * stride + col];
            }
            upload(VRAM_TBP, VRAM_BW, PSM_CT16, x, y + row, w, 1, s_Staging, (u32)((w * 2 + 15) & ~15));
        }
    }
    hr_blit(1, x, y, w, h);
}

void GpuGs_ClearImage(s32 x, s32 y, s32 w, s32 h, u32 rgb)
{
    pend_flush();
    init();
    fill_rect(x, y, w, h, rgb);
}

void GpuGs_MoveImage(s32 sx, s32 sy, s32 dx, s32 dy, s32 w, s32 h)
{
    pend_flush();
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

/** Local-to-host transfer of the VRAM rectangle (x, y, w, h) into dst (rows of `stride` pixels).
 * x and w are rounded out to multiples of 64 pixels (128 bytes) for the transfer: the DMA takes the
 * VIF1 FIFO's data in 8-quadword bursts, and on a real PS2 a transfer that isn't a whole number of
 * bursts never ends (16x1 CLUT reads, 2 quadwords, stuck every time; PCSX2 doesn't care). At most
 * sizeof(s_Readback) bytes at a time (larger rectangles are read in bands). No wrapping. */
static void download_from(u32 tbp, u32 bw, u32 psm, s32 x, s32 y, s32 w, s32 h, void* dst, s32 stride);
static void download_rect(s32 x, s32 y, s32 w, s32 h, u16* dst, s32 stride)
{
    hr_blit(0, x, y, w, h);
    download_from(VRAM_TBP, VRAM_BW, PSM_CT16, x, y, w, h, dst, stride);
}

/** download_rect from any 16- or 32-bit buffer (tbp: block address, bw: width / 64, psm: its format;
 * dst: rows of `stride` pixels in that format). */
static void download_from(u32 tbp, u32 bw, u32 psm, s32 x, s32 y, s32 w, s32 h, void* dst, s32 stride)
{
    s32 bpp = psm == PSM_CT32 ? 4 : 2;
    s32 x0 = x & ~63, tw = ((x + w + 63) & ~63) - x0;
    s32 band = (s32)(sizeof(s_Readback) / bpp) / tw, row;
    if (h > band)
    {
        for (row = 0; row < h; row += band)
        {
            download_from(tbp, bw, psm, x, y + row, w, h - row < band ? h - row : band,
                          (u8*)dst + row * stride * bpp, stride);
        }
        return;
    }
    {
        u32 qwc = (u32)(tw * bpp * h / 16);
        const u8* rb = (const u8*)((u32)s_Readback | 0x20000000); /* uncached: the DMA wrote behind the cache */
        s32 spins;
        s_StReadbacks++;
        GpuGs_Flush();
        GpuGs_Sync();
        *GS_CSR = 2; /* clear FINISH, then wait for this transfer's FINISH before turning the bus around */
        ad(R_BITBLTBUF, (u64x)tbp | ((u64x)bw << 16) | ((u64x)psm << 24));
        ad(R_TRXPOS, (u64x)x0 | ((u64x)y << 16));
        ad(R_TRXREG, (u64x)tw | ((u64x)h << 32));
        ad(R_FINISH, 0);
        ad(R_TRXDIR, 1);
        GpuGs_Flush();
        GpuGs_Sync();

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
            /* Wedged: stop the channel and reset VIF1 rather than freezing (the data is stale). */
            printf("gpu_gs: VRAM download stuck (%d,%d %dx%d, qwc left %u), recovered\n", x0, y, tw, h,
                   (unsigned)*D1_QWC);
            *D1_CHCR    = 0;
            *VIF1_FBRST = 1;
        }
        *GS_BUSDIR = 0;
        *VIF1_STAT = 0;
        *GS_CSR    = 2; /* clear FINISH */
        for (row = 0; row < h; row++)
        {
            memcpy((u8*)dst + row * stride * bpp, &rb[(row * tw + (x - x0)) * bpp], (u32)(w * bpp));
        }
    }
}

/** StoreImage: the VRAM buffer's rectangle into `dst` (w pixels a row), split at the VRAM edges. */
void GpuGs_StoreImage(s32 x, s32 y, s32 w, s32 h, u16* dst)
{
#ifdef SH_PORT_SCREENREAD
    if (sr_hit(x, y, x + w, y + h))
    {
        sr_note(6, (u32)x | ((u32)y << 16), (u32)w | ((u32)h << 16), g_PortGpuCaller, x, y, x + w, y + h);
    }
#endif
    s32 w1, h1;
    if (w <= 0 || h <= 0)
    {
        return;
    }
    PROF_BEGIN("gs: StoreImage (VRAM readback)")
    pend_flush();
    init();
    x &= VRAM_W - 1;
    y &= VRAM_H - 1;
    w1 = x + w > VRAM_W ? VRAM_W - x : w;
    h1 = y + h > VRAM_H ? VRAM_H - y : h;
    download_rect(x, y, w1, h1, dst, w);
    if (w1 < w) download_rect(0, y, w - w1, h1, dst + w1, w);
    if (h1 < h) download_rect(x, 0, w1, h - h1, dst + h1 * w, w);
    if (w1 < w && h1 < h) download_rect(0, 0, w - w1, h - h1, dst + h1 * w + w1, w);
    PROF_END("gs: StoreImage (VRAM readback)")
}

/* --- 2x renderer prototype (see HIRES_ADDR) ---------------------------------------------------*/

/** s_Sh from the clip area: drawing goes to the hires buffer when the area lies inside its part of
 * VRAM. An area straddling its edge is drawn into VRAM (lost under the hires area): logged once. */
static void hr_target(void)
{
    u32 sh = s_Hires && s_AreaX2 < HR_W && s_AreaY2 < HR_H;
    if (s_Hires && !sh && s_AreaX1 < HR_W && s_AreaY1 < HR_H)
    {
        static int logged;
        if (logged++ < 8)
        {
            printf("hires: clip area (%d,%d)-(%d,%d) straddles the 2x area: drawn at 1x\n", (int)s_AreaX1,
                   (int)s_AreaY1, (int)s_AreaX2, (int)s_AreaY2);
        }
    }
    if (sh != s_Sh)
    {
        s_Sh = sh;
        s_Epoch++;
    }
}

/* Tiles (16x16 VRAM pixels) of the hires area drawn into since VRAM was last brought up to date:
 * one word per tile row, a bit per tile column. Only these are scaled down for reads: everything
 * else in VRAM is already right (uploaded data, such as the font atlas and CLUTs below the frame
 * buffers, never makes a scale round trip). */
#define HR_TILE 16
static u32 s_HrDirty[HR_H / HR_TILE];

/** Marks the tiles of VRAM's rectangle (x0, y0)-(x1, y1) (inclusive) as drawn in the hires buffer. */
static void hr_mark(s32 x0, s32 y0, s32 x1, s32 y1)
{
    s32 r;
    u32 bits;
    x1 = x1 < HR_W - 1 ? x1 : HR_W - 1;
    y1 = y1 < HR_H - 1 ? y1 : HR_H - 1;
    if (x0 > x1 || y0 > y1)
    {
        return;
    }
    bits = ((2u << (x1 / HR_TILE)) - 1) & ~((1u << (x0 / HR_TILE)) - 1);
    for (r = y0 / HR_TILE; r <= y1 / HR_TILE; r++)
    {
        s_HrDirty[r] |= bits;
    }
}

/** Common sprite state for the scaling blits; `up`: drawing into the shadow buffer. */
static void hr_blit_state(int up)
{
    pend_flush();
    s_Epoch++;
    if (up)
    {
        set(R_FRAME, (u64x)HIRES_FBP | ((u64x)HIRES_BW << 16) | ((u64x)s_HrPsm << 24));
        scissor(0, 0, (HR_W << s_HrSh) - 1, (HR_H << s_HrSh) - 1);
    }
    else
    {
        set(R_FRAME, (u64x)VRAM_FBP | ((u64x)VRAM_BW << 16) | ((u64x)PSM_CT16 << 24));
        scissor(0, 0, VRAM_W - 1, VRAM_H - 1);
    }
    set(R_XYOFFSET, (u64x)(2048 << 4) | ((u64x)(2048 << 4) << 32));
    set(R_PRMODECONT, 1);
    set(R_TEST, (1 << 16) | (1 << 17));
    set(R_FBA, 0);
    set(R_DTHE, 0); /* 32 -> 16 bits: truncated, as DuckStation's true colour VRAM reads */
    set(R_TEX1, 0);
    set(R_CLAMP, 0);
    set(R_TEXA, 0x40 | (1 << 15) | ((u64x)0x80 << 32));
    SCANMSK_CHECK();
    ad(R_TEXFLUSH, 0);
}

/* Texel offsets (1/16 texel) of the scaling sprites, so every sample sits inside its texel whether the
 * GS samples a pixel at its corner or its centre: 1:1 copies +1/4; scaling up 2x +1/8 (both pixels in
 * the texel); scaling down 2x +1/2. */
#define HR_BIAS_UP   (s_HrSh ? 2 : 4)
#define HR_BIAS_DOWN (s_HrSh ? 8 : 4)

/** Copies the dirty tiles of the shadow area in (x0, y0)-(x1, y1) (exclusive) down into VRAM (at 2x
 * each 2x2's top-left pixel; 32-bit colours truncated to 5 bits) and marks them clean. In tile rows
 * (16 VRAM rows): the texture is based at the row's page row, since UV's 14-bit fields stop at texel
 * 1023.9375 and the area's bottom edge at 2x is texel row 1024. */
static void hr_down(s32 x0, s32 y0, s32 x1, s32 y1)
{
    s32 r, c, started = 0;
    for (r = y0 / HR_TILE; r * HR_TILE < y1; r++)
    {
        u32 want = s_HrDirty[r] & ((x1 >= HR_W ? (1u << (HR_W / HR_TILE)) - 1 : (2u << ((x1 - 1) / HR_TILE)) - 1) &
                                   ~((1u << (x0 / HR_TILE)) - 1));
        s32 ty = r * HR_TILE, base = ((ty << s_HrSh) / HR_PAGE_H) * HR_PAGE_H; /* a page row of the shadow buffer */
        if (!want)
        {
            continue;
        }
        if (!started)
        {
            hr_blit_state(0);
            started = 1;
        }
        set(R_TEX0, (u64x)(HIRES_TBP + (base / HR_PAGE_H) * HIRES_BW * 32) | ((u64x)HIRES_BW << 14) |
                        ((u64x)s_HrPsm << 20) | (10ULL << 26) | (9ULL << 30) | (1ULL << 34) | (1ULL << 35)); /* TCC, DECAL */
        for (c = 0; c < HR_W / HR_TILE; c++)
        {
            s32 c1 = c, tx, tx1;
            if (!(want & (1u << c)))
            {
                continue;
            }
            while (c1 + 1 < HR_W / HR_TILE && (want & (1u << (c1 + 1))))
            {
                c1++;
            }
            tx  = c * HR_TILE;
            tx1 = (c1 + 1) * HR_TILE;
            ad(R_PRIM, 6 | (1 << 4) | (1 << 8));
            ad(R_RGBAQ, 0x80808080ULL | (0x3F800000ULL << 32));
            ad(R_UV, (u64x)(((tx << s_HrSh) << 4) + HR_BIAS_DOWN) | ((u64x)((((ty << s_HrSh) - base) << 4) + HR_BIAS_DOWN) << 16));
            ad(R_XYZ2, (u64x)((2048 + tx) << 4) | ((u64x)((2048 + ty) << 4) << 16));
            ad(R_RGBAQ, 0x80808080ULL | (0x3F800000ULL << 32));
            ad(R_UV, (u64x)(((tx1 << s_HrSh) << 4) + HR_BIAS_DOWN) |
                         ((u64x)(((((ty + HR_TILE) << s_HrSh) - base) << 4) + HR_BIAS_DOWN) << 16));
            ad(R_XYZ2, (u64x)((2048 + tx1) << 4) | ((u64x)((2048 + ty + HR_TILE) << 4) << 16));
            c = c1;
        }
        s_HrDirty[r] &= ~want;
    }
    if (started)
    {
        ad(R_TEXFLUSH, 0);
        s_Epoch++; /* prim_state again: state_other marks the clip area drawn again */
    }
}

/** Copies VRAM's (x0, y0)-(x1, y1) (exclusive) up into the shadow buffer (at 2x each pixel doubled;
 * 32-bit: 5-bit colours widened, bit 15 to alpha's bit 7 through TEXA). Tiles it covers whole are clean
 * afterwards. */
static void hr_up(s32 x0, s32 y0, s32 x1, s32 y1)
{
    s32 r;
    hr_blit_state(1);
    set(R_TEX0, (u64x)VRAM_TBP | ((u64x)VRAM_BW << 14) | ((u64x)PSM_CT16 << 20) | (10ULL << 26) | (9ULL << 30) |
                    (1ULL << 34) | (1ULL << 35)); /* 1024x512, TCC, DECAL */
    ad(R_PRIM, 6 | (1 << 4) | (1 << 8));
    ad(R_RGBAQ, 0x80808080ULL | (0x3F800000ULL << 32));
    ad(R_UV, (u64x)((x0 << 4) + HR_BIAS_UP) | ((u64x)((y0 << 4) + HR_BIAS_UP) << 16));
    ad(R_XYZ2, (u64x)((2048 + (x0 << s_HrSh)) << 4) | ((u64x)((2048 + (y0 << s_HrSh)) << 4) << 16));
    ad(R_RGBAQ, 0x80808080ULL | (0x3F800000ULL << 32));
    ad(R_UV, (u64x)((x1 << 4) + HR_BIAS_UP) | ((u64x)((y1 << 4) + HR_BIAS_UP) << 16));
    ad(R_XYZ2, (u64x)((2048 + (x1 << s_HrSh)) << 4) | ((u64x)((2048 + (y1 << s_HrSh)) << 4) << 16));
    ad(R_TEXFLUSH, 0);
    s_Epoch++;
    {
        s32 c0 = (x0 + HR_TILE - 1) / HR_TILE, c1 = x1 / HR_TILE; /* whole tiles: columns [c0, c1) */
        u32 bits = c1 > c0 ? ((1u << c1) - 1) & ~((1u << c0) - 1) : 0;
        for (r = (y0 + HR_TILE - 1) / HR_TILE; (r + 1) * HR_TILE <= y1; r++)
        {
            s_HrDirty[r] &= ~bits;
        }
    }
}

/** The shadow area's part of VRAM's rectangle (x, y, w, h): `up` copies VRAM's contents (just written:
 * uploads, fills' VRAM side, copies) up into the shadow buffer; otherwise VRAM is brought up to date
 * from it before a read. */
static void hr_blit(int up, s32 x, s32 y, s32 w, s32 h)
{
    s32 x1 = x + w < HR_W ? x + w : HR_W, y1 = y + h < HR_H ? y + h : HR_H;
    if (!s_Hires || x >= x1 || y >= y1)
    {
        return;
    }
    if (up)
    {
        hr_up(x, y, x1, y1);
    }
    else
    {
        hr_down(x, y, x1, y1);
    }
}

/** Turns the shadow buffer on or off (display_ps2.c, on video mode changes: off before gsKit's setup,
 * which clears the bottom of GS memory). On: the area copied up from VRAM; off: its drawn tiles down. */
void GpuGs_Hires(int on)
{
    if (!on == !s_Hires || (on && !s_HrLayout))
    {
        return;
    }
    pend_flush();
    init();
    if (on)
    {
        s_Hires = 1;
        hr_blit(1, 0, 0, HR_W, HR_H);
    }
    else
    {
        hr_blit(0, 0, 0, HR_W, HR_H);
        GpuGs_Flush();
        GpuGs_Sync();
        s_Hires = 0;
        cache_invalidate(0, 0, HR_W, HR_H);
    }
    hr_target();
    s_TexGen++;
    s_StateGen++;
    s_Epoch++;
    s_RegValid[R_TEX0] = 0;
    printf("hires: %s (%dx, %d-bit)\n", on ? "on" : "off", 1 << s_HrSh, s_HrPsm == PSM_CT32 ? 32 : 16);
}

/** The Debug Options rendering settings (display_ps2.c): `mode` bit 0 2x, bit 1 32-bit; 0 no shadow
 * buffer. Lays GS memory out for its buffer and the page cache slots left (see CACHE_SLOTS). The
 * shadow buffer is off when this is called. Every cached page goes (their slots move). */
void GpuGs_HiresLayout(int mode)
{
    s32 i;
    mode &= 3;
    if (mode == s_HrLayout)
    {
        return;
    }
    GpuGs_Hires(0);
    pend_flush();
    init();
    GpuGs_Flush();
    GpuGs_Sync();
    for (i = 0; i < CACHE_SLOTS; i++)
    {
        if (s_Slots[i].key >= 0)
        {
            s_PageMap[s_Slots[i].key] = 0;
            s_Slots[i].key            = -1;
        }
    }
    s_HrLayout         = mode;
    s_HrSh             = mode & 1;
    s_HrPsm            = (mode & 2) ? PSM_CT32 : PSM_CT16;
    s_CacheSlots       = mode == 1 ? 27 : mode == 3 ? 7 : mode == 2 ? CACHE_SLOTS : CACHE_SLOTS - 1;
    s_TexGen++;
    s_Epoch++;
    s_RegValid[R_TEX0] = 0;
    printf("hires: option %dx %d-bit, %d page cache slots\n", 1 << s_HrSh, s_HrPsm == PSM_CT32 ? 32 : 16,
           (int)s_CacheSlots);
}

int GpuGs_HiresOn(void)
{
    return s_Hires;
}

/** The shadow buffer's scale shift and depth (bits a pixel): for display_ps2.c's mode and dumps. */
int GpuGs_HiresShift(void)
{
    return (int)s_HrSh;
}

int GpuGs_HiresBits(void)
{
    return s_HrPsm == PSM_CT32 ? 32 : 16;
}

/** StoreImage from the shadow buffer (frame dumps): (x, y, w, h) in its own pixels, in its format
 * (16-bit PS1 pixels, or 32-bit RGBA). */
void GpuGs_StoreHires(s32 x, s32 y, s32 w, s32 h, void* dst)
{
    pend_flush();
    init();
    download_from(HIRES_TBP, HIRES_BW, s_HrPsm, x, y, w, h, dst, w);
}

/* --- Display ----------------------------------------------------------------------------------*/

/** Copies the PS1 display area 1:1 into the GS display frame buffer (fbp: page address, fbw: width / 64,
 * psm: its format). */
static void display_copy(u32 fbp, u32 fbw, u32 psm, s32 x, s32 y, s32 w, s32 h)
{
    pend_flush();
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
    GpuGs_Sync(); /* drawn before it is shown */
}


/** Shows the PS1 display area: output circuit 1 scans out the 16-bit VRAM buffer from (x, y), as the
 * PS1 scans out its own VRAM (PS1 15-bit pixels have the GS's 16-bit layout), with no copy; except
 * single-buffered interlaced screens (see s_Interlace480), shown from a snapshot at SNAP_ADDR.
 * `interlace480`: the PS1 display is interlaced with 480 lines. `blank`: output off (BGCOLOR,
 * black), for the frames right after a video mode change. PMODE: circuit 1 only, fixed alpha (bit
 * 15, the PS1 mask bit, would otherwise be read as alpha). */
void GpuGs_DisplayShow(s32 x, s32 y, s32 w, s32 h, s32 interlace480, s32 blank)
{
#ifdef SH_PORT_SCREENREAD
    sr_screen(x, y, w, h);
    sr_summary();
#endif
    u64x dispfb;
    if (interlace480 != s_Interlace480 || (y & 1) != (s_DispY & 1))
    {
        s_Interlace480 = interlace480;
        s_DispY        = y;
        s_MaskDirty    = 1;
    }
    if (s_Hires)
    {
        GpuGs_Flush();
        GpuGs_Sync(); /* the frame's last packet sent before the display switches, as when sends waited */
        dispfb = (u64x)HIRES_FBP | ((u64x)HIRES_BW << 9) | ((u64x)s_HrPsm << 15) | ((u64x)((x << s_HrSh) & 2047) << 32) |
                 ((u64x)((y << s_HrSh) & 2047) << 43);
    }
    else
#ifndef SH_PORT_INTERLACE_FIELDS
    if (interlace480 && !(s_TexPage & 0x400))
    {
        display_copy(SNAP_ADDR / 8192, 640 / 64, PSM_CT16, x, y, w, h);
        dispfb = (u64x)(SNAP_ADDR / 8192) | ((u64x)(640 / 64) << 9) | ((u64x)PSM_CT16 << 15);
    }
    else
#endif
    {
        GpuGs_Flush();
        GpuGs_Sync(); /* the frame's last packet sent before the display switches, as when sends waited */
        dispfb = (u64x)VRAM_FBP | ((u64x)VRAM_BW << 9) | ((u64x)PSM_CT16 << 15) |
                 ((u64x)(x & (VRAM_W - 1)) << 32) | ((u64x)(y & (VRAM_H - 1)) << 43);
    }
    *(volatile u64x*)0x12000000 = (blank ? 0 : 1) | (1 << 2) | (1 << 5) | (0xFFULL << 8); /* PMODE: EN1, MMOD (ALP) */
    *(volatile u64x*)0x12000070 = dispfb;                                                   /* DISPFB1 */
}

/** A movie frame (movie_ps2.c): `rgba` (w x h, 32-bit) uploaded into the snapshot area as a PSMCT32
 * buffer `fbw` x 64 pixels wide, at line `top` (the lines above and below were cleared to black when
 * `clear`), and shown from there. Movies bypass the PS1's VRAM and its 24-bit display mode. */
void GpuGs_MovieShow(const u32* rgba, s32 w, s32 h, s32 top, s32 lines, s32 clear, s32 blank)
{
    const s32 FBW = (w + 63) / 64, BAND = (s32)(sizeof(s_ImgPkt) - 32) / (w * 4);
    s32       y;
    pend_flush();
    init();
    s_Epoch++;
    if (clear)
    {
        /* Black: draw a sprite over the whole buffer. */
        set(R_FRAME, (u64x)(SNAP_ADDR / 8192) | ((u64x)FBW << 16) | ((u64x)PSM_CT32 << 24));
        set(R_XYOFFSET, (u64x)(2048 << 4) | ((u64x)(2048 << 4) << 32));
        scissor(0, 0, FBW * 64 - 1, lines - 1);
        set(R_TEST, (1 << 16) | (1 << 17));
        set(R_FBA, 0);
        set(R_DTHE, 0);
        ad(R_PRIM, 6);
        ad(R_RGBAQ, 0x80000000ULL | (0x3F800000ULL << 32));
        ad(R_XYZ2, (u64x)(2048 << 4) | ((u64x)(2048 << 4) << 16));
        ad(R_XYZ2, (u64x)((2048 + FBW * 64) << 4) | ((u64x)((2048 + lines) << 4) << 16));
        GpuGs_Flush();
    }
    for (y = 0; y < h; y += BAND)
    {
        s32 n = h - y < BAND ? h - y : BAND;
        upload(SNAP_ADDR / 256, (u32)FBW, PSM_CT32, 0, top + y, w, n, rgba + y * w, (u32)(w * n * 4));
    }
    GpuGs_Sync();
    *(volatile u64x*)0x12000000 = (blank ? 0 : 1) | (1 << 2) | (1 << 5) | (0xFFULL << 8); /* PMODE: EN1, MMOD (ALP) */
    *(volatile u64x*)0x12000070 = (u64x)(SNAP_ADDR / 8192) | ((u64x)FBW << 9) | ((u64x)PSM_CT32 << 15); /* DISPFB1 */
}

/** Prints and clears the statistics (averaged over `frames`). */
void GpuGs_Stats(u32 frames)
{
    s32 a, m;
    if (!frames)
    {
        return;
    }
    printf("gs stats per frame: %u prims, %u TEX0 changes, %u CLUT loads, %u check-mask prims, %u prims "
           "texturing from VRAM, %u uploads, %u readbacks\n", s_StPrims / frames, s_StTex0 / frames, s_StClut / frames,
           s_StDate / frames, s_StRtTex / frames, s_StUploads / frames, s_StReadbacks / frames);
    s_StPrims = s_StTex0 = s_StClut = s_StDate = s_StRtTex = s_StUploads = s_StReadbacks = 0;
#ifdef SH_PORT_LITEPROF
    printf("gs filtered CLUTs per frame: %u lookups, %u misses\n", s_StFiltCalls / frames, s_StFiltMiss / frames);
    s_StFiltCalls = s_StFiltMiss = 0;
    printf("gs cache per frame: %u page misses, %u CLUT misses, %u pages and %u CLUTs dropped by VRAM writes (%u drops; first rects:",
           s_StPageMiss / frames, s_StClutMiss / frames, s_StPageDrop / frames, s_StClutDrop / frames, s_StDropRects / frames);
    for (a = 0; a < (s32)s_StDropRects && a < 8; a++)
    {
        printf(" %d,%d %dx%d", s_DropRect[a][0], s_DropRect[a][1], s_DropRect[a][2], s_DropRect[a][3]);
    }
    printf(")\n");
    s_StPageMiss = s_StClutMiss = s_StPageDrop = s_StClutDrop = s_StDropRects = 0;
    {
        static const char* const NAMES[LP_COUNT] = { "DrawOTag", "GpuGs_Commands", "polygon: parse", "polygon: state",
                                                     "polygon: emit", "rectangle", "lines", "flush (DMA)",
                                                     "state: texture", "state: other", "mesh: light", "mesh: transform",
                                                     "mesh: packets", "light: collect", "light: batch", "light: apply",
                                                     "map fx (grid)", "map fx: batch", "particles: move",
                                                     "particles: draw", "snow: rtps" };
        for (a = 0; a < LP_COUNT; a++)
        {
            printf("liteprof: %-16s %5u kcycles %5u calls a frame\n", NAMES[a], g_LiteProf[a][0] / 1000 / frames,
                   g_LiteProf[a][1] / frames);
            g_LiteProf[a][0] = g_LiteProf[a][1] = 0;
        }
    }
#endif
#ifdef GS_FOG_PAIR_STATS
    printf("gs fog pairs per frame (plain opaque/semi x textured mode 0-3): %u %u %u %u / %u %u %u %u\n",
           s_StFogPair[0][0] / frames, s_StFogPair[0][1] / frames, s_StFogPair[0][2] / frames, s_StFogPair[0][3] / frames,
           s_StFogPair[1][0] / frames, s_StFogPair[1][1] / frames, s_StFogPair[1][2] / frames, s_StFogPair[1][3] / frames);
    for (a = 0; a < 2; a++)
    {
        for (m = 0; m < 4; m++)
        {
            s_StFogPair[a][m] = 0;
        }
    }
#endif
}
