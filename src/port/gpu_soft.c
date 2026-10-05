/** @brief Software PS1 GPU: executes GP0 command packets into the emulated VRAM (g_PortVram).
 *
 * Written from psx-spx ("GPU Render Polygon/Rectangle/Line Commands", "Rendering Attributes").
 * Supports polygons (flat/gouraud, textured 4/8/15-bit, raw/modulated), rectangles, lines and
 * polylines, fills, VRAM copies, the E1-E6 settings, semi-transparency (modes 0-3, texel bit 15),
 * mask bit set/check, texture windows and dithering. Aims at visually correct output; it's also the
 * reference for the GS renderer. Plain C, no PS2 dependencies.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef signed int     s32;
typedef unsigned int   u32;
typedef long long      s64;

#define VRAM_W 1024
#define VRAM_H 512

extern u16 g_PortVram[VRAM_H][VRAM_W];

/* Drawing state (GP0 E1-E6). */
static u32 s_TexPage;          /* E1 bits 0-13 */
static u32 s_TwMaskX, s_TwMaskY, s_TwOffX, s_TwOffY;
static s32 s_AreaX1, s_AreaY1, s_AreaX2 = VRAM_W - 1, s_AreaY2 = VRAM_H - 1;
static s32 s_OffX, s_OffY;
static u32 s_SetMask, s_CheckMask;

static const s32 DITHER[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };

typedef struct
{
    s32 x, y;
    s32 r, g, b;
    s32 u, v;
} Vtx;

typedef struct
{
    u32 textured, raw, semi, gouraud, dither;
    u32 tpage; /* texture page for this primitive */
    u32 clut;
    u32 semiMode;
} Prim;

static s32 sext11(u32 v)
{
    return ((s32)(v << 21)) >> 21;
}

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

/* --- Texel fetch ------------------------------------------------------------------------------*/

static u16 texel(const Prim* p, s32 u, s32 v)
{
    u32 tx = (p->tpage & 0xF) * 64;
    u32 ty = ((p->tpage >> 4) & 1) * 256;
    u32 depth = (p->tpage >> 7) & 3;
    u &= 0xFF;
    v &= 0xFF;
    u = (u & ~(s_TwMaskX * 8)) | ((s_TwOffX & s_TwMaskX) * 8);
    v = (v & ~(s_TwMaskY * 8)) | ((s_TwOffY & s_TwMaskY) * 8);

    if (depth == 0)
    {
        u16 w   = g_PortVram[(ty + v) & (VRAM_H - 1)][(tx + (u >> 2)) & (VRAM_W - 1)];
        u32 idx = (w >> ((u & 3) * 4)) & 0xF;
        return g_PortVram[(p->clut >> 6) & 0x1FF][((p->clut & 0x3F) * 16 + idx) & (VRAM_W - 1)];
    }
    if (depth == 1)
    {
        u16 w   = g_PortVram[(ty + v) & (VRAM_H - 1)][(tx + (u >> 1)) & (VRAM_W - 1)];
        u32 idx = (w >> ((u & 1) * 8)) & 0xFF;
        return g_PortVram[(p->clut >> 6) & 0x1FF][((p->clut & 0x3F) * 16 + idx) & (VRAM_W - 1)];
    }
    return g_PortVram[(ty + v) & (VRAM_H - 1)][(tx + u) & (VRAM_W - 1)];
}

/* --- Pixel output -----------------------------------------------------------------------------*/

static s32 clamp5(s32 v)
{
    return v < 0 ? 0 : v > 31 ? 31 : v;
}

/** r/g/b are 8-bit vertex colours (0x80 = 1.0 for modulation). */
static void plot(const Prim* p, s32 x, s32 y, s32 r, s32 g, s32 b, s32 u, s32 v)
{
    u16* dst = &g_PortVram[y][x];
    u32 stp  = 0;
    s32 R, G, B;

    if (s_CheckMask && (*dst & 0x8000))
    {
        return;
    }

    if (p->textured)
    {
        u16 t = texel(p, u, v);
        if (t == 0)
        {
            return; /* fully transparent */
        }
        stp = t & 0x8000;
        if (p->raw)
        {
            R = t & 31;
            G = (t >> 5) & 31;
            B = (t >> 10) & 31;
        }
        else
        {
            /* Modulate in 8-bit precision, then (optionally) dither down to 5 bits. */
            R = ((t & 31) << 3) * r >> 7;
            G = (((t >> 5) & 31) << 3) * g >> 7;
            B = (((t >> 10) & 31) << 3) * b >> 7;
            if (p->dither)
            {
                s32 d = DITHER[y & 3][x & 3];
                R += d;
                G += d;
                B += d;
            }
            R = clamp5(R >> 3);
            G = clamp5(G >> 3);
            B = clamp5(B >> 3);
        }
    }
    else
    {
        R = r;
        G = g;
        B = b;
        if (p->dither)
        {
            s32 d = DITHER[y & 3][x & 3];
            R += d;
            G += d;
            B += d;
        }
        R = clamp5(R >> 3);
        G = clamp5(G >> 3);
        B = clamp5(B >> 3);
    }

    if (p->semi && (!p->textured || stp))
    {
        s32 bR = *dst & 31, bG = (*dst >> 5) & 31, bB = (*dst >> 10) & 31;
        switch (p->semiMode)
        {
            case 0: R = (bR + R) >> 1; G = (bG + G) >> 1; B = (bB + B) >> 1; break;
            case 1: R = clamp5(bR + R); G = clamp5(bG + G); B = clamp5(bB + B); break;
            case 2: R = clamp5(bR - R); G = clamp5(bG - G); B = clamp5(bB - B); break;
            default: R = clamp5(bR + (R >> 2)); G = clamp5(bG + (G >> 2)); B = clamp5(bB + (B >> 2)); break;
        }
    }

    *dst = (u16)(R | (G << 5) | (B << 10) | stp | (s_SetMask ? 0x8000 : 0));
}

static int in_area(s32 x, s32 y)
{
    return x >= s_AreaX1 && x <= s_AreaX2 && y >= s_AreaY1 && y <= s_AreaY2;
}

/* --- Triangles --------------------------------------------------------------------------------*/

static s64 edge(const Vtx* a, const Vtx* b, s32 x, s32 y)
{
    return (s64)(b->x - a->x) * (y - a->y) - (s64)(b->y - a->y) * (x - a->x);
}

/** Top-left rule: a pixel on an edge belongs to the triangle only for top or left edges. */
static int is_top_left(const Vtx* a, const Vtx* b)
{
    return (a->y == b->y && b->x < a->x) || (b->y < a->y);
}

static void triangle(const Prim* p, Vtx v0, Vtx v1, Vtx v2)
{
    s64 area;
    s32 minx, maxx, miny, maxy, x, y;
    int tl0, tl1, tl2;

    /* The GPU skips polygons spanning more than 1023x511. */
    if ((v0.x > v1.x ? v0.x - v1.x : v1.x - v0.x) > 1023 || (v1.x > v2.x ? v1.x - v2.x : v2.x - v1.x) > 1023 ||
        (v0.x > v2.x ? v0.x - v2.x : v2.x - v0.x) > 1023 || (v0.y > v1.y ? v0.y - v1.y : v1.y - v0.y) > 511 ||
        (v1.y > v2.y ? v1.y - v2.y : v2.y - v1.y) > 511 || (v0.y > v2.y ? v0.y - v2.y : v2.y - v0.y) > 511)
    {
        return;
    }

    area = edge(&v0, &v1, v2.x, v2.y);
    if (area == 0)
    {
        return;
    }
    if (area < 0)
    {
        Vtx t = v1;
        v1    = v2;
        v2    = t;
        area  = -area;
    }

    minx = v0.x; if (v1.x < minx) minx = v1.x; if (v2.x < minx) minx = v2.x;
    maxx = v0.x; if (v1.x > maxx) maxx = v1.x; if (v2.x > maxx) maxx = v2.x;
    miny = v0.y; if (v1.y < miny) miny = v1.y; if (v2.y < miny) miny = v2.y;
    maxy = v0.y; if (v1.y > maxy) maxy = v1.y; if (v2.y > maxy) maxy = v2.y;
    if (minx < s_AreaX1) minx = s_AreaX1;
    if (miny < s_AreaY1) miny = s_AreaY1;
    if (maxx > s_AreaX2) maxx = s_AreaX2;
    if (maxy > s_AreaY2) maxy = s_AreaY2;

    tl0 = is_top_left(&v1, &v2);
    tl1 = is_top_left(&v2, &v0);
    tl2 = is_top_left(&v0, &v1);

    for (y = miny; y <= maxy; y++)
    {
        for (x = minx; x <= maxx; x++)
        {
            s64 w0 = edge(&v1, &v2, x, y);
            s64 w1 = edge(&v2, &v0, x, y);
            s64 w2 = edge(&v0, &v1, x, y);
            s32 r, g, b, u, v;

            if (w0 < 0 || w1 < 0 || w2 < 0 || (w0 == 0 && !tl0) || (w1 == 0 && !tl1) || (w2 == 0 && !tl2))
            {
                continue;
            }
            if (p->gouraud)
            {
                r = (s32)((w0 * v0.r + w1 * v1.r + w2 * v2.r) / area);
                g = (s32)((w0 * v0.g + w1 * v1.g + w2 * v2.g) / area);
                b = (s32)((w0 * v0.b + w1 * v1.b + w2 * v2.b) / area);
            }
            else
            {
                r = v0.r;
                g = v0.g;
                b = v0.b;
            }
            if (p->textured)
            {
                u = (s32)((w0 * v0.u + w1 * v1.u + w2 * v2.u) / area);
                v = (s32)((w0 * v0.v + w1 * v1.v + w2 * v2.v) / area);
            }
            else
            {
                u = v = 0;
            }
            plot(p, x, y, r, g, b, u, v);
        }
    }
}

/* --- Commands ---------------------------------------------------------------------------------*/

static u32 polygon(const u32* w, s32 n)
{
    u32 cmd = w[0] >> 24;
    Prim p;
    Vtx  v[4];
    s32  nv = (cmd & 0x08) ? 4 : 3;
    s32  i, k = 0;

    p.gouraud  = (cmd >> 4) & 1;
    p.textured = (cmd >> 2) & 1;
    p.raw      = (cmd & 1) && p.textured;
    p.semi     = (cmd >> 1) & 1;
    p.tpage    = s_TexPage;
    p.clut     = 0;

    for (i = 0; i < nv; i++)
    {
        if (i == 0 || p.gouraud)
        {
            if (k >= n) return n;
            read_rgb(w[k++], &v[i]);
        }
        else
        {
            v[i].r = v[0].r; v[i].g = v[0].g; v[i].b = v[0].b;
        }
        if (k >= n) return n;
        read_xy(w[k++], &v[i]);
        if (p.textured)
        {
            if (k >= n) return n;
            v[i].u = w[k] & 0xFF;
            v[i].v = (w[k] >> 8) & 0xFF;
            if (i == 0) p.clut = w[k] >> 16;
            if (i == 1) p.tpage = (s_TexPage & ~0x1FFu) | ((w[k] >> 16) & 0x1FF);
            k++;
        }
    }
    p.semiMode = (p.tpage >> 5) & 3;
    p.dither   = ((s_TexPage >> 9) & 1) && (p.gouraud || (p.textured && !p.raw));
    triangle(&p, v[0], v[1], v[2]);
    if (nv == 4)
    {
        triangle(&p, v[1], v[2], v[3]);
    }
    return (u32)k;
}

static u32 rectangle(const u32* w, s32 n)
{
    u32 cmd = w[0] >> 24;
    u32 siz = (cmd >> 3) & 3;
    Prim p;
    Vtx  v;
    s32  k = 0, rw, rh, x, y, u0 = 0, v0 = 0;

    p.gouraud  = 0;
    p.textured = (cmd >> 2) & 1;
    p.raw      = (cmd & 1) && p.textured;
    p.semi     = (cmd >> 1) & 1;
    p.tpage    = s_TexPage;
    p.semiMode = (s_TexPage >> 5) & 3;
    p.dither   = 0;
    p.clut     = 0;

    read_rgb(w[k++], &v);
    if (k >= n) return n;
    read_xy(w[k++], &v);
    if (p.textured)
    {
        if (k >= n) return n;
        u0     = w[k] & 0xFF;
        v0     = (w[k] >> 8) & 0xFF;
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

    for (y = 0; y < rh; y++)
    {
        for (x = 0; x < rw; x++)
        {
            s32 px = v.x + x, py = v.y + y;
            s32 tu = (s_TexPage & 0x1000) ? u0 - x : u0 + x;
            s32 tv = (s_TexPage & 0x2000) ? v0 - y : v0 + y;
            if (in_area(px, py))
            {
                plot(&p, px, py, v.r, v.g, v.b, tu, tv);
            }
        }
    }
    return (u32)k;
}

static void line(const Prim* p, Vtx a, Vtx b)
{
    s32 dx = b.x - a.x, dy = b.y - a.y;
    s32 steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    s32 i;

    if ((dx < 0 ? -dx : dx) > 1023 || (dy < 0 ? -dy : dy) > 511)
    {
        return;
    }
    for (i = 0; i <= steps; i++)
    {
        s32 x = steps ? a.x + (dx * i + (dx >= 0 ? steps / 2 : -steps / 2)) / steps : a.x;
        s32 y = steps ? a.y + (dy * i + (dy >= 0 ? steps / 2 : -steps / 2)) / steps : a.y;
        s32 r = p->gouraud && steps ? a.r + (b.r - a.r) * i / steps : a.r;
        s32 g = p->gouraud && steps ? a.g + (b.g - a.g) * i / steps : a.g;
        s32 bb = p->gouraud && steps ? a.b + (b.b - a.b) * i / steps : a.b;
        if (in_area(x, y))
        {
            plot(p, x, y, r, g, bb, 0, 0);
        }
    }
}

static u32 lines(const u32* w, s32 n)
{
    u32 cmd = w[0] >> 24;
    Prim p;
    Vtx  prev, cur;
    s32  k = 0;

    p.gouraud  = (cmd >> 4) & 1;
    p.textured = 0;
    p.raw      = 0;
    p.semi     = (cmd >> 1) & 1;
    p.semiMode = (s_TexPage >> 5) & 3;
    p.dither   = (s_TexPage >> 9) & 1;
    p.tpage    = s_TexPage;
    p.clut     = 0;

    read_rgb(w[k++], &prev);
    if (k >= n) return n;
    read_xy(w[k++], &prev);
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
        line(&p, prev, cur);
        prev = cur;
        if (!(cmd & 0x08))
        {
            return (u32)k;
        }
    }
}

static void fill(const u32* w)
{
    u16 c = (u16)(((w[0] & 0xFF) >> 3) | ((((w[0] >> 8) & 0xFF) >> 3) << 5) | ((((w[0] >> 16) & 0xFF) >> 3) << 10));
    s32 x0 = w[1] & 0x3F0, y0 = (w[1] >> 16) & 0x1FF;
    s32 fw = ((w[2] & 0x3FF) + 15) & ~15, fh = (w[2] >> 16) & 0x1FF;
    s32 x, y;
    for (y = 0; y < fh; y++)
    {
        for (x = 0; x < fw; x++)
        {
            g_PortVram[(y0 + y) & (VRAM_H - 1)][(x0 + x) & (VRAM_W - 1)] = c;
        }
    }
}

static void vram_copy(const u32* w)
{
    s32 sx = w[1] & 0x3FF, sy = (w[1] >> 16) & 0x1FF;
    s32 dx = w[2] & 0x3FF, dy = (w[2] >> 16) & 0x1FF;
    s32 cw = ((w[3] - 1) & 0x3FF) + 1, ch = (((w[3] >> 16) - 1) & 0x1FF) + 1;
    s32 x, y;
    for (y = 0; y < ch; y++)
    {
        for (x = 0; x < cw; x++)
        {
            u16 c = g_PortVram[(sy + y) & (VRAM_H - 1)][(sx + x) & (VRAM_W - 1)];
            u16* d = &g_PortVram[(dy + y) & (VRAM_H - 1)][(dx + x) & (VRAM_W - 1)];
            if (s_CheckMask && (*d & 0x8000))
            {
                continue;
            }
            *d = c | (s_SetMask ? 0x8000 : 0);
        }
    }
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
void GpuSoft_Commands(const u32* w, s32 n)
{
    while (n > 0)
    {
        u32 cmd = w[0] >> 24;
        u32 used = 1;

        if (cmd >= 0x20 && cmd < 0x40)
        {
            used = polygon(w, n); /* the first word holds the command and vertex 0's colour */
        }
        else if (cmd >= 0x40 && cmd < 0x60)
        {
            used = lines(w, n);
        }
        else if (cmd >= 0x60 && cmd < 0x80)
        {
            used = rectangle(w, n);
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
        /* 0x00/0x01 (nop, cache clear) and anything else: one word. */
        if (used == 0)
        {
            used = 1;
        }
        w += used;
        n -= (s32)used;
    }
}
