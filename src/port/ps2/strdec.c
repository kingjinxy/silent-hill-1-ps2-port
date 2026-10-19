/** @brief PS1 movie (STR) frame decoder: the bitstream that DecDCTvlc decodes and the MDEC's
 * reconstruction (dequantization, inverse DCT, YCbCr to RGB), in plain C so it can run on the PS2's
 * EE (movie_ps2.c) as well as on the host (tools/port/str/strdump.c).
 *
 * Frame data (the video sectors' 2016-byte payloads, in order): an 8-byte header (u16 run-length
 * code count, u16 0x3800, u16 quantizer scale, u16 version), then the bitstream, read as 16-bit
 * little-endian words, most significant bit first. Version 2 (Silent Hill's): per macroblock, six
 * 8x8 blocks Cr, Cb, Y1-Y4 (Y in 2x2 order); per block, the DC coefficient as a 10-bit signed value,
 * then AC coefficients as MPEG-1 DCT coefficient codes (run, level, sign; "10" ends the block;
 * "000001" escapes to a 6-bit run and a 10-bit signed level). Macroblocks come in columns: top to
 * bottom, then the next 16-pixel column (the MDEC's slice order).
 *
 * Reconstruction as the MDEC does it (dequant, idct below): the inverse DCT; colour from the 2x2
 * subsampled Cr/Cb.
 * In integers throughout, with the MDEC's own IDCT matrix, dequantization and rounding: the output
 * matches DuckStation's MDEC bit for bit (tools/port/duckstation_movie.py). */

#include "strdec.h"

/* --- Bitstream ------------------------------------------------------------------------------------*/

typedef struct
{
    const unsigned char* data;
    int                  size; /* bytes */
    int                  pos;  /* bit position */
} Bits;

static unsigned int word(const Bits* b, int i)
{
    return 2 * i + 1 < b->size ? b->data[2 * i] | (b->data[2 * i + 1] << 8) : 0;
}

/** The next n bits (1-16): two 16-bit words hold any 16 bits starting in the first. */
static unsigned int peek(Bits* b, int n)
{
    int          w = b->pos >> 4;
    unsigned int v = (word(b, w) << 16) | word(b, w + 1);
    return (v << (b->pos & 15)) >> (32 - n);
}

static unsigned int get(Bits* b, int n)
{
    unsigned int v = peek(b, n);
    b->pos += n;
    return v;
}

/* MPEG-1 DCT coefficient codes (ISO 11172-2 table B.5c, "dct_coeff_next"): run, level, code length
 * without the sign bit, code. */
typedef struct
{
    unsigned char run, level, len;
    unsigned short code;
} Vlc;

static const Vlc VLC[] = {
    { 0, 1, 2, 0x3 },     { 1, 1, 3, 0x3 },     { 0, 2, 4, 0x4 },     { 2, 1, 4, 0x5 },     { 0, 3, 5, 0x5 },
    { 3, 1, 5, 0x7 },     { 4, 1, 5, 0x6 },     { 1, 2, 6, 0x6 },     { 5, 1, 6, 0x7 },     { 6, 1, 6, 0x5 },
    { 7, 1, 6, 0x4 },     { 0, 4, 7, 0x6 },     { 2, 2, 7, 0x4 },     { 8, 1, 7, 0x7 },     { 9, 1, 7, 0x5 },
    { 0, 5, 8, 0x26 },    { 0, 6, 8, 0x21 },    { 1, 3, 8, 0x25 },    { 3, 2, 8, 0x24 },    { 10, 1, 8, 0x27 },
    { 11, 1, 8, 0x23 },   { 12, 1, 8, 0x22 },   { 13, 1, 8, 0x20 },   { 0, 7, 10, 0x0A },   { 1, 4, 10, 0x0C },
    { 2, 3, 10, 0x0B },   { 4, 2, 10, 0x0F },   { 5, 2, 10, 0x09 },   { 14, 1, 10, 0x0E },  { 15, 1, 10, 0x0D },
    { 16, 1, 10, 0x08 },  { 0, 8, 12, 0x1D },   { 0, 9, 12, 0x18 },   { 0, 10, 12, 0x13 },  { 0, 11, 12, 0x10 },
    { 1, 5, 12, 0x1B },   { 2, 4, 12, 0x14 },   { 3, 3, 12, 0x1C },   { 4, 3, 12, 0x12 },   { 6, 2, 12, 0x1E },
    { 7, 2, 12, 0x15 },   { 8, 2, 12, 0x11 },   { 17, 1, 12, 0x1F },  { 18, 1, 12, 0x1A },  { 19, 1, 12, 0x19 },
    { 20, 1, 12, 0x17 },  { 21, 1, 12, 0x16 },  { 0, 12, 13, 0x1A },  { 0, 13, 13, 0x19 },  { 0, 14, 13, 0x18 },
    { 0, 15, 13, 0x17 },  { 1, 6, 13, 0x16 },   { 1, 7, 13, 0x15 },   { 2, 5, 13, 0x14 },   { 3, 4, 13, 0x13 },
    { 5, 3, 13, 0x12 },   { 9, 2, 13, 0x11 },   { 10, 2, 13, 0x10 },  { 22, 1, 13, 0x1F },  { 23, 1, 13, 0x1E },
    { 24, 1, 13, 0x1D },  { 25, 1, 13, 0x1C },  { 26, 1, 13, 0x1B },  { 0, 16, 14, 0x1F },  { 0, 17, 14, 0x1E },
    { 0, 18, 14, 0x1D },  { 0, 19, 14, 0x1C },  { 0, 20, 14, 0x1B },  { 0, 21, 14, 0x1A },  { 0, 22, 14, 0x19 },
    { 0, 23, 14, 0x18 },  { 0, 24, 14, 0x17 },  { 0, 25, 14, 0x16 },  { 0, 26, 14, 0x15 },  { 0, 27, 14, 0x14 },
    { 0, 28, 14, 0x13 },  { 0, 29, 14, 0x12 },  { 0, 30, 14, 0x11 },  { 0, 31, 14, 0x10 },  { 0, 32, 15, 0x18 },
    { 0, 33, 15, 0x17 },  { 0, 34, 15, 0x16 },  { 0, 35, 15, 0x15 },  { 0, 36, 15, 0x14 },  { 0, 37, 15, 0x13 },
    { 0, 38, 15, 0x12 },  { 0, 39, 15, 0x11 },  { 0, 40, 15, 0x10 },  { 1, 8, 15, 0x1F },   { 1, 9, 15, 0x1E },
    { 1, 10, 15, 0x1D },  { 1, 11, 15, 0x1C },  { 1, 12, 15, 0x1B },  { 1, 13, 15, 0x1A },  { 1, 14, 15, 0x19 },
    { 1, 15, 16, 0x13 },  { 1, 16, 16, 0x12 },  { 1, 17, 16, 0x11 },  { 1, 18, 16, 0x10 },  { 6, 3, 16, 0x14 },
    { 11, 2, 16, 0x1A },  { 12, 2, 16, 0x19 },  { 13, 2, 16, 0x18 },  { 14, 2, 16, 0x17 },  { 15, 2, 16, 0x16 },
    { 16, 2, 16, 0x15 },  { 27, 1, 16, 0x1F },  { 28, 1, 16, 0x1E },  { 29, 1, 16, 0x1D },  { 30, 1, 16, 0x1C },
    { 31, 1, 16, 0x1B },
};

/* Lookup by the next 16 bits (codes are at most 16 bits; the table is built once). */
static short s_LutRun[65536], s_LutLevel[65536];
static unsigned char s_LutLen[65536];
static int s_LutReady;

static void lut_build(void)
{
    unsigned int i, k;
    for (i = 0; i < 65536; i++)
    {
        s_LutLen[i] = 0;
    }
    for (k = 0; k < sizeof(VLC) / sizeof(VLC[0]); k++)
    {
        const Vlc* v     = &VLC[k];
        unsigned int top = (unsigned int)v->code << (16 - v->len), n = 1u << (16 - v->len);
        for (i = 0; i < n; i++)
        {
            s_LutRun[top | i]   = v->run;
            s_LutLevel[top | i] = v->level;
            s_LutLen[top | i]   = v->len;
        }
    }
    s_LutReady = 1;
}

/* --- MDEC reconstruction ---------------------------------------------------------------------------*/

static const unsigned char ZIGZAG[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* The PS1's default quantization table (libpress DecDCTReset): MPEG's default intra matrix with 2 as
 * the DC entry, here in natural order; QT is it in zigzag order (built with the VLC lookup). */
static const unsigned char QT_NATURAL[64] = {
    2,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
    34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32,
    35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};
static unsigned char QT[64];

/* The MDEC's IDCT matrix as libpress DecDCTReset uploads it (MDEC command 0x60000000; the stream
 * overlay's libpress data): row u, column x = cos((2x + 1) u pi / 16) x 32768, row 0 x 1/sqrt(2). */
static const short SCALE[64] = {
    0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82,
    0x7D8A, 0x6A6D, 0x471C, 0x18F8, -0x18F9, -0x471D, -0x6A6E, -0x7D8B,
    0x7641, 0x30FB, -0x30FC, -0x7642, -0x7642, -0x30FC, 0x30FB, 0x7641,
    0x6A6D, -0x18F9, -0x7D8B, -0x471D, 0x471C, 0x7D8A, 0x18F8, -0x6A6E,
    0x5A82, -0x5A83, -0x5A83, 0x5A82, 0x5A82, -0x5A83, -0x5A83, 0x5A82,
    0x471C, -0x7D8B, 0x18F8, 0x6A6D, -0x6A6E, -0x18F9, 0x7D8A, -0x471D,
    0x30FB, -0x7642, 0x7641, -0x30FC, -0x30FC, 0x7641, -0x7642, 0x30FB,
    0x18F8, -0x471D, 0x6A6D, -0x7D8B, 0x7D8A, -0x6A6E, 0x471C, -0x18F9,
};

/** The low 9 bits as a signed value (the MDEC's 9-bit intermediate), clamped to -128..127. */
static int clamp9(int v)
{
    v = (v & 0x1FF) - ((v & 0x100) << 1);
    return v < -128 ? -128 : v > 127 ? 127 : v;
}

/** One pass's output value from the two halves of its sum (products 0-3 and 4-7, each within 32
 * bits: |coefficient| <= 16384, |matrix| <= 32767): (lo + hi + 0x20000) >> 18, floor, as the MDEC
 * rounds; the full sum may not fit 32 bits, floor((lo + hi) / 2) does. Saturated to 16 bits. */
static int pass_round(int lo, int hi)
{
    int avg = (lo & hi) + ((lo ^ hi) >> 1);
    int v   = ((avg >> 16) + 1) >> 1;
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

/** Inverse DCT of one block into `out`, as the MDEC computes it (the arithmetic DuckStation models):
 * `in` holds the dequantized coefficients (natural order, row = vertical frequency) with 4 extra bits
 * of precision. Pass 1 transforms each column (vertical), pass 2 each row, both with the uploaded
 * IDCT matrix and pass_round; the result wraps to 9 bits and is clamped to -128..127. `cols`: bit u
 * set when column u has a nonzero coefficient; `rows`: the same for rows. Zero terms are skipped
 * (an intermediate column is zero exactly when the coefficient column is), and a block with only its
 * DC coefficient is one value: all exact. */
static void idct(const int* in, int* out, unsigned int cols, unsigned int rows)
{
    int tmp[64], t, u, v, x;
    if (cols == 1 && rows == 1)
    {
        int c = clamp9(pass_round(pass_round(in[0] * SCALE[0], 0) * SCALE[0], 0));
        for (x = 0; x < 64; x++)
        {
            out[x] = c;
        }
        return;
    }
    for (u = 0; u < 8; u++) /* pass 1: column u (horizontal frequency), vertical frequency v -> row t */
    {
        if (!(cols >> u & 1))
        {
            for (t = 0; t < 8; t++)
            {
                tmp[t * 8 + u] = 0;
            }
            continue;
        }
        for (t = 0; t < 8; t++)
        {
            int lo = 0, hi = 0;
            for (v = 0; v < 4; v++)
            {
                lo += in[v * 8 + u] * SCALE[v * 8 + t];
            }
            if (rows & 0xF0)
            {
                for (v = 4; v < 8; v++)
                {
                    hi += in[v * 8 + u] * SCALE[v * 8 + t];
                }
            }
            tmp[t * 8 + u] = pass_round(lo, hi);
        }
    }
    for (t = 0; t < 8; t++) /* pass 2: row t, horizontal frequency u -> column x */
    {
        const int* r = &tmp[t * 8];
        for (x = 0; x < 8; x++)
        {
            int lo = 0, hi = 0;
            for (u = 0; u < 4; u++)
            {
                lo += r[u] * SCALE[u * 8 + x];
            }
            if (cols & 0xF0)
            {
                for (u = 4; u < 8; u++)
                {
                    hi += r[u] * SCALE[u * 8 + x];
                }
            }
            out[t * 8 + x] = clamp9(pass_round(lo, hi));
        }
    }
}

/** The MDEC's dequantization (the arithmetic DuckStation models): coefficients keep 4 extra bits, a
 * half step toward zero, saturated to -0x4000..0x3FFF. DC: v * qt[0]; AC: floor(v * q * qt[k] / 8);
 * a zero scale passes v * 32. */
static int dequant_clamp(int c, int v)
{
    c += v ? (v < 0 ? 8 : -8) : 0;
    return c < -0x4000 ? -0x4000 : c > 0x3FFF ? 0x3FFF : c;
}

static int dequant(int v, int qt0)
{
    return qt0 == 0 ? (v << 5 < -0x4000 ? -0x4000 : v << 5 > 0x3FFF ? 0x3FFF : v << 5) : dequant_clamp((v * qt0) << 4, v);
}

static int dequant_ac(int v, int scq)
{
    return scq == 0 ? (v << 5 < -0x4000 ? -0x4000 : v << 5 > 0x3FFF ? 0x3FFF : v << 5) : dequant_clamp(((v * scq) >> 3) << 4, v);
}

/** One block: DC and AC codes into dequantized coefficients, then the inverse DCT. 0 on bad data. */
static int block(Bits* b, int qscale, int* out)
{
    int          coef[64], k = 0, i;
    unsigned int cols = 1, rows = 1;
    int          dc   = (int)get(b, 10);
    for (i = 0; i < 64; i++)
    {
        coef[i] = 0;
    }
    dc      = dc & 0x200 ? dc - 0x400 : dc;
    coef[0] = dequant(dc, QT[0]);
    for (;;)
    {
        unsigned int w = peek(b, 16);
        int          run, level;
        if ((w >> 14) == 2) /* "10": end of block */
        {
            b->pos += 2;
            break;
        }
        if ((w >> 10) == 1) /* "000001": escape */
        {
            b->pos += 6;
            run   = (int)get(b, 6);
            level = (int)get(b, 10);
            level = level & 0x200 ? level - 0x400 : level;
        }
        else
        {
            if (!s_LutLen[w])
            {
                return 0; /* not a code */
            }
            b->pos += s_LutLen[w];
            run   = s_LutRun[w];
            level = s_LutLevel[w];
            if (get(b, 1))
            {
                level = -level;
            }
        }
        k += run + 1;
        if (k > 63)
        {
            return 0;
        }
        coef[ZIGZAG[k]] = dequant_ac(level, QT[k] * qscale);
        cols |= 1u << (ZIGZAG[k] & 7);
        rows |= 1u << (ZIGZAG[k] >> 3);
    }
    idct(coef, out, cols, rows);
    return 1;
}

static unsigned char clamp8(int v)
{
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

int StrDec_Frame(const unsigned char* data, int size, int width, int height, unsigned char* rgb, int bpp)
{
    Bits b;
    int  qscale, version, mx, my, cols = (width + 15) / 16, rows = (height + 15) / 16;
    if (!s_LutReady)
    {
        int k;
        lut_build();
        for (k = 0; k < 64; k++)
        {
            QT[k] = QT_NATURAL[ZIGZAG[k]];
        }
    }
    if (size < 8 || (data[2] | (data[3] << 8)) != 0x3800)
    {
        return -1;
    }
    qscale  = data[4] | (data[5] << 8);
    version = data[6] | (data[7] << 8);
    if (version != 2)
    {
        return -2; /* only version 2 so far */
    }
    b.data = data + 8;
    b.size = size - 8;
    b.pos  = 0;
    for (mx = 0; mx < cols; mx++)
    {
        for (my = 0; my < rows; my++)
        {
            int cr[64], cb[64], y[4][64], i, px, py;
            if (!block(&b, qscale, cr) || !block(&b, qscale, cb))
            {
                return -3;
            }
            for (i = 0; i < 4; i++)
            {
                if (!block(&b, qscale, y[i]))
                {
                    return -3;
                }
            }
            for (py = 0; py < 16; py += 2)
            {
                for (px = 0; px < 16; px += 2)
                {
                    int c  = (py >> 1) * 8 + (px >> 1);
                    /* BT.601 with the MDEC's rounding (DuckStation, after Mednafen). */
                    int rc = (359 * cr[c] + 0x80) >> 8;
                    int gc = (((-88 * cb[c]) & ~0x1F) + ((-183 * cr[c]) & ~0x07) + 0x80) >> 8;
                    int bc = (454 * cb[c] + 0x80) >> 8;
                    int dy, dx;
                    for (dy = 0; dy < 2; dy++)
                    {
                        for (dx = 0; dx < 2; dx++)
                        {
                            int X = mx * 16 + px + dx, Y = my * 16 + py + dy, qy = py + dy, qx = px + dx;
                            int yy = y[(qy >> 3) * 2 + (qx >> 3)][(qy & 7) * 8 + (qx & 7)];
                            unsigned char* o;
                            if (X >= width || Y >= height)
                            {
                                continue;
                            }
                            o    = &rgb[(Y * width + X) * bpp];
                            o[0] = (unsigned char)(clamp9(yy + rc) + 128);
                            o[1] = (unsigned char)(clamp9(yy + gc) + 128);
                            o[2] = (unsigned char)(clamp9(yy + bc) + 128);
                            if (bpp == 4)
                            {
                                o[3] = 0x80;
                            }
                        }
                    }
                }
            }
        }
    }
    return b.pos;
}
