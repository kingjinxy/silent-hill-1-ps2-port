/** @brief Software PS1 GTE. See include/port/gte.h.
 *
 * Reference: psx-spx, "Geometry Transformation Engine (GTE)". Section names from that document are
 * quoted in comments where a detail is non-obvious.
 */

#define GTE_IMPLEMENTATION
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

u32 g_GteData[32];
u32 g_GteCtrl[32];
#define d g_GteData
#define c g_GteCtrl

/* Speed-ups (results unchanged; tools/port/gte_test checks every register on the host and the EE):
 *  - The rotation, light and light-colour matrices, unpacked from RT/LLM/LCM when written.
 *  - s_Small: TR, BK and FC are all within +-2^30. Then no 44-bit MAC check can trigger: every
 *    other operand is 16-bit (products below 2^30), so sums stay below 2^43; mac44() skips its
 *    checks (s_FastMac, per command: not for GPF/GPL, which add a shifted 32-bit MAC). */
static s32 m_rt[9], m_llm[9], m_lcm[9];
#ifdef _EE
/* RT for MMI (rt_dot3): [r00 r01 r02 0 r10 r11 r12 0], [r20 r21 r22 0 0 0 0 0]. */
static s16 m_rt_mmi[16] __attribute__((aligned(16)));
#endif
static int s_Small = 1;
static int s_FastMac;

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

static void unpack_matrix(int base, s32* m)
{
    m[0] = (s16)(c[base + 0] & 0xFFFF); m[1] = (s16)(c[base + 0] >> 16);
    m[2] = (s16)(c[base + 1] & 0xFFFF); m[3] = (s16)(c[base + 1] >> 16);
    m[4] = (s16)(c[base + 2] & 0xFFFF); m[5] = (s16)(c[base + 2] >> 16);
    m[6] = (s16)(c[base + 3] & 0xFFFF); m[7] = (s16)(c[base + 3] >> 16);
    m[8] = (s16)(c[base + 4] & 0xFFFF);
}

static int small30(u32 v)
{
    return (s32)v >= -0x40000000 && (s32)v < 0x40000000;
}

/** Keeps the unpacked matrices and s_Small in step with a control register write. */
static void ctrl_written(unsigned int reg)
{
    if (reg <= C_RT4)
    {
        unpack_matrix(C_RT0, m_rt);
#ifdef _EE
        m_rt_mmi[0] = (s16)m_rt[0]; m_rt_mmi[1] = (s16)m_rt[1]; m_rt_mmi[2]  = (s16)m_rt[2];
        m_rt_mmi[4] = (s16)m_rt[3]; m_rt_mmi[5] = (s16)m_rt[4]; m_rt_mmi[6]  = (s16)m_rt[5];
        m_rt_mmi[8] = (s16)m_rt[6]; m_rt_mmi[9] = (s16)m_rt[7]; m_rt_mmi[10] = (s16)m_rt[8];
#endif
    }
    else if (reg >= C_LLM0 && reg <= C_LLM4)
    {
        unpack_matrix(C_LLM0, m_llm);
    }
    else if (reg >= C_LCM0 && reg <= C_LCM4)
    {
        unpack_matrix(C_LCM0, m_lcm);
    }
    else if ((reg >= C_TRX && reg <= C_TRZ) || (reg >= C_RBK && reg <= C_BBK) || (reg >= C_RFC && reg <= C_BFC))
    {
        s_Small = small30(c[C_TRX]) && small30(c[C_TRY]) && small30(c[C_TRZ]) && small30(c[C_RBK]) &&
                  small30(c[C_GBK]) && small30(c[C_BBK]) && small30(c[C_RFC]) && small30(c[C_GFC]) &&
                  small30(c[C_BFC]);
    }
}

void Gte_Reset(void)
{
    int i;
    for (i = 0; i < 32; i++)
    {
        d[i] = 0;
        c[i] = 0;
    }
    for (i = 0; i < 9; i++)
    {
        m_rt[i] = m_llm[i] = m_lcm[i] = 0;
    }
    s_Small = 1;
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
    ctrl_written(reg);
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
static s64 mac44_checked(int n, s64 v) __attribute__((noinline));
#define mac44(n, v) (s_FastMac ? (s64)(v) : mac44_checked((n), (v)))

static s64 mac44_checked(int n, s64 v)
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
    const s32* src = base == C_RT0 ? m_rt : base == C_LLM0 ? m_llm : m_lcm;
    m[0][0] = src[0]; m[0][1] = src[1]; m[0][2] = src[2];
    m[1][0] = src[3]; m[1][1] = src[4]; m[1][2] = src[5];
    m[2][0] = src[6]; m[2][1] = src[7]; m[2][2] = src[8];
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

/** Leading zero count of a nonzero value below 2^31. */
static u32 clz32(u32 v)
{
#ifdef _EE
    u64 r;
    /* MMI PLZCW: leading bits equal to the sign bit, minus one (per 32-bit half). */
    __asm__("plzcw %0, %1" : "=r"(r) : "r"((u64)v));
    return (u32)(r & 0xFFFFFFFF) + 1;
#else
    return (u32)__builtin_clz(v);
#endif
}

/* UNR division for RTPS/RTPT: (((H*20000h/SZ3)+1)/2), saturated to 1FFFFh. */
static u32 gte_divide(u32 h, u32 sz3)
{
    if (h < sz3 * 2)
    {
        u32 z = 0, n, dd, u;
        u64 r;
        z = clz32(sz3) - 16; /* sz3 is 1..FFFFh here */
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

static __attribute__((noinline)) void rtp(int v, int sf, int lm, int last)
{
    s32 rt[3][3], vec[3];
    s64 m1, m2, m3, mac0;
    s32 sx, sy;
    u32 n; /* at most 1FFFFh: products are done as s32 x s32 (one MULT on the EE, no 64-bit multiply) */

    if (s_FastMac)
    {
        /* No 44-bit check can trigger (s_Small): plain sums, matrix from the cache. Products are
         * s16 x s16 (one 32-bit MULT each). */
        s32 vx = lo16(d[D_VXY0 + v * 2]), vy = hi16(d[D_VXY0 + v * 2]), vz = lo16(d[D_VZ0 + v * 2]);
        m1 = ((s64)(s32)c[C_TRX] << 12) + (s64)(m_rt[0] * vx) + (s64)(m_rt[1] * vy) + (s64)(m_rt[2] * vz);
        m2 = ((s64)(s32)c[C_TRY] << 12) + (s64)(m_rt[3] * vx) + (s64)(m_rt[4] * vy) + (s64)(m_rt[5] * vz);
        m3 = ((s64)(s32)c[C_TRZ] << 12) + (s64)(m_rt[6] * vx) + (s64)(m_rt[7] * vy) + (s64)(m_rt[8] * vz);
    }
    else
    {
        get_matrix(C_RT0, rt);
        get_vector(v, vec);
        m1 = mat_row(1, (s32)c[C_TRX], rt[0], vec);
        m2 = mat_row(2, (s32)c[C_TRY], rt[1], vec);
        m3 = mat_row(3, (s32)c[C_TRZ], rt[2], vec);
    }
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

    mac0      = mac0_check((s64)(s32)n * (s16)d[D_IR1] + (s32)c[C_OFX]);
    d[D_MAC0] = (u32)(s32)mac0;
    sx        = (s32)(mac0 >> 16);
    if (sx < -0x400) { sx = -0x400; flag |= F_SX2; }
    if (sx > 0x3FF)  { sx = 0x3FF;  flag |= F_SX2; }

    mac0      = mac0_check((s64)(s32)n * (s16)d[D_IR2] + (s32)c[C_OFY]);
    d[D_MAC0] = (u32)(s32)mac0;
    sy        = (s32)(mac0 >> 16);
    if (sy < -0x400) { sy = -0x400; flag |= F_SY2; }
    if (sy > 0x3FF)  { sy = 0x3FF;  flag |= F_SY2; }
    push_sxy(sx, sy);

    if (last)
    {
        s64 ir0;
        mac0       = mac0_check((s64)(s32)n * (s16)c[C_DQA] + (s32)c[C_DQB]);
        d[D_MAC0]  = (u32)(s32)mac0;
        ir0        = mac0 >> 12;
        if (ir0 < 0)      { ir0 = 0;      flag |= F_IR0; }
        if (ir0 > 0x1000) { ir0 = 0x1000; flag |= F_IR0; }
        d[D_IR0] = (u32)(s32)ir0;
    }
}

/** RTPS/RTPT for one vertex, when nothing saturates or overflows: computed in registers, committed
 * only if every result is in range (FLAG stays 0, MAC/IR/FIFOs exactly as rtp() would leave them).
 * Returns 0 without changing any register otherwise, and rtp() then does the full work. */
#ifdef _EE
/** RT * V with MMI: PHMADH gives the sums of adjacent products ([r0 r1 r2 0] . [vx vy vz 0] as
 * r0*vx + r1*vy, r2*vz + 0), exact while no pair sum reaches 2^31 (only possible with vx or vy
 * = -8000h: the caller checks). */
static void rt_dot3(u32 vxy, u32 vz, s64 out[3])
{
    static u64 vbuf[2] __attribute__((aligned(16)));
    s32        r[8] __attribute__((aligned(16)));
    u64        x = (u64)vxy | ((u64)(vz & 0xFFFF) << 32);
    vbuf[0] = x;
    vbuf[1] = x;
    __asm__ volatile("lq $8, 0(%1)\n\t"
                     "lq $9, 0(%2)\n\t"
                     "lq $10, 16(%2)\n\t"
                     "phmadh $11, $9, $8\n\t"
                     "phmadh $12, $10, $8\n\t"
                     "sq $11, 0(%0)\n\t"
                     "sq $12, 16(%0)"
                     :
                     : "r"(r), "r"(vbuf), "r"(m_rt_mmi)
                     : "$8", "$9", "$10", "$11", "$12", "hi", "lo", "memory");
    out[0] = (s64)r[0] + r[1];
    out[1] = (s64)r[2] + r[3];
    out[2] = (s64)r[4] + r[5];
}
#endif

static __attribute__((noinline)) int rtp_fast(int v, int sf, int lm, int last)
{
    s32 vx = (s16)(d[D_VXY0 + v * 2] & 0xFFFF);
    s32 vy = (s16)(d[D_VXY0 + v * 2] >> 16);
    s32 vz = (s16)(d[D_VZ0 + v * 2] & 0xFFFF);
    s64 m1, m2, m3;
    s32 mac1, mac2, mac3, sz, lo, mx, my, ir0 = 0;
    s64 mac0x, mac0y, mac0z = 0;
    u32 n, h, z, nn, dd, u;

    if (!s_Small)
    {
        return 0;
    }
    /* TR * 1000h + RT * V: below 2^43, no 44-bit overflow (s_Small). Products are s16 x s16. */
#ifdef _EE
    if (vx != -0x8000 && vy != -0x8000)
    {
        s64 p[3];
        rt_dot3(d[D_VXY0 + v * 2], d[D_VZ0 + v * 2], p);
        m1 = ((s64)(s32)c[C_TRX] << 12) + p[0];
        m2 = ((s64)(s32)c[C_TRY] << 12) + p[1];
        m3 = ((s64)(s32)c[C_TRZ] << 12) + p[2];
    }
    else
#endif
    {
        m1 = ((s64)(s32)c[C_TRX] << 12) + (s64)(m_rt[0] * vx) + (s64)(m_rt[1] * vy) + (s64)(m_rt[2] * vz);
        m2 = ((s64)(s32)c[C_TRY] << 12) + (s64)(m_rt[3] * vx) + (s64)(m_rt[4] * vy) + (s64)(m_rt[5] * vz);
        m3 = ((s64)(s32)c[C_TRZ] << 12) + (s64)(m_rt[6] * vx) + (s64)(m_rt[7] * vy) + (s64)(m_rt[8] * vz);
    }
    mac1 = (s32)(sf ? m1 >> 12 : m1);
    mac2 = (s32)(sf ? m2 >> 12 : m2);
    mac3 = (s32)(sf ? m3 >> 12 : m3);
    lo   = lm ? 0 : -0x8000;
    if (mac1 < lo || mac1 > 0x7FFF || mac2 < lo || mac2 > 0x7FFF || mac3 < lo || mac3 > 0x7FFF)
    {
        return 0; /* IR saturation */
    }
    sz = (s32)(m3 >> 12);
    if (sz < 0 || sz > 0x7FFF)
    {
        return 0; /* IR3 flag check (as lm=0) or SZ saturation */
    }
    h = c[C_H] & 0xFFFF;
    if (h >= (u32)sz * 2)
    {
        return 0; /* divide overflow */
    }
    z  = clz32((u32)sz) - 16;
    nn = h << z;
    dd = (u32)sz << z;
    u  = unr_table[(dd - 0x7FC0) >> 7] + 0x101;
    dd = (0x2000080u - (dd * u)) >> 8;
    dd = (0x0000080u + (dd * u)) >> 8;
    {
        u64 r = (((u64)nn * dd) + 0x8000) >> 16;
        n     = r > 0x1FFFF ? 0x1FFFF : (u32)r;
    }
    mac0x = (s64)(s32)n * mac1 + (s32)c[C_OFX];
    mac0y = (s64)(s32)n * mac2 + (s32)c[C_OFY];
    if (mac0x != (s32)mac0x || mac0y != (s32)mac0y)
    {
        return 0; /* MAC0 overflow */
    }
    mx = (s32)(mac0x >> 16);
    my = (s32)(mac0y >> 16);
    if (mx < -0x400 || mx > 0x3FF || my < -0x400 || my > 0x3FF)
    {
        return 0; /* SX2/SY2 saturation */
    }
    if (last)
    {
        mac0z = (s64)(s32)n * (s16)c[C_DQA] + (s32)c[C_DQB];
        if (mac0z != (s32)mac0z)
        {
            return 0;
        }
        ir0 = (s32)(mac0z >> 12);
        if (ir0 < 0 || ir0 > 0x1000)
        {
            return 0;
        }
    }

    /* Commit (same register updates as rtp()). */
    d[D_MAC1] = (u32)mac1;
    d[D_MAC2] = (u32)mac2;
    d[D_MAC3] = (u32)mac3;
    d[D_IR1]  = (u32)mac1;
    d[D_IR2]  = (u32)mac2;
    d[D_IR3]  = (u32)mac3;
    d[D_SZ0]  = d[D_SZ1];
    d[D_SZ1]  = d[D_SZ2];
    d[D_SZ2]  = d[D_SZ3];
    d[D_SZ3]  = (u32)sz;
    d[D_SXY0] = d[D_SXY1];
    d[D_SXY1] = d[D_SXY2];
    d[D_SXY2] = ((u32)mx & 0xFFFF) | ((u32)my << 16);
    d[D_MAC0] = (u32)(s32)(last ? mac0z : mac0y);
    if (last)
    {
        d[D_IR0] = (u32)ir0;
    }
    return 1;
}

/** DPCS from `rgb` when nothing saturates (see rtp_fast): MAC = RGB*10000h + (FC*1000h - RGB*10000h)
 * * IR0, then the colour FIFO. Returns 0 without changing any register otherwise. */
static __attribute__((noinline)) int dpcs_fast(u32 rgb, int sf, int lm)
{
    s32 mac[3], col[3], i, lo = lm ? 0 : -0x8000;
    s32 ir0 = (s16)d[D_IR0];
    if (!s_Small)
    {
        return 0;
    }
    for (i = 0; i < 3; i++)
    {
        s32 m0 = (s32)((rgb >> (i * 8)) & 0xFF) << 16;
        s64 t  = ((s64)(s32)c[C_RFC + i] << 12) - m0;
        s32 tv = (s32)(sf ? t >> 12 : t);
        s64 m;
        if (tv < -0x8000 || tv > 0x7FFF)
        {
            return 0;
        }
        m      = (s64)(tv * ir0) + m0;
        mac[i] = (s32)(sf ? m >> 12 : m);
        if (mac[i] < lo || mac[i] > 0x7FFF)
        {
            return 0;
        }
        col[i] = mac[i] >> 4;
        if (col[i] < 0 || col[i] > 0xFF)
        {
            return 0;
        }
    }
    d[D_MAC1] = d[D_IR1] = (u32)mac[0];
    d[D_MAC2] = d[D_IR2] = (u32)mac[1];
    d[D_MAC3] = d[D_IR3] = (u32)mac[2];
    d[D_RGB0] = d[D_RGB1];
    d[D_RGB1] = d[D_RGB2];
    d[D_RGB2] = (u32)col[0] | ((u32)col[1] << 8) | ((u32)col[2] << 16) | (d[D_RGBC] & 0xFF000000u);
    return 1;
}

/** MVMVA with matrix RT/LLM/LCM and translation TR/BK/none when nothing saturates (see rtp_fast).
 * Returns 0 without changing any register otherwise (and for the garbage matrix and the FC bug). */
static __attribute__((noinline)) int mvmva_fast(int sf, int mx, int v, int cv, int lm)
{
    const s32* m = mx == 0 ? m_rt : mx == 1 ? m_llm : m_lcm;
    s32 vx, vy, vz, mac[3], i, lo = lm ? 0 : -0x8000;
    if (!s_Small || mx == 3 || cv == 2)
    {
        return 0;
    }
    if (v == 3)
    {
        vx = (s16)d[D_IR1]; vy = (s16)d[D_IR2]; vz = (s16)d[D_IR3];
    }
    else
    {
        vx = lo16(d[D_VXY0 + v * 2]); vy = hi16(d[D_VXY0 + v * 2]); vz = lo16(d[D_VZ0 + v * 2]);
    }
    for (i = 0; i < 3; i++)
    {
        s64 t = cv == 0 ? (s64)(s32)c[C_TRX + i] << 12 : cv == 1 ? (s64)(s32)c[C_RBK + i] << 12 : 0;
        s64 r = t + (s64)(m[i * 3] * vx) + (s64)(m[i * 3 + 1] * vy) + (s64)(m[i * 3 + 2] * vz);
        mac[i] = (s32)(sf ? r >> 12 : r);
        if (mac[i] < lo || mac[i] > 0x7FFF)
        {
            return 0;
        }
    }
    d[D_MAC1] = d[D_IR1] = (u32)mac[0];
    d[D_MAC2] = d[D_IR2] = (u32)mac[1];
    d[D_MAC3] = d[D_IR3] = (u32)mac[2];
    return 1;
}

static __attribute__((noinline)) void nclip(void)
{
    s32 sx0 = lo16(d[D_SXY0]), sy0 = hi16(d[D_SXY0]);
    s32 sx1 = lo16(d[D_SXY1]), sy1 = hi16(d[D_SXY1]);
    s32 sx2 = lo16(d[D_SXY2]), sy2 = hi16(d[D_SXY2]);
    /* Products of 16-bit values fit 32 bits (one MULT each); the sum needs 64. */
    s64 v = (s64)(sx0 * sy1) + (s64)(sx1 * sy2) + (s64)(sx2 * sy0) - (s64)(sx0 * sy2) - (s64)(sx1 * sy0) -
            (s64)(sx2 * sy1);
    d[D_MAC0] = (u32)(s32)mac0_check(v);
}

static __attribute__((noinline)) void avsz(int four)
{
    s64 sum = (s64)d[D_SZ1] + d[D_SZ2] + d[D_SZ3];
    s64 v;
    if (four)
    {
        sum += d[D_SZ0];
    }
    v         = mac0_check((s64)(s16)c[four ? C_ZSF4 : C_ZSF3] * (s32)sum); /* sum <= 4 * FFFFh */
    d[D_MAC0] = (u32)(s32)v;
    d[D_OTZ]  = sat_sz(v >> 12);
}

static __attribute__((noinline)) void op(int sf, int lm)
{
    s64 d1 = lo16(c[C_RT0]), d2 = lo16(c[C_RT2]), d3 = lo16(c[C_RT4]);
    s64 i1 = (s16)d[D_IR1], i2 = (s16)d[D_IR2], i3 = (s16)d[D_IR3];
    s64 m1 = mac44(1, mac44(1, i3 * d2) - i2 * d3);
    s64 m2 = mac44(2, mac44(2, i1 * d3) - i3 * d1);
    s64 m3 = mac44(3, mac44(3, i2 * d1) - i1 * d2);
    set_mac123(m1, m2, m3, sf);
    ir_from_mac(lm);
}

static __attribute__((noinline)) void sqr(int sf, int lm)
{
    s64 i1 = (s16)d[D_IR1], i2 = (s16)d[D_IR2], i3 = (s16)d[D_IR3];
    set_mac123(mac44(1, i1 * i1), mac44(2, i2 * i2), mac44(3, i3 * i3), sf);
    ir_from_mac(lm);
}

static __attribute__((noinline)) void mvmva(int sf, int mx, int v, int cv, int lm)
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
static __attribute__((noinline)) void color_stage(int mode, int sf, int lm)
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
static __attribute__((noinline)) void light_color(int sf, int lm)
{
    s32 lcm[3][3], ir[3];
    s64 bk[3];
    get_matrix(C_LCM0, lcm);
    get_vector(3, ir);
    bk[0] = (s32)c[C_RBK]; bk[1] = (s32)c[C_GBK]; bk[2] = (s32)c[C_BBK];
    mul_mat_vec(lcm, ir, bk, sf, lm);
}

static __attribute__((noinline)) void normal_color(int v, int mode, int sf, int lm)
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
static __attribute__((noinline)) void depth_cue_from(s64 m[3], int sf, int lm)
{
    interp_fc(m, sf, lm);
    push_color();
}

static __attribute__((noinline)) void dpcs_rgb(u32 rgb, int sf, int lm)
{
    s64 m[3];
    m[0] = mac44(1, (s64)(rgb & 0xFF) << 16);
    m[1] = mac44(2, (s64)((rgb >> 8) & 0xFF) << 16);
    m[2] = mac44(3, (s64)((rgb >> 16) & 0xFF) << 16);
    depth_cue_from(m, sf, lm);
}

static __attribute__((noinline)) void gpf_gpl(int base, int sf, int lm)
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
    flag      = 0;
    s_FastMac = s_Small && (cmd & 0x3F) != 0x3D && (cmd & 0x3F) != 0x3E;

    switch (cmd & 0x3F)
    {
        case 0x01:                                                            /* RTPS */
            if (!rtp_fast(0, sf, lm, 1)) rtp(0, sf, lm, 1);
            break;
        case 0x30:                                                            /* RTPT */
            if (!rtp_fast(0, sf, lm, 0)) rtp(0, sf, lm, 0);
            if (!rtp_fast(1, sf, lm, 0)) rtp(1, sf, lm, 0);
            if (!rtp_fast(2, sf, lm, 1)) rtp(2, sf, lm, 1);
            break;
        case 0x06: nclip(); break;                                            /* NCLIP */
        case 0x0C: op(sf, lm); break;                                         /* OP */
        case 0x10:                                                            /* DPCS */
            if (!dpcs_fast(d[D_RGBC], sf, lm)) dpcs_rgb(d[D_RGBC], sf, lm);
            break;
        case 0x11:                                                            /* INTPL */
        {
            s64 m[3];
            m[0] = mac44(1, (s64)(s16)d[D_IR1] << 12);
            m[1] = mac44(2, (s64)(s16)d[D_IR2] << 12);
            m[2] = mac44(3, (s64)(s16)d[D_IR3] << 12);
            depth_cue_from(m, sf, lm);
            break;
        }
        case 0x12:                                                            /* MVMVA */
            if (!mvmva_fast(sf, mx, v, cv, lm)) mvmva(sf, mx, v, cv, lm);
            break;
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
