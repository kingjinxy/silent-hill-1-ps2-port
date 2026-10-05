/** @brief Software PS1 GTE. See include/port/gte.h.
 *
 * Reference: psx-spx, "Geometry Transformation Engine (GTE)". Section names from that document are
 * quoted in comments where a detail is non-obvious.
 */

#include "port/gte.h"

/* No <stdint.h>: the port build uses -nostdinc. int is 32-bit and long long 64-bit on both the EE
 * and the hosts this is tested on. */
typedef signed short       s16;
typedef signed int         s32;
typedef signed long long   s64;
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* Data registers (cop2r0-31). */
enum
{
    D_VXY0, D_VZ0, D_VXY1, D_VZ1, D_VXY2, D_VZ2, D_RGBC, D_OTZ,
    D_IR0, D_IR1, D_IR2, D_IR3, D_SXY0, D_SXY1, D_SXY2, D_SXYP,
    D_SZ0, D_SZ1, D_SZ2, D_SZ3, D_RGB0, D_RGB1, D_RGB2, D_RES1,
    D_MAC0, D_MAC1, D_MAC2, D_MAC3, D_IRGB, D_ORGB, D_LZCS, D_LZCR
};

/* Control registers (cop2r32-63 as 0-31). */
enum
{
    C_RT0, C_RT1, C_RT2, C_RT3, C_RT4, C_TRX, C_TRY, C_TRZ,
    C_LLM0, C_LLM1, C_LLM2, C_LLM3, C_LLM4, C_RBK, C_GBK, C_BBK,
    C_LCM0, C_LCM1, C_LCM2, C_LCM3, C_LCM4, C_RFC, C_GFC, C_BFC,
    C_OFX, C_OFY, C_H, C_DQA, C_DQB, C_ZSF3, C_ZSF4, C_FLAG
};

/* FLAG bits ("GTE Saturation"). */
#define F_MAC1_POS (1u << 30)
#define F_MAC1_NEG (1u << 27)
#define F_IR1      (1u << 24)
#define F_IR2      (1u << 23)
#define F_IR3      (1u << 22)
#define F_COLOR_R  (1u << 21)
#define F_COLOR_G  (1u << 20)
#define F_COLOR_B  (1u << 19)
#define F_SZ_OTZ   (1u << 18)
#define F_DIVIDE   (1u << 17)
#define F_MAC0_POS (1u << 16)
#define F_MAC0_NEG (1u << 15)
#define F_SX2      (1u << 14)
#define F_SY2      (1u << 13)
#define F_IR0      (1u << 12)
#define F_ERROR_MASK 0x7F87E000u /* bits 30-23 and 18-13 feed bit 31 */

static u32 d[32];
static u32 c[32];

/* Unsigned Newton-Raphson reciprocal table ("GTE Division Inaccuracy"). */
static u8   unr_table[0x101];
static int  unr_ready;

static void unr_init(void)
{
    int i;
    for (i = 0; i < 0x101; i++)
    {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        unr_table[i] = (u8)(v < 0 ? 0 : v);
    }
    unr_ready = 1;
}

void Gte_Reset(void)
{
    int i;
    for (i = 0; i < 32; i++)
    {
        d[i] = 0;
        c[i] = 0;
    }
}

/* ----------------------------------------------------------------------------------------------
 * Register access
 * --------------------------------------------------------------------------------------------*/

static s16 lo16(u32 v) { return (s16)(v & 0xFFFF); }
static s16 hi16(u32 v) { return (s16)(v >> 16); }

static u32 orgb(void)
{
    s32 r = (s16)d[D_IR1] >> 7;
    s32 g = (s16)d[D_IR2] >> 7;
    s32 b = (s16)d[D_IR3] >> 7;
    r = r < 0 ? 0 : r > 0x1F ? 0x1F : r;
    g = g < 0 ? 0 : g > 0x1F ? 0x1F : g;
    b = b < 0 ? 0 : b > 0x1F ? 0x1F : b;
    return (u32)(r | (g << 5) | (b << 10));
}

static u32 count_leading(u32 v)
{
    u32 n = 0;
    if ((s32)v < 0)
    {
        v = ~v;
    }
    while (n < 32 && !(v & 0x80000000u))
    {
        v <<= 1;
        n++;
    }
    return n;
}

void Gte_DataWrite(unsigned int reg, unsigned int value)
{
    reg &= 31;
    switch (reg)
    {
        /* 16-bit signed registers occupying a whole word: stored sign-extended. */
        case D_VZ0: case D_VZ1: case D_VZ2:
        case D_IR0: case D_IR1: case D_IR2: case D_IR3:
            d[reg] = (u32)(s32)(s16)value;
            break;

        /* Unsigned 16-bit. */
        case D_OTZ: case D_SZ0: case D_SZ1: case D_SZ2: case D_SZ3:
            d[reg] = value & 0xFFFF;
            break;

        case D_SXYP: /* Move-on-write mirror of SXY2. */
            d[D_SXY0] = d[D_SXY1];
            d[D_SXY1] = d[D_SXY2];
            d[D_SXY2] = value;
            break;

        case D_IRGB: /* Expands 5:5:5 into IR1-3. */
            d[D_IRGB] = value & 0x7FFF;
            d[D_IR1]  = (value & 0x1F) << 7;
            d[D_IR2]  = ((value >> 5) & 0x1F) << 7;
            d[D_IR3]  = ((value >> 10) & 0x1F) << 7;
            break;

        case D_ORGB: /* Read only. */
            break;

        case D_LZCS:
            d[D_LZCS] = value;
            d[D_LZCR] = count_leading(value);
            break;

        case D_LZCR: /* Read only. */
            break;

        default:
            d[reg] = value;
            break;
    }
}

unsigned int Gte_DataRead(unsigned int reg)
{
    reg &= 31;
    switch (reg)
    {
        case D_SXYP:
            return d[D_SXY2];

        case D_IRGB:
        case D_ORGB: /* ORGB is a read-only mirror of IRGB, which reflects IR1-3. */
            return orgb();

        default:
            return d[reg];
    }
}

void Gte_CtrlWrite(unsigned int reg, unsigned int value)
{
    reg &= 31;
    switch (reg)
    {
        /* Last matrix elements, H (unsigned), DQA, ZSF3/4: 16-bit. */
        case C_RT4: case C_LLM4: case C_LCM4:
        case C_DQA: case C_ZSF3: case C_ZSF4:
            c[reg] = (u32)(s32)(s16)value;
            break;

        case C_H:
            c[reg] = value & 0xFFFF;
            break;

        case C_FLAG:
            value &= 0x7FFFF000u;
            if (value & F_ERROR_MASK)
            {
                value |= 0x80000000u;
            }
            c[reg] = value;
            break;

        default:
            c[reg] = value;
            break;
    }
}

unsigned int Gte_CtrlRead(unsigned int reg)
{
    reg &= 31;
    if (reg == C_H)
    {
        return (u32)(s32)(s16)c[C_H]; /* Hardware bug: H reads back sign-expanded. */
    }
    return c[reg];
}

/* ----------------------------------------------------------------------------------------------
 * Arithmetic helpers
 * --------------------------------------------------------------------------------------------*/

static u32 flag;

/** 44-bit MAC accumulation: flags overflow of MACn (n=1..3) and wraps to 44 bits, as the hardware does
 * after each addition. */
static s64 mac44(int n, s64 v)
{
    if (v > 0x7FFFFFFFFFFLL)
    {
        flag |= F_MAC1_POS >> (n - 1);
    }
    else if (v < -0x80000000000LL)
    {
        flag |= F_MAC1_NEG >> (n - 1);
    }
    return (s64)((u64)v << 20) >> 20;
}

/** MAC0 (32-bit) overflow check; returns the value unchanged (MAC0 isn't saturated). */
static s64 mac0_check(s64 v)
{
    if (v > 0x7FFFFFFFLL)
    {
        flag |= F_MAC0_POS;
    }
    else if (v < -0x80000000LL)
    {
        flag |= F_MAC0_NEG;
    }
    return v;
}

static s32 sat_ir(int n, s64 v, int lm)
{
    s32 lo = lm ? 0 : -0x8000;
    if (v < lo)
    {
        flag |= F_IR1 >> (n - 1);
        return lo;
    }
    if (v > 0x7FFF)
    {
        flag |= F_IR1 >> (n - 1);
        return 0x7FFF;
    }
    return (s32)v;
}

static u32 sat_color(int n, s32 v)
{
    if (v < 0)
    {
        flag |= F_COLOR_R >> (n - 1);
        return 0;
    }
    if (v > 0xFF)
    {
        flag |= F_COLOR_R >> (n - 1);
        return 0xFF;
    }
    return (u32)v;
}

static u32 sat_sz(s64 v)
{
    if (v < 0)
    {
        flag |= F_SZ_OTZ;
        return 0;
    }
    if (v > 0xFFFF)
    {
        flag |= F_SZ_OTZ;
        return 0xFFFF;
    }
    return (u32)v;
}

/** Sets MAC1-3 from 44-bit results shifted by sf*12. */
static void set_mac123(s64 m1, s64 m2, s64 m3, int sf)
{
    int sh = sf ? 12 : 0;
    d[D_MAC1] = (u32)(s32)(m1 >> sh);
    d[D_MAC2] = (u32)(s32)(m2 >> sh);
    d[D_MAC3] = (u32)(s32)(m3 >> sh);
}

/** [IR1,IR2,IR3] = [MAC1,MAC2,MAC3], saturated per lm. */
static void ir_from_mac(int lm)
{
    d[D_IR1] = (u32)sat_ir(1, (s32)d[D_MAC1], lm);
    d[D_IR2] = (u32)sat_ir(2, (s32)d[D_MAC2], lm);
    d[D_IR3] = (u32)sat_ir(3, (s32)d[D_MAC3], lm);
}

/** Color FIFO push: [MAC1/16, MAC2/16, MAC3/16, CODE]. */
static void push_color(void)
{
    u32 r = sat_color(1, (s32)d[D_MAC1] >> 4);
    u32 g = sat_color(2, (s32)d[D_MAC2] >> 4);
    u32 b = sat_color(3, (s32)d[D_MAC3] >> 4);
    d[D_RGB0] = d[D_RGB1];
    d[D_RGB1] = d[D_RGB2];
    d[D_RGB2] = r | (g << 8) | (b << 16) | (d[D_RGBC] & 0xFF000000u);
}

static void push_sz(u32 z)
{
    d[D_SZ0] = d[D_SZ1];
    d[D_SZ1] = d[D_SZ2];
    d[D_SZ2] = d[D_SZ3];
    d[D_SZ3] = z;
}

static void push_sxy(s32 x, s32 y)
{
    d[D_SXY0] = d[D_SXY1];
    d[D_SXY1] = d[D_SXY2];
    d[D_SXY2] = ((u32)x & 0xFFFF) | ((u32)y << 16);
}

/* Matrices as 3x3 s16 from three consecutive control register blocks (RT, LLM, LCM). */
static void get_matrix(int base, s32 m[3][3])
{
    m[0][0] = lo16(c[base + 0]); m[0][1] = hi16(c[base + 0]);
    m[0][2] = lo16(c[base + 1]); m[1][0] = hi16(c[base + 1]);
    m[1][1] = lo16(c[base + 2]); m[1][2] = hi16(c[base + 2]);
    m[2][0] = lo16(c[base + 3]); m[2][1] = hi16(c[base + 3]);
    m[2][2] = lo16(c[base + 4]);
}

static void get_vector(int v, s32 out[3])
{
    if (v == 3)
    {
        out[0] = (s16)d[D_IR1];
        out[1] = (s16)d[D_IR2];
        out[2] = (s16)d[D_IR3];
        return;
    }
    out[0] = lo16(d[D_VXY0 + v * 2]);
    out[1] = hi16(d[D_VXY0 + v * 2]);
    out[2] = lo16(d[D_VZ0 + v * 2]);
}

/** MACn = (T*1000h + M*V), with the 44-bit check after each addition. Returns unshifted. */
static s64 mat_row(int n, s64 t, const s32 m[3], const s32 v[3])
{
    s64 acc = mac44(n, t * 0x1000);
    acc     = mac44(n, acc + (s64)m[0] * v[0]);
    acc     = mac44(n, acc + (s64)m[1] * v[1]);
    acc     = mac44(n, acc + (s64)m[2] * v[2]);
    return acc;
}

/** General MVMVA-style operation into MAC1-3 and IR1-3. */
static void mul_mat_vec(const s32 m[3][3], const s32 v[3], const s64 t[3], int sf, int lm)
{
    s64 r1 = mat_row(1, t[0], m[0], v);
    s64 r2 = mat_row(2, t[1], m[1], v);
    s64 r3 = mat_row(3, t[2], m[2], v);
    set_mac123(r1, r2, r3, sf);
    ir_from_mac(lm);
}

/* UNR division for RTPS/RTPT: (((H*20000h/SZ3)+1)/2), saturated to 1FFFFh. */
static u32 gte_divide(u32 h, u32 sz3)
{
    if (h < sz3 * 2)
    {
        u32 z = 0, n, dd, u;
        u64 r;
        while (z < 16 && !(sz3 & (0x8000u >> z)))
        {
            z++;
        }
        n  = h << z;
        dd = sz3 << z;
        u  = unr_table[(dd - 0x7FC0) >> 7] + 0x101;
        dd = (0x2000080u - (dd * u)) >> 8;
        dd = (0x0000080u + (dd * u)) >> 8;
        r  = (((u64)n * dd) + 0x8000) >> 16;
        return r > 0x1FFFF ? 0x1FFFF : (u32)r;
    }
    flag |= F_DIVIDE;
    return 0x1FFFF;
}

/* ----------------------------------------------------------------------------------------------
 * Commands
 * --------------------------------------------------------------------------------------------*/

static void rtp(int v, int sf, int lm, int last)
{
    s32 rt[3][3], vec[3];
    s64 m1, m2, m3, mac0;
    s32 sx, sy;
    u32 n;

    get_matrix(C_RT0, rt);
    get_vector(v, vec);
    m1 = mat_row(1, (s32)c[C_TRX], rt[0], vec);
    m2 = mat_row(2, (s32)c[C_TRY], rt[1], vec);
    m3 = mat_row(3, (s32)c[C_TRZ], rt[2], vec);
    set_mac123(m1, m2, m3, sf);

    d[D_IR1] = (u32)sat_ir(1, (s32)d[D_MAC1], lm);
    d[D_IR2] = (u32)sat_ir(2, (s32)d[D_MAC2], lm);
    /* IR3: stored value respects lm, but FLAG.22 is checked on MAC3 SAR 12 as if lm=0. */
    {
        s32 mac3 = (s32)d[D_MAC3];
        s32 chk  = (s32)(m3 >> 12);
        s32 lo   = lm ? 0 : -0x8000;
        if (chk < -0x8000 || chk > 0x7FFF)
        {
            flag |= F_IR3;
        }
        d[D_IR3] = (u32)(mac3 < lo ? lo : mac3 > 0x7FFF ? 0x7FFF : mac3);
    }

    push_sz(sat_sz(m3 >> 12)); /* SZ3 = MAC3 SAR ((1-sf)*12), i.e. the unshifted sum SAR 12 either way. */

    n = gte_divide(c[C_H] & 0xFFFF, d[D_SZ3]);

    mac0      = mac0_check((s64)n * (s16)d[D_IR1] + (s32)c[C_OFX]);
    d[D_MAC0] = (u32)(s32)mac0;
    sx        = (s32)(mac0 >> 16);
    if (sx < -0x400) { sx = -0x400; flag |= F_SX2; }
    if (sx > 0x3FF)  { sx = 0x3FF;  flag |= F_SX2; }

    mac0      = mac0_check((s64)n * (s16)d[D_IR2] + (s32)c[C_OFY]);
    d[D_MAC0] = (u32)(s32)mac0;
    sy        = (s32)(mac0 >> 16);
    if (sy < -0x400) { sy = -0x400; flag |= F_SY2; }
    if (sy > 0x3FF)  { sy = 0x3FF;  flag |= F_SY2; }
    push_sxy(sx, sy);

    if (last)
    {
        s64 ir0;
        mac0       = mac0_check((s64)n * (s16)c[C_DQA] + (s32)c[C_DQB]);
        d[D_MAC0]  = (u32)(s32)mac0;
        ir0        = mac0 >> 12;
        if (ir0 < 0)      { ir0 = 0;      flag |= F_IR0; }
        if (ir0 > 0x1000) { ir0 = 0x1000; flag |= F_IR0; }
        d[D_IR0] = (u32)(s32)ir0;
    }
}

static void nclip(void)
{
    s64 sx0 = lo16(d[D_SXY0]), sy0 = hi16(d[D_SXY0]);
    s64 sx1 = lo16(d[D_SXY1]), sy1 = hi16(d[D_SXY1]);
    s64 sx2 = lo16(d[D_SXY2]), sy2 = hi16(d[D_SXY2]);
    s64 v   = sx0 * sy1 + sx1 * sy2 + sx2 * sy0 - sx0 * sy2 - sx1 * sy0 - sx2 * sy1;
    d[D_MAC0] = (u32)(s32)mac0_check(v);
}

static void avsz(int four)
{
    s64 sum = (s64)d[D_SZ1] + d[D_SZ2] + d[D_SZ3];
    s64 v;
    if (four)
    {
        sum += d[D_SZ0];
    }
    v         = mac0_check((s64)(s16)c[four ? C_ZSF4 : C_ZSF3] * sum);
    d[D_MAC0] = (u32)(s32)v;
    d[D_OTZ]  = sat_sz(v >> 12);
}

static void op(int sf, int lm)
{
    s64 d1 = lo16(c[C_RT0]), d2 = lo16(c[C_RT2]), d3 = lo16(c[C_RT4]);
    s64 i1 = (s16)d[D_IR1], i2 = (s16)d[D_IR2], i3 = (s16)d[D_IR3];
    s64 m1 = mac44(1, mac44(1, i3 * d2) - i2 * d3);
    s64 m2 = mac44(2, mac44(2, i1 * d3) - i3 * d1);
    s64 m3 = mac44(3, mac44(3, i2 * d1) - i1 * d2);
    set_mac123(m1, m2, m3, sf);
    ir_from_mac(lm);
}

static void sqr(int sf, int lm)
{
    s64 i1 = (s16)d[D_IR1], i2 = (s16)d[D_IR2], i3 = (s16)d[D_IR3];
    set_mac123(mac44(1, i1 * i1), mac44(2, i2 * i2), mac44(3, i3 * i3), sf);
    ir_from_mac(lm);
}

static void mvmva(int sf, int mx, int v, int cv, int lm)
{
    s32 m[3][3], vec[3];
    s64 t[3] = { 0, 0, 0 };

    if (mx == 0)
    {
        get_matrix(C_RT0, m);
    }
    else if (mx == 1)
    {
        get_matrix(C_LLM0, m);
    }
    else if (mx == 2)
    {
        get_matrix(C_LCM0, m);
    }
    else
    {
        /* Garbage matrix: -R*10h, +R*10h, IR0, RT13, RT13, RT13, RT22, RT22, RT22. */
        s32 r = (s32)(d[D_RGBC] & 0xFF) << 4;
        m[0][0] = -r;        m[0][1] = r;                m[0][2] = (s16)d[D_IR0];
        m[1][0] = lo16(c[C_RT1]); m[1][1] = lo16(c[C_RT1]); m[1][2] = lo16(c[C_RT1]);
        m[2][0] = lo16(c[C_RT2]); m[2][1] = lo16(c[C_RT2]); m[2][2] = lo16(c[C_RT2]);
    }
    get_vector(v, vec);

    if (cv == 0)
    {
        t[0] = (s32)c[C_TRX]; t[1] = (s32)c[C_TRY]; t[2] = (s32)c[C_TRZ];
    }
    else if (cv == 1)
    {
        t[0] = (s32)c[C_RBK]; t[1] = (s32)c[C_GBK]; t[2] = (s32)c[C_BBK];
    }
    else if (cv == 2)
    {
        /* FC bug: the first column product (with T) only affects FLAG; the result is only the
         * last two products. */
        s32 fc[3];
        s64 r[3];
        int i;
        fc[0] = (s32)c[C_RFC]; fc[1] = (s32)c[C_GFC]; fc[2] = (s32)c[C_BFC];
        for (i = 0; i < 3; i++)
        {
            s64 bogus = mac44(i + 1, (s64)fc[i] * 0x1000);
            bogus     = mac44(i + 1, bogus + (s64)m[i][0] * vec[0]);
            sat_ir(i + 1, bogus >> (sf ? 12 : 0), 0);
            r[i] = mac44(i + 1, (s64)m[i][1] * vec[1]);
            r[i] = mac44(i + 1, r[i] + (s64)m[i][2] * vec[2]);
        }
        set_mac123(r[0], r[1], r[2], sf);
        ir_from_mac(lm);
        return;
    }
    mul_mat_vec(m, vec, t, sf, lm);
}

/** MAC = MAC + (FC - MAC) * IR0, then SAR sf*12 ("Details on MAC+(FC-MAC)*IR0"). Input is the
 * unshifted 44-bit MAC values. */
static void interp_fc(s64 m[3], int sf, int lm)
{
    s32 fc[3];
    s32 ir[3];
    s64 ir0 = (s16)d[D_IR0];
    int i;
    fc[0] = (s32)c[C_RFC]; fc[1] = (s32)c[C_GFC]; fc[2] = (s32)c[C_BFC];
    for (i = 0; i < 3; i++)
    {
        /* The difference is overflow-checked as a MAC value, but IR saturates from the shifted
         * result truncated to 32 bits, not from the full value (verified against the PS1 GTE). */
        s64 t = ((s64)fc[i] << 12) - m[i];
        mac44(i + 1, t);
        ir[i] = sat_ir(i + 1, (s32)(t >> (sf ? 12 : 0)), 0);
    }
    for (i = 0; i < 3; i++)
    {
        m[i] = mac44(i + 1, (s64)ir[i] * ir0 + m[i]);
    }
    set_mac123(m[0], m[1], m[2], sf);
    ir_from_mac(lm);
}

/** Color stage shared by NCx/CC/CDP/DCPL/DPCx/INTPL: from the light color result in IR1-3 to the
 * color FIFO. mode: 0 = plain (NCS/NCT), 1 = color (NCCx/CC), 2 = color + depth cue (NCDx/CDP). */
static void color_stage(int mode, int sf, int lm)
{
    if (mode == 0)
    {
        push_color();
        return;
    }
    {
        s64 m[3];
        s64 r = d[D_RGBC] & 0xFF, g = (d[D_RGBC] >> 8) & 0xFF, b = (d[D_RGBC] >> 16) & 0xFF;
        m[0] = mac44(1, (r * (s16)d[D_IR1]) << 4);
        m[1] = mac44(2, (g * (s16)d[D_IR2]) << 4);
        m[2] = mac44(3, (b * (s16)d[D_IR3]) << 4);
        if (mode == 2)
        {
            interp_fc(m, sf, lm);
        }
        else
        {
            set_mac123(m[0], m[1], m[2], sf);
            ir_from_mac(lm);
        }
        push_color();
    }
}

/** (BK*1000h + LCM*IR) SAR sf*12 into MAC/IR. */
static void light_color(int sf, int lm)
{
    s32 lcm[3][3], ir[3];
    s64 bk[3];
    get_matrix(C_LCM0, lcm);
    get_vector(3, ir);
    bk[0] = (s32)c[C_RBK]; bk[1] = (s32)c[C_GBK]; bk[2] = (s32)c[C_BBK];
    mul_mat_vec(lcm, ir, bk, sf, lm);
}

static void normal_color(int v, int mode, int sf, int lm)
{
    s32 llm[3][3], vec[3];
    s64 zero[3] = { 0, 0, 0 };
    get_matrix(C_LLM0, llm);
    get_vector(v, vec);
    mul_mat_vec(llm, vec, zero, sf, lm);
    light_color(sf, lm);
    color_stage(mode, sf, lm);
}

/** DPCS/DPCT/INTPL/DCPL: start values, then MAC + (FC - MAC) * IR0, then FIFO. */
static void depth_cue_from(s64 m[3], int sf, int lm)
{
    interp_fc(m, sf, lm);
    push_color();
}

static void dpcs_rgb(u32 rgb, int sf, int lm)
{
    s64 m[3];
    m[0] = mac44(1, (s64)(rgb & 0xFF) << 16);
    m[1] = mac44(2, (s64)((rgb >> 8) & 0xFF) << 16);
    m[2] = mac44(3, (s64)((rgb >> 16) & 0xFF) << 16);
    depth_cue_from(m, sf, lm);
}

static void gpf_gpl(int base, int sf, int lm)
{
    s64 m[3];
    s64 ir0 = (s16)d[D_IR0];
    int sh  = sf ? 12 : 0;
    if (base)
    {
        m[0] = (s64)(s32)d[D_MAC1] << sh;
        m[1] = (s64)(s32)d[D_MAC2] << sh;
        m[2] = (s64)(s32)d[D_MAC3] << sh;
    }
    else
    {
        m[0] = m[1] = m[2] = 0;
    }
    m[0] = mac44(1, (s64)(s16)d[D_IR1] * ir0 + m[0]);
    m[1] = mac44(2, (s64)(s16)d[D_IR2] * ir0 + m[1]);
    m[2] = mac44(3, (s64)(s16)d[D_IR3] * ir0 + m[2]);
    set_mac123(m[0], m[1], m[2], sf);
    ir_from_mac(lm);
    push_color();
}

#ifdef SH_PORT_PROF
#include "port/prof.h"
static void gte_command(unsigned int cmd);
static const char* const OP_NAMES[64] = {
    "GTE 00", "GTE RTPS", "GTE 02", "GTE 03", "GTE 04", "GTE 05", "GTE NCLIP", "GTE 07",
    "GTE 08", "GTE 09", "GTE 0A", "GTE 0B", "GTE OP", "GTE 0D", "GTE 0E", "GTE 0F",
    "GTE DPCS", "GTE INTPL", "GTE MVMVA", "GTE NCDS", "GTE CDP", "GTE 15", "GTE NCDT", "GTE 17",
    "GTE 18", "GTE 19", "GTE 1A", "GTE NCCS", "GTE CC", "GTE 1D", "GTE NCS", "GTE 1F",
    "GTE NCT", "GTE 21", "GTE 22", "GTE 23", "GTE 24", "GTE 25", "GTE 26", "GTE 27",
    "GTE SQR", "GTE DCPL", "GTE DPCT", "GTE 2B", "GTE 2C", "GTE AVSZ3", "GTE AVSZ4", "GTE 2F",
    "GTE RTPT", "GTE 31", "GTE 32", "GTE 33", "GTE 34", "GTE 35", "GTE 36", "GTE 37",
    "GTE 38", "GTE 39", "GTE 3A", "GTE 3B", "GTE 3C", "GTE GPF", "GTE GPL", "GTE NCCT",
};
void Gte_Command(unsigned int cmd)
{
    unsigned int t;
    Prof_Begin(OP_NAMES[cmd & 63], &t);
    gte_command(cmd);
    Prof_End(OP_NAMES[cmd & 63], t);
}
#define Gte_Command gte_command
static
#endif
void Gte_Command(unsigned int cmd)
{
    int sf = (cmd >> 19) & 1;
    int mx = (cmd >> 17) & 3;
    int v  = (cmd >> 15) & 3;
    int cv = (cmd >> 13) & 3;
    int lm = (cmd >> 10) & 1;

    if (!unr_ready)
    {
        unr_init();
    }
    flag = 0;

    switch (cmd & 0x3F)
    {
        case 0x01: rtp(0, sf, lm, 1); break;                                  /* RTPS */
        case 0x30: rtp(0, sf, lm, 0); rtp(1, sf, lm, 0); rtp(2, sf, lm, 1); break; /* RTPT */
        case 0x06: nclip(); break;                                            /* NCLIP */
        case 0x0C: op(sf, lm); break;                                         /* OP */
        case 0x10: dpcs_rgb(d[D_RGBC], sf, lm); break;                        /* DPCS */
        case 0x11:                                                            /* INTPL */
        {
            s64 m[3];
            m[0] = mac44(1, (s64)(s16)d[D_IR1] << 12);
            m[1] = mac44(2, (s64)(s16)d[D_IR2] << 12);
            m[2] = mac44(3, (s64)(s16)d[D_IR3] << 12);
            depth_cue_from(m, sf, lm);
            break;
        }
        case 0x12: mvmva(sf, mx, v, cv, lm); break;                           /* MVMVA */
        case 0x13: normal_color(0, 2, sf, lm); break;                         /* NCDS */
        case 0x14: light_color(sf, lm); color_stage(2, sf, lm); break;        /* CDP */
        case 0x16:                                                            /* NCDT */
            normal_color(0, 2, sf, lm); normal_color(1, 2, sf, lm); normal_color(2, 2, sf, lm);
            break;
        case 0x1B: normal_color(0, 1, sf, lm); break;                         /* NCCS */
        case 0x1C: light_color(sf, lm); color_stage(1, sf, lm); break;        /* CC */
        case 0x1E: normal_color(0, 0, sf, lm); break;                         /* NCS */
        case 0x20:                                                            /* NCT */
            normal_color(0, 0, sf, lm); normal_color(1, 0, sf, lm); normal_color(2, 0, sf, lm);
            break;
        case 0x28: sqr(sf, lm); break;                                        /* SQR */
        case 0x29:                                                            /* DCPL */
        {
            s64 m[3];
            s64 r = d[D_RGBC] & 0xFF, g = (d[D_RGBC] >> 8) & 0xFF, b = (d[D_RGBC] >> 16) & 0xFF;
            m[0] = mac44(1, (r * (s16)d[D_IR1]) << 4);
            m[1] = mac44(2, (g * (s16)d[D_IR2]) << 4);
            m[2] = mac44(3, (b * (s16)d[D_IR3]) << 4);
            depth_cue_from(m, sf, lm);
            break;
        }
        case 0x2A:                                                            /* DPCT: from RGB0, thrice */
            dpcs_rgb(d[D_RGB0], sf, lm); dpcs_rgb(d[D_RGB0], sf, lm); dpcs_rgb(d[D_RGB0], sf, lm);
            break;
        case 0x2D: avsz(0); break;                                            /* AVSZ3 */
        case 0x2E: avsz(1); break;                                            /* AVSZ4 */
        case 0x3D: gpf_gpl(0, sf, lm); break;                                 /* GPF */
        case 0x3E: gpf_gpl(1, sf, lm); break;                                 /* GPL */
        case 0x3F:                                                            /* NCCT */
            normal_color(0, 1, sf, lm); normal_color(1, 1, sf, lm); normal_color(2, 1, sf, lm);
            break;
        default:
            break;
    }

    if (flag & F_ERROR_MASK)
    {
        flag |= 0x80000000u;
    }
    c[C_FLAG] = flag;
}
