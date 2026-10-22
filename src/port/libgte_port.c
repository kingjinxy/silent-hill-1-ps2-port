/** @brief Sony's libgte in C for the port, function by function from the library objects (lib/libgte).
 *
 * Each function does what the original's instructions do, in the same order: the same GTE register
 * writes and reads (`mtc2`/`lwc2` = Gte_DataWrite, `ctc2` = Gte_CtrlWrite, ...) and the same commands
 * (Gte_Command with the original's COP2 immediate), so the GTE is left as on the PS1, and the same
 * memory accesses, including words that also cover padding. tools/port/lib_test compares them with the
 * originals. Functions not yet here are still recompiled (tools/port/recomp_all.sh).
 */

#include "common.h"
#include <psyq/libgte.h>
#include "port/gte.h"

extern s16 SQRT[]; /* libgte_tables.c */

#define CTC(reg, v) Gte_CtrlWrite((reg), (u32)(v))
#define CFC(reg)    Gte_CtrlRead(reg)
#define MTC(reg, v) Gte_DataWrite((reg), (u32)(v))
#define MFC(reg)    Gte_DataRead(reg)
#define W32(p, off) (*(u32*)((u8*)(p) + (off)))
#define LWC(reg, p, off) MTC((reg), W32((p), (off)))
#define SWC(reg, p, off) (W32((p), (off)) = MFC(reg))

/* Commands (COP2 immediates). */
#define RTPS      0x0180001 /* rtps */
#define RTPT      0x0280030 /* rtpt */
#define RTV0TR    0x0480012 /* mvmva sf=1 RT*V0+TR */
#define RTV0      0x0486012 /* mvmva sf=1 RT*V0 */
#define RTIR_SF0  0x041E012 /* mvmva sf=0 RT*IR */
#define RTIR      0x049E012 /* mvmva sf=1 RT*IR */

/* Registers. */
#define VXY0 0
#define VZ0  1
#define VXY1 2
#define VZ1  3
#define VXY2 4
#define VZ2  5
#define IR1  9
#define IR2  10
#define IR3  11
#define SXY0 12
#define SXY1 13
#define SXY2 14
#define SZ3  19
#define MAC1 25
#define MAC2 26
#define MAC3 27
#define LZCS 30
#define LZCR 31
#define C_RT    0  /* RT, 5 words */
#define C_TR    5  /* TRX, TRY, TRZ */
#define C_LLM   8
#define C_BK    13
#define C_LCM   16
#define C_FC    21
#define C_OFX   24
#define C_OFY   25
#define C_H     26
#define C_FLAG  31

/** msc00.o. On PS1 this also installs a kernel patch (`_patch_gte`) and enables COP2 in the status
 * register; neither applies to the software GTE. Default register values as set by the original. */
void InitGeom(void)
{
    CTC(29, 341);         /* ZSF3 */
    CTC(30, 256);         /* ZSF4 */
    CTC(C_H, 1000);
    CTC(27, -4194);       /* DQA */
    CTC(28, 0x1400000);   /* DQB */
    CTC(C_OFX, 0);
    CTC(C_OFY, 0);
}

/* --- Registers ----------------------------------------------------------------------------------*/

long ReadGeomScreen(void) /* reg09.o */
{
    return CFC(C_H);
}

void SetGeomScreen(long h) /* reg13.o */
{
    CTC(C_H, h);
}

void SetGeomOffset(long ofx, long ofy) /* reg12.o */
{
    CTC(C_OFX, (u32)ofx << 16);
    CTC(C_OFY, (u32)ofy << 16);
}

void ReadGeomOffset(long* ofx, long* ofy) /* reg08.o */
{
    *ofx = (s32)CFC(C_OFX) >> 16;
    *ofy = (s32)CFC(C_OFY) >> 16;
}

void SetBackColor(long rbk, long gbk, long bbk) /* reg10.o */
{
    CTC(C_BK + 0, (u32)rbk << 4);
    CTC(C_BK + 1, (u32)gbk << 4);
    CTC(C_BK + 2, (u32)bbk << 4);
}

void SetFarColor(long rfc, long gfc, long bfc) /* reg11.o */
{
    CTC(C_FC + 0, (u32)rfc << 4);
    CTC(C_FC + 1, (u32)gfc << 4);
    CTC(C_FC + 2, (u32)bfc << 4);
}

/** Five words of a matrix into control registers `base`.. (RT, LLM, LCM). */
static void set_matrix(int base, const MATRIX* m)
{
    int i;
    for (i = 0; i < 5; i++)
    {
        CTC(base + i, W32(m, i * 4));
    }
}

void SetRotMatrix(MATRIX* m) /* mtx_09.o */
{
    set_matrix(C_RT, m);
}

void SetLightMatrix(MATRIX* m) /* mtx_10.o */
{
    set_matrix(C_LLM, m);
}

void SetColorMatrix(MATRIX* m) /* mtx_11.o */
{
    set_matrix(C_LCM, m);
}

void SetTransMatrix(MATRIX* m) /* mtx_12.o */
{
    CTC(C_TR + 0, W32(m, 20));
    CTC(C_TR + 1, W32(m, 24));
    CTC(C_TR + 2, W32(m, 28));
}

/** The light matrix (LLM) and the back colour (BK, in the translation's place). */
void ReadLightMatrix(MATRIX* m) /* mtx_007.o */
{
    int i;
    for (i = 0; i < 5; i++)
    {
        W32(m, i * 4) = CFC(C_LLM + i);
    }
    for (i = 0; i < 3; i++)
    {
        W32(m, 20 + i * 4) = CFC(C_BK + i);
    }
}

/* PushMatrix/PopMatrix (mtx_006.o): a 20-deep stack of RT and TR in the object's .data. */
#define MATRIX_STACK_BYTES 640
static s32  s_MatrixSp;                       /* bytes in use */
static u32  s_MatrixStack[MATRIX_STACK_BYTES / 4];
extern int  printf(const char* fmt, ...);

void PushMatrix(void)
{
    u32* p;
    int  i;
    if (s_MatrixSp >= MATRIX_STACK_BYTES)
    {
        printf("Error: Can't push matrix,stack(max 20) is full!\n");
        return;
    }
    p = &s_MatrixStack[s_MatrixSp / 4];
    for (i = 0; i < 8; i++)
    {
        p[i] = CFC(C_RT + i); /* RT (5), TR (3) */
    }
    s_MatrixSp += 32;
}

void PopMatrix(void)
{
    u32* p;
    int  i;
    if (s_MatrixSp <= 0)
    {
        printf("Error: Can't pop matrix,stack is empty!\n");
        return;
    }
    s_MatrixSp -= 32;
    p = &s_MatrixStack[s_MatrixSp / 4];
    for (i = 0; i < 8; i++)
    {
        CTC(C_RT + i, p[i]);
    }
}

/* --- Vectors ------------------------------------------------------------------------------------*/

void RotTrans(SVECTOR* v0, VECTOR* v1, long* flag) /* smp_04.o */
{
    LWC(VXY0, v0, 0);
    LWC(VZ0, v0, 4);
    Gte_Command(RTV0TR);
    SWC(MAC1, v1, 0);
    SWC(MAC2, v1, 4);
    SWC(MAC3, v1, 8);
    *flag = CFC(C_FLAG);
}

VECTOR* ApplyRotMatrix(SVECTOR* v0, VECTOR* v1) /* mtx_005.o */
{
    MTC(VXY0, W32(v0, 0));
    MTC(VZ0, W32(v0, 4));
    Gte_Command(RTV0);
    SWC(IR1, v1, 0);
    SWC(IR2, v1, 4);
    SWC(IR3, v1, 8);
    return v1;
}

VECTOR* ApplyMatrix(MATRIX* m, SVECTOR* v0, VECTOR* v1) /* mtx_05.o */
{
    set_matrix(C_RT, m);
    LWC(VXY0, v0, 0);
    LWC(VZ0, v0, 4);
    Gte_Command(RTV0);
    SWC(MAC1, v1, 0);
    SWC(MAC2, v1, 4);
    SWC(MAC3, v1, 8);
    return v1;
}

SVECTOR* ApplyMatrixSV(MATRIX* m, SVECTOR* v0, SVECTOR* v1) /* mtx_06.o */
{
    u32 x, y, z;
    set_matrix(C_RT, m);
    LWC(VXY0, v0, 0);
    LWC(VZ0, v0, 4);
    Gte_Command(RTV0);
    x = MFC(IR1);
    y = MFC(IR2);
    z = MFC(IR3);
    v1->vx = x;
    v1->vy = y;
    v1->vz = z;
    return v1;
}

/** A 32-bit component split for two 16-bit passes: `hi` = bits 15 and up, `lo` = bits 0-14, both with
 * the sign of `v` (the originals negate, split and negate again for negative values). */
static void split15(s32 v, s32* hi, s32* lo)
{
    if (v >= 0)
    {
        *hi = v >> 15;
        *lo = v & 0x7FFF;
    }
    else
    {
        u32 n = -(u32)v;
        *hi = -(u32)((s32)n >> 15);
        *lo = -(u32)(n & 0x7FFF);
    }
}

/** RT times a 32-bit vector: the high parts (sf = 0) times 8, plus the low parts (sf = 1). */
static VECTOR* apply_lv(VECTOR* v0, VECTOR* v1)
{
    s32 hx, hy, hz, lx, ly, lz;
    u32 mx, my, mz;

    split15(v0->vx, &hx, &lx);
    split15(v0->vy, &hy, &ly);
    split15(v0->vz, &hz, &lz);
    MTC(IR1, hx);
    MTC(IR2, hy);
    MTC(IR3, hz);
    Gte_Command(RTIR_SF0);
    mx = MFC(MAC1);
    my = MFC(MAC2);
    mz = MFC(MAC3);
    MTC(IR1, lx);
    MTC(IR2, ly);
    MTC(IR3, lz);
    Gte_Command(RTIR);
    v1->vx = MFC(MAC1) + (mx << 3);
    v1->vy = MFC(MAC2) + (my << 3);
    v1->vz = MFC(MAC3) + (mz << 3);
    return v1;
}

VECTOR* ApplyRotMatrixLV(VECTOR* v0, VECTOR* v1) /* mtx_01.o */
{
    return apply_lv(v0, v1);
}

VECTOR* ApplyMatrixLV(MATRIX* m, VECTOR* v0, VECTOR* v1) /* mtx_004.o */
{
    set_matrix(C_RT, m);
    return apply_lv(v0, v1);
}

long RotTransPers(SVECTOR* v0, long* sxy, long* p, long* flag) /* smp_02.o */
{
    LWC(VXY0, v0, 0);
    LWC(VZ0, v0, 4);
    Gte_Command(RTPS);
    SWC(SXY2, sxy, 0);
    SWC(8, p, 0); /* IR0 */
    *flag = CFC(C_FLAG);
    return (s32)MFC(SZ3) >> 2;
}

long RotTransPers3(SVECTOR* v0, SVECTOR* v1, SVECTOR* v2, long* sxy0, long* sxy1, long* sxy2, long* p,
                   long* flag) /* smp_03.o */
{
    LWC(VXY0, v0, 0);
    LWC(VZ0, v0, 4);
    LWC(VXY1, v1, 0);
    LWC(VZ1, v1, 4);
    LWC(VXY2, v2, 0);
    LWC(VZ2, v2, 4);
    Gte_Command(RTPT);
    SWC(SXY0, sxy0, 0);
    SWC(SXY1, sxy1, 0);
    SWC(SXY2, sxy2, 0);
    SWC(8, p, 0);
    *flag = CFC(C_FLAG);
    return (s32)MFC(SZ3) >> 2;
}

/** The first three by RTPT, the fourth by RTPS; the flags of both. */
long RotTransPers4(SVECTOR* v0, SVECTOR* v1, SVECTOR* v2, SVECTOR* v3, long* sxy0, long* sxy1, long* sxy2,
                   long* sxy3, long* p, long* flag) /* cmb_00.o */
{
    u32 flag3;
    LWC(VXY0, v0, 0);
    LWC(VZ0, v0, 4);
    LWC(VXY1, v1, 0);
    LWC(VZ1, v1, 4);
    LWC(VXY2, v2, 0);
    LWC(VZ2, v2, 4);
    Gte_Command(RTPT);
    SWC(SXY0, sxy0, 0);
    SWC(SXY1, sxy1, 0);
    SWC(SXY2, sxy2, 0);
    flag3 = CFC(C_FLAG);
    LWC(VXY0, v3, 0);
    LWC(VZ0, v3, 4);
    Gte_Command(RTPS);
    SWC(SXY2, sxy3, 0);
    SWC(8, p, 0);
    *flag = CFC(C_FLAG) | flag3;
    return (s32)MFC(SZ3) >> 2;
}

/* --- Matrices -----------------------------------------------------------------------------------*/

MATRIX* TransMatrix(MATRIX* m, VECTOR* v) /* mtx_07.o */
{
    u32 x = W32(v, 0), y = W32(v, 4), z = W32(v, 8);
    W32(m, 20) = x;
    W32(m, 24) = y;
    W32(m, 28) = z;
    return m;
}

/** m1 = m0 transposed, through words as the original (m0 == m1 works). */
MATRIX* TransposeMatrix(MATRIX* m0, MATRIX* m1) /* fgo_00.o */
{
    u32 a = W32(m0, 0), b = W32(m0, 4), c, d;
    s16 e;
    W32(m1, 4)            = a;
    W32(m1, 0)            = b;
    *(u16*)((u8*)m1 + 0)  = a;
    c                     = W32(m0, 8);
    d                     = W32(m0, 12);
    W32(m1, 12)           = c;
    W32(m1, 8)            = d;
    *(u16*)((u8*)m1 + 12) = b;
    *(u16*)((u8*)m1 + 8)  = c;
    e                     = *(s16*)((u8*)m0 + 16);
    *(u16*)((u8*)m1 + 4)  = d;
    *(u16*)((u8*)m1 + 16) = e;
    return m1;
}

/** Each column of `m` scaled by v's component: (m[i][j] * v[j]) >> 12 (low 32 bits of the product).
 * The last element is stored as a word, as the original does (its sign also fills the padding). */
MATRIX* ScaleMatrix(MATRIX* m, VECTOR* v) /* mtx_08.o */
{
    static const u8 COL[9] = { 0, 1, 2, 0, 1, 2, 0, 1, 2 };
    u32             s[3];
    int             w;
    s[0] = v->vx;
    s[1] = v->vy;
    s[2] = v->vz;
    for (w = 0; w < 4; w++)
    {
        u32 x  = W32(m, w * 4);
        s32 lo = (s32)((u32)(s32)(s16)x * s[COL[w * 2]]) >> 12;
        s32 hi = (s32)((u32)((s32)x >> 16) * s[COL[w * 2 + 1]]) >> 12;
        W32(m, w * 4) = ((u32)lo & 0xFFFF) | ((u32)hi << 16);
    }
    W32(m, 16) = (s32)((u32)(s32)(s16)W32(m, 16) * s[2]) >> 12;
    return m;
}

/** RT times the columns of `m` (three MVMVA RT*V0), packed into `out` as the originals do. The last
 * element is IR3's whole word. Every read of `m` comes before the first store to `out`. */
static void mul_columns(const MATRIX* m, MATRIX* out)
{
    u32 c0[3], c1[3];

    MTC(VXY0, (*(u16*)((u8*)m + 0)) | (W32(m, 4) & 0xFFFF0000));
    MTC(VZ0, W32(m, 12));
    Gte_Command(RTV0);
    c0[0] = MFC(IR1);
    c0[1] = MFC(IR2);
    c0[2] = MFC(IR3);
    MTC(VXY0, (*(u16*)((u8*)m + 2)) | (W32(m, 8) << 16));
    MTC(VZ0, (s32) * (s16*)((u8*)m + 14));
    Gte_Command(RTV0);
    c1[0] = MFC(IR1);
    c1[1] = MFC(IR2);
    c1[2] = MFC(IR3);
    MTC(VXY0, (*(u16*)((u8*)m + 4)) | (W32(m, 8) & 0xFFFF0000));
    MTC(VZ0, W32(m, 16));
    Gte_Command(RTV0);

    W32(out, 0)  = (c0[0] & 0xFFFF) | (c1[0] << 16);
    W32(out, 12) = (c0[2] & 0xFFFF) | (c1[2] << 16);
    W32(out, 4)  = (MFC(IR1) & 0xFFFF) | (c0[1] << 16);
    W32(out, 8)  = (c1[1] & 0xFFFF) | (MFC(IR2) << 16);
    SWC(IR3, out, 16);
}

MATRIX* MulMatrix0(MATRIX* m0, MATRIX* m1, MATRIX* m2) /* mtx_000.o */
{
    set_matrix(C_RT, m0);
    mul_columns(m1, m2);
    return m2;
}

MATRIX* MulMatrix(MATRIX* m0, MATRIX* m1) /* mtx_03.o: m0 = m0 * m1 */
{
    set_matrix(C_RT, m0);
    mul_columns(m1, m0);
    return m0;
}

MATRIX* MulMatrix2(MATRIX* m0, MATRIX* m1) /* mtx_04.o: m1 = m0 * m1 */
{
    set_matrix(C_RT, m0);
    mul_columns(m1, m1);
    return m1;
}

MATRIX* MulRotMatrix(MATRIX* m0) /* mtx_001.o: m0 = RT * m0 */
{
    mul_columns(m0, m0);
    return m0;
}

/** RT = RT * m0, the product written to RT in the original's order. */
MATRIX* SetMulRotMatrix(MATRIX* m0) /* mtx_02.o */
{
    u32 c0[3], c1[3], c2[3];

    MTC(VXY0, (*(u16*)((u8*)m0 + 0)) | (W32(m0, 4) & 0xFFFF0000));
    MTC(VZ0, W32(m0, 12));
    Gte_Command(RTV0);
    c0[0] = MFC(IR1);
    c0[1] = MFC(IR2);
    c0[2] = MFC(IR3);
    MTC(VXY0, (*(u16*)((u8*)m0 + 2)) | (W32(m0, 8) << 16));
    MTC(VZ0, (s32) * (s16*)((u8*)m0 + 14));
    Gte_Command(RTV0);
    c1[0] = MFC(IR1);
    c1[1] = MFC(IR2);
    c1[2] = MFC(IR3);
    MTC(VXY0, (*(u16*)((u8*)m0 + 4)) | (W32(m0, 8) & 0xFFFF0000));
    MTC(VZ0, W32(m0, 16));
    Gte_Command(RTV0);
    c2[0] = MFC(IR1);
    c2[1] = MFC(IR2);
    c2[2] = MFC(IR3);

    CTC(0, (c0[0] & 0xFFFF) | (c1[0] << 16));
    CTC(3, (c0[2] & 0xFFFF) | (c1[2] << 16));
    CTC(1, (c2[0] & 0xFFFF) | (c0[1] << 16));
    CTC(2, (c1[1] & 0xFFFF) | (c2[1] << 16));
    CTC(4, c2[2]);
    return m0;
}

/** m2 = m0 * m1, with translation m0 * m1.t + m0.t. */
MATRIX* CompMatrix(MATRIX* m0, MATRIX* m1, MATRIX* m2) /* mtx_003.o */
{
    u32 c0[3], c1[3], ir1, ir2;

    set_matrix(C_RT, m0);
    MTC(VXY0, (*(u16*)((u8*)m1 + 0)) | (W32(m1, 4) & 0xFFFF0000));
    MTC(VZ0, W32(m1, 12));
    Gte_Command(RTV0);
    c0[0] = MFC(IR1);
    c0[1] = MFC(IR2);
    c0[2] = MFC(IR3);
    MTC(VXY0, (*(u16*)((u8*)m1 + 2)) | (W32(m1, 8) << 16));
    MTC(VZ0, (s32) * (s16*)((u8*)m1 + 14));
    Gte_Command(RTV0);
    c1[0] = MFC(IR1);
    c1[1] = MFC(IR2);
    c1[2] = MFC(IR3);
    MTC(VXY0, (*(u16*)((u8*)m1 + 4)) | (W32(m1, 8) & 0xFFFF0000));
    MTC(VZ0, W32(m1, 16));
    Gte_Command(RTV0);

    W32(m2, 0)  = (c0[0] & 0xFFFF) | (c1[0] << 16);
    W32(m2, 12) = (c0[2] & 0xFFFF) | (c1[2] << 16);
    ir1         = MFC(IR1);
    ir2         = MFC(IR2);
    SWC(IR3, m2, 16);

    MTC(VXY0, (*(u16*)((u8*)m1 + 20)) | (W32(m1, 24) << 16));
    MTC(VZ0, W32(m1, 28));
    Gte_Command(RTV0);
    W32(m2, 4) = (ir1 & 0xFFFF) | (c0[1] << 16);
    W32(m2, 8) = (c1[1] & 0xFFFF) | (ir2 << 16);
    {
        u32 x = MFC(MAC1), y = MFC(MAC2), z = MFC(MAC3);
        u32 tx = W32(m0, 20), ty = W32(m0, 24), tz = W32(m0, 28);
        W32(m2, 20) = x + tx;
        W32(m2, 24) = y + ty;
        W32(m2, 28) = z + tz;
    }
    return m2;
}

/* --- Square roots -------------------------------------------------------------------------------*/

/** The originals' shared start: the GTE's leading-bit count (LZCS/LZCR), the value normalised to 7 bits
 * by a shift (srav right, sllv left; low 5 bits of the amount), and the table entry. For a == 0 (a
 * count of 32), *lz = -1. */
static s32 sqrt_mantissa(long a, s32* lz)
{
    s32 n, t;
    MTC(LZCS, a);
    n = MFC(LZCR);
    if (n == 32)
    {
        *lz = -1;
        return 0;
    }
    n &= ~1;
    t = n - 24;
    if (t >= 0)
    {
        t = (u32)a << (t & 31);
    }
    else
    {
        t = (s32)a >> ((24 - n) & 31);
    }
    *lz = n;
    return SQRT[t - 64];
}

long SquareRoot0(long a) /* msc01.o */
{
    s32 n, m = sqrt_mantissa(a, &n);
    if (n < 0)
    {
        return 0;
    }
    return ((u32)m << (((31 - n) >> 1) & 31)) >> 12;
}

long SquareRoot12(long a) /* msc09.o */
{
    s32 n, m = sqrt_mantissa(a, &n), sh;
    if (n < 0)
    {
        return 0;
    }
    sh = (19 - n) >> 1;
    if (sh >= 0)
    {
        return (u32)m << (sh & 31);
    }
    return (u32)m >> (-sh & 31);
}

/* --- Colours and depth cueing (smp.o, smp_00.o) -------------------------------------------------*/

#define RGBC 6
#define IR0  8
#define OTZ  7
#define RGB0 20
#define RGB1 21
#define RGB2 22

void LightColor(VECTOR* v0, VECTOR* v1) /* smp_00.o: LCM * IR */
{
    LWC(IR1, v0, 0);
    LWC(IR2, v0, 4);
    LWC(IR3, v0, 8);
    Gte_Command(0x04DA412);
    SWC(IR1, v1, 0);
    SWC(IR2, v1, 4);
    SWC(IR3, v1, 8);
}

void DpqColorLight(VECTOR* v0, CVECTOR* v1, long p, CVECTOR* v2) /* smp_00.o: DCPL */
{
    LWC(IR1, v0, 0);
    LWC(IR2, v0, 4);
    LWC(IR3, v0, 8);
    LWC(RGBC, v1, 0);
    MTC(IR0, p);
    Gte_Command(0x0680029);
    SWC(RGB2, v2, 0);
}

void DpqColor3(CVECTOR* v0, CVECTOR* v1, CVECTOR* v2, long p, CVECTOR* v3, CVECTOR* v4, CVECTOR* v5) /* smp_00.o: DPCT */
{
    LWC(RGB0, v0, 0);
    LWC(RGB1, v1, 0);
    LWC(RGB2, v2, 0);
    LWC(RGBC, v2, 0);
    MTC(IR0, p);
    Gte_Command(0x0F8002A);
    SWC(RGB0, v3, 0);
    SWC(RGB1, v4, 0);
    SWC(RGB2, v5, 0);
}

void Intpl(VECTOR* v0, long p, CVECTOR* v1) /* smp_00.o: INTPL */
{
    LWC(IR1, v0, 0);
    LWC(IR2, v0, 4);
    LWC(IR3, v0, 8);
    MTC(IR0, p);
    Gte_Command(0x0980011);
    SWC(RGB2, v1, 0);
}

VECTOR* Square12(VECTOR* v0, VECTOR* v1) /* smp_00.o: SQR sf=1 */
{
    LWC(IR1, v0, 0);
    LWC(IR2, v0, 4);
    LWC(IR3, v0, 8);
    Gte_Command(0x0A80428);
    SWC(MAC1, v1, 0);
    SWC(MAC2, v1, 4);
    SWC(MAC3, v1, 8);
    return v1;
}

VECTOR* Square0(VECTOR* v0, VECTOR* v1) /* smp_00.o: SQR sf=0 */
{
    LWC(IR1, v0, 0);
    LWC(IR2, v0, 4);
    LWC(IR3, v0, 8);
    Gte_Command(0x0A00428);
    SWC(MAC1, v1, 0);
    SWC(MAC2, v1, 4);
    SWC(MAC3, v1, 8);
    return v1;
}

long AverageZ3(long sz0, long sz1, long sz2) /* smp_00.o: AVSZ3 */
{
    MTC(17, sz0);
    MTC(18, sz1);
    MTC(19, sz2);
    Gte_Command(0x158002D);
    return MFC(OTZ);
}

long AverageZ4(long sz0, long sz1, long sz2, long sz3) /* smp_00.o: AVSZ4 */
{
    MTC(16, sz0);
    MTC(17, sz1);
    MTC(18, sz2);
    MTC(19, sz3);
    Gte_Command(0x168002E);
    return MFC(OTZ);
}

/** OP with v0 on RT's diagonal; RT's diagonal words are put back afterwards. */
static void outer_product(VECTOR* v0, VECTOR* v1, VECTOR* v2, u32 cmd)
{
    u32 r0 = CFC(0), r2 = CFC(2), r4 = CFC(4);
    CTC(0, W32(v0, 0));
    CTC(2, W32(v0, 4));
    CTC(4, W32(v0, 8));
    LWC(IR3, v1, 8);
    LWC(IR1, v1, 0);
    LWC(IR2, v1, 4);
    Gte_Command(cmd);
    SWC(MAC1, v2, 0);
    SWC(MAC2, v2, 4);
    SWC(MAC3, v2, 8);
    CTC(0, r0);
    CTC(2, r2);
    CTC(4, r4);
}

void OuterProduct12(VECTOR* v0, VECTOR* v1, VECTOR* v2) /* smp_00.o */
{
    outer_product(v0, v1, v2, 0x178000C);
}

void OuterProduct0(VECTOR* v0, VECTOR* v1, VECTOR* v2) /* smp_00.o */
{
    outer_product(v0, v1, v2, 0x170000C);
}

long Lzc(long data) /* smp_00.o */
{
    MTC(LZCS, data);
    return MFC(LZCR);
}

void LocalLight(SVECTOR* v0, VECTOR* v1) /* smp.o: LLM * V0 */
{
    LWC(VXY0, v0, 0);
    LWC(VZ0, v0, 4);
    Gte_Command(0x04A6412);
    SWC(IR1, v1, 0);
    SWC(IR2, v1, 4);
    SWC(IR3, v1, 8);
}

void DpqColor(CVECTOR* v0, long p, CVECTOR* v1) /* smp.o: DPCS */
{
    LWC(RGBC, v0, 0);
    MTC(IR0, p);
    Gte_Command(0x0780010);
    SWC(RGB2, v1, 0);
}

void NormalColor(SVECTOR* v0, CVECTOR* v1) /* smp.o: NCS */
{
    LWC(VXY0, v0, 0);
    LWC(VZ0, v0, 4);
    Gte_Command(0x0C8041E);
    SWC(RGB2, v1, 0);
}

void NormalColor3(SVECTOR* v0, SVECTOR* v1, SVECTOR* v2, CVECTOR* v3, CVECTOR* v4, CVECTOR* v5) /* smp.o: NCT */
{
    LWC(VXY0, v0, 0);
    LWC(VZ0, v0, 4);
    LWC(VXY1, v1, 0);
    LWC(VZ1, v1, 4);
    LWC(VXY2, v2, 0);
    LWC(VZ2, v2, 4);
    Gte_Command(0x0D80420);
    SWC(RGB0, v3, 0);
    SWC(RGB1, v4, 0);
    SWC(RGB2, v5, 0);
}

void NormalColorDpq(SVECTOR* v0, CVECTOR* v1, long p, CVECTOR* v2) /* smp.o: NCDS */
{
    LWC(VXY0, v0, 0);
    LWC(VZ0, v0, 4);
    LWC(RGBC, v1, 0);
    MTC(IR0, p);
    Gte_Command(0x0E80413);
    SWC(RGB2, v2, 0);
}

void NormalColorDpq3(SVECTOR* v0, SVECTOR* v1, SVECTOR* v2, CVECTOR* v3, long p, CVECTOR* v4, CVECTOR* v5,
                     CVECTOR* v6) /* smp.o: NCDT */
{
    LWC(VXY0, v0, 0);
    LWC(VZ0, v0, 4);
    LWC(VXY1, v1, 0);
    LWC(VZ1, v1, 4);
    LWC(VXY2, v2, 0);
    LWC(VZ2, v2, 4);
    LWC(RGBC, v3, 0);
    MTC(IR0, p);
    Gte_Command(0x0F80416);
    SWC(RGB0, v4, 0);
    SWC(RGB1, v5, 0);
    SWC(RGB2, v6, 0);
}

void NormalColorCol(SVECTOR* v0, CVECTOR* v1, CVECTOR* v2) /* smp.o: NCCS */
{
    LWC(VXY0, v0, 0);
    LWC(VZ0, v0, 4);
    LWC(RGBC, v1, 0);
    Gte_Command(0x108041B);
    SWC(RGB2, v2, 0);
}

void NormalColorCol3(SVECTOR* v0, SVECTOR* v1, SVECTOR* v2, CVECTOR* v3, CVECTOR* v4, CVECTOR* v5,
                     CVECTOR* v6) /* smp.o: NCCT */
{
    LWC(VXY0, v0, 0);
    LWC(VZ0, v0, 4);
    LWC(VXY1, v1, 0);
    LWC(VZ1, v1, 4);
    LWC(VXY2, v2, 0);
    LWC(VZ2, v2, 4);
    LWC(RGBC, v3, 0);
    Gte_Command(0x118043F);
    SWC(RGB0, v4, 0);
    SWC(RGB1, v5, 0);
    SWC(RGB2, v6, 0);
}

void ColorDpq(VECTOR* v0, CVECTOR* v1, long p, CVECTOR* v2) /* smp.o: CDP */
{
    LWC(IR1, v0, 0);
    LWC(IR2, v0, 4);
    LWC(IR3, v0, 8);
    LWC(RGBC, v1, 0);
    MTC(IR0, p);
    Gte_Command(0x1280414);
    SWC(RGB2, v2, 0);
}

void ColorCol(VECTOR* v0, CVECTOR* v1, CVECTOR* v2) /* smp.o: CC */
{
    LWC(IR1, v0, 0);
    LWC(IR2, v0, 4);
    LWC(IR3, v0, 8);
    LWC(RGBC, v1, 0);
    Gte_Command(0x138041C);
    SWC(RGB2, v2, 0);
}

long AverageSZ3(void) /* smp.o: AVSZ3 on SZ1-SZ3 as they are */
{
    Gte_Command(0x158002D);
    return MFC(OTZ);
}

long AverageSZ4(void) /* smp.o */
{
    Gte_Command(0x168002E);
    return MFC(OTZ);
}

/* --- Interpolation (msc06.o) --------------------------------------------------------------------*/

#define GPF_SF1 0x198003D
#define GPF_SF0 0x190003D
#define GPL_SF1 0x1A8003E
#define GPL_SF0 0x1A0003E

/** v2 = p0 * v0 + p1 * v1 (GPF then GPL). The originals read LZCR in between (into $v0; the functions
 * are void). */
static long load_average(VECTOR* v0, VECTOR* v1, long p0, long p1, VECTOR* v2, u32 gpf, u32 gpl)
{
    long ret;
    MTC(IR0, p0);
    LWC(IR1, v0, 0);
    LWC(IR2, v0, 4);
    LWC(IR3, v0, 8);
    Gte_Command(gpf);
    ret = MFC(LZCR);
    MTC(IR0, p1);
    LWC(IR1, v1, 0);
    LWC(IR2, v1, 4);
    LWC(IR3, v1, 8);
    Gte_Command(gpl);
    SWC(IR1, v2, 0);
    SWC(IR2, v2, 4);
    SWC(IR3, v2, 8);
    return ret;
}

void LoadAverage12(VECTOR* v0, VECTOR* v1, long p0, long p1, VECTOR* v2)
{
    load_average(v0, v1, p0, p1, v2, GPF_SF1, GPL_SF1);
}

void LoadAverage0(VECTOR* v0, VECTOR* v1, long p0, long p1, VECTOR* v2)
{
    load_average(v0, v1, p0, p1, v2, GPF_SF0, GPL_SF0);
}

/** SVECTOR halves into IR1-3: x zero-extended, y by an arithmetic shift, z zero-extended. */
static void ir_from_svector(const SVECTOR* v)
{
    u32 a = W32(v, 0), b = W32(v, 4);
    MTC(IR1, a & 0xFFFF);
    MTC(IR2, (s32)a >> 16);
    MTC(IR3, b & 0xFFFF);
}

static long load_average_short(SVECTOR* v0, SVECTOR* v1, long p0, long p1, SVECTOR* v2, u32 gpf, u32 gpl)
{
    long ret;
    u32  x;
    MTC(IR0, p0);
    ir_from_svector(v0);
    Gte_Command(gpf);
    ret = MFC(LZCR);
    MTC(IR0, p1);
    ir_from_svector(v1);
    Gte_Command(gpl);
    x            = (MFC(IR1) & 0xFFFF) | (MFC(IR2) << 16);
    W32(v2, 0)   = x;
    W32(v2, 4)   = MFC(IR3);
    return ret;
}

void LoadAverageShort12(SVECTOR* v0, SVECTOR* v1, long p0, long p1, SVECTOR* v2)
{
    load_average_short(v0, v1, p0, p1, v2, GPF_SF1, GPL_SF1);
}

void LoadAverageShort0(SVECTOR* v0, SVECTOR* v1, long p0, long p1, SVECTOR* v2)
{
    load_average_short(v0, v1, p0, p1, v2, GPF_SF0, GPL_SF0);
}

/** Bytes: sf = 0, then the MACs >> 12. `n` bytes (2 or 3). */
static long load_average_bytes(u8* v0, u8* v1, long p0, long p1, u8* v2, int n)
{
    long ret;
    int  i;
    MTC(IR0, p0);
    for (i = 0; i < n; i++)
    {
        MTC(IR1 + i, v0[i]);
    }
    Gte_Command(GPF_SF0);
    ret = MFC(LZCR);
    MTC(IR0, p1);
    for (i = 0; i < n; i++)
    {
        MTC(IR1 + i, v1[i]);
    }
    Gte_Command(GPL_SF0);
    for (i = 0; i < n; i++)
    {
        v2[i] = (s32)MFC(MAC1 + i) >> 12;
    }
    return ret;
}

void LoadAverageByte(u_char* v0, u_char* v1, long p0, long p1, u_char* v2)
{
    load_average_bytes(v0, v1, p0, p1, v2, 2);
}

void LoadAverageCol(u_char* v0, u_char* v1, long p0, long p1, u_char* v2)
{
    load_average_bytes(v0, v1, p0, p1, v2, 3);
}

/* --- Normalisation (msc02.o) ------------------------------------------------------------------*/

extern const s16 g_LibgteRsqrtTable[]; /* libgte_tables.c */

/** The originals' leading-bit normalisation: LZCR of `v` (even), and `v` shifted to 7 significant bits
 * (srav/sllv: low 5 bits of the amount). */
static s32 normalise7(s32 v, s32* lz)
{
    s32 n, t;
    MTC(LZCS, v);
    n   = MFC(LZCR) & ~1;
    *lz = n;
    t   = n - 24;
    return t >= 0 ? (s32)((u32)v << (t & 31)) : v >> ((24 - n) & 31);
}

/** The shared normaliser: |v|^2 by SQR (sf = 0), its reciprocal square root from the table into IR0,
 * then GPF (sf = 0) and a shift. Returns |v|^2. For a zero vector the PS1 reads the table at -64, in
 * whatever the PS1 linker put before it; here entry 0 (the products are 0 either way, only IR0
 * differs). */
static s32 normalise(s32* x, s32* y, s32* z)
{
    s32 sum, n, sh, idx;
    MTC(IR1, *x);
    MTC(IR2, *y);
    MTC(IR3, *z);
    Gte_Command(0x0A00428);
    sum = MFC(MAC1) + MFC(MAC2) + MFC(MAC3);
    idx = normalise7(sum, &n) - 64;
    sh  = (31 - n) >> 1;
    MTC(IR0, g_LibgteRsqrtTable[idx >= 0 ? idx : 0]);
    MTC(IR1, *x);
    MTC(IR2, *y);
    MTC(IR3, *z);
    Gte_Command(GPF_SF0);
    *x = (s32)MFC(MAC1) >> (sh & 31);
    *y = (s32)MFC(MAC2) >> (sh & 31);
    *z = (s32)MFC(MAC3) >> (sh & 31);
    return sum;
}

void InvSquareRoot(long a, long* b, long* c)
{
    s32 n, m;
    MTC(LZCS, a);
    n = MFC(LZCR);
    if (n == 32 || n == 0)
    {
        return; /* the original returns -1 */
    }
    m  = normalise7(a, &n);
    *c = (31 - n) >> 1;
    *b = g_LibgteRsqrtTable[m - 64];
}

long VectorNormal(VECTOR* v0, VECTOR* v1)
{
    s32 x = W32(v0, 0), y = W32(v0, 4), z = W32(v0, 8), sum = normalise(&x, &y, &z);
    W32(v1, 0) = x;
    W32(v1, 4) = y;
    W32(v1, 8) = z;
    return sum;
}

long VectorNormalS(VECTOR* v0, SVECTOR* v1)
{
    s32 x = W32(v0, 0), y = W32(v0, 4), z = W32(v0, 8), sum = normalise(&x, &y, &z);
    v1->vx = x;
    v1->vy = y;
    v1->vz = z;
    return sum;
}

long VectorNormalSS(SVECTOR* v0, SVECTOR* v1)
{
    s32 x = v0->vx, y = v0->vy, z = v0->vz, sum = normalise(&x, &y, &z);
    v1->vx = x;
    v1->vy = y;
    v1->vz = z;
    return sum;
}

/** Rows 0 and 1 kept in direction; row 2 = row 0 x row 1, row 0 = row 1 x row 2 (by OP with RT's
 * diagonal), then each normalised. RT's diagonal words are put back before the normalising. */
void MatrixNormal(MATRIX* m, MATRIX* n)
{
    s32 a0 = m->m[0][0], a1 = m->m[0][1], a2 = m->m[0][2];
    s32 b0 = m->m[1][0], b1 = m->m[1][1], b2 = m->m[1][2];
    u32 r0 = CFC(0), r2 = CFC(2), r4 = CFC(4);
    s32 c0, c1, c2, x, y, z;

    CTC(0, a0);
    CTC(2, a1);
    CTC(4, a2);
    MTC(IR3, b2);
    MTC(IR1, b0);
    MTC(IR2, b1);
    Gte_Command(0x178000C);
    c0 = MFC(MAC1);
    c1 = MFC(MAC2);
    c2 = MFC(MAC3);
    CTC(0, b0);
    CTC(2, b1);
    CTC(4, b2);
    Gte_Command(0x178000C);
    MTC(VXY0, b0); /* kept in the GTE for the second row, as the original */
    MTC(VZ0, b1);
    MTC(VXY1, b2);
    x = MFC(MAC1);
    y = MFC(MAC2);
    z = MFC(MAC3);
    CTC(0, r0);
    CTC(2, r2);
    CTC(4, r4);

    normalise(&x, &y, &z);
    n->m[0][0] = x;
    n->m[0][1] = y;
    n->m[0][2] = z;
    x = MFC(VXY0);
    y = MFC(VZ0);
    z = MFC(VXY1);
    normalise(&x, &y, &z);
    n->m[1][0] = x;
    n->m[1][1] = y;
    n->m[1][2] = z;
    x = c0;
    y = c1;
    z = c2;
    normalise(&x, &y, &z);
    n->m[2][0] = x;
    n->m[2][1] = y;
    n->m[2][2] = z;
}

/* --- ratan2 (ratan.o) ---------------------------------------------------------------------------*/

extern const s16 ratan_tbl[]; /* libgte_tables.c */

long ratan2(long y, long x)
{
    s32 negx = 0, negy = 0, a;
    if (x < 0)
    {
        negx = 1;
        x    = -(u32)x;
    }
    if (y < 0)
    {
        negy = 1;
        y    = -(u32)y;
    }
    if (x == 0 && y == 0)
    {
        return 0;
    }
    if (y < x)
    {
        a = ratan_tbl[(y & 0x7FE00000) ? y / (x >> 10) : (s32)((u32)y << 10) / x];
    }
    else
    {
        a = 1024 - ratan_tbl[(x & 0x7FE00000) ? x / (y >> 10) : (s32)((u32)x << 10) / y];
    }
    if (negx)
    {
        a = 2048 - a;
    }
    if (negy)
    {
        a = -a;
    }
    return a;
}
