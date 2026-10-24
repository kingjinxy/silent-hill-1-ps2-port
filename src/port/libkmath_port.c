/** @brief Konami's libkmath in C for the port: the hand-written PS1 assembly in
 * src/bodyprog/libkmath/libkmath.s, which the matching build keeps.
 *
 * Exact: every product is the asm's `mult`, then `(lo >> 12) | (hi << 20)`, i.e. bits 12-43 of the
 * 64-bit product (FIX12). The two `...Gte` builders run the same GTE commands (GPL, GPF) on the same
 * registers, so the GTE is left as on the PS1. tools/port/lib_test compares all of them with the PS1
 * originals. The matrix builders return void as declared in bodyprog/math/math.h (the asm leaves
 * the matrix in $v0; no caller reads it).
 */

#include "common.h"
#include <psyq/libgte.h>
#include "port/gte.h"

extern const s16 g_SineTable[5120]; /* libkmath_table.c */

#define SIN(a) ((s32)g_SineTable[(a) & 0xFFF])
#define COS(a) ((s32)g_SineTable[((a) & 0xFFF) + 0x400])

/** Bits 12-43 of the 64-bit product. */
#define FIX12(a, b) ((s32)(((s64)(s32)(a) * (s32)(b)) >> 12))

/** Halves rounding toward zero (the asm's `srl 31; addu; sra 1`). */
#define HALF(x) (((s32)(x) + (s32)((u32)(x) >> 31)) >> 1)

#define GTE_IR0  8
#define GTE_IR1  9
#define GTE_IR2  10
#define GTE_IR3  11
#define GTE_MAC1 25
#define GTE_MAC2 26
#define GTE_MAC3 27
#define GTE_GPL_SF1 0x01A8003E /* gpl 1 */
#define GTE_GPF_SF1 0x0198003D /* gpf 1 */

#ifdef SH_PORT_SENTINEL
extern void Port_Sentinel(const char* where, unsigned int ra); /* gte.c */
#define SENTINEL(where) Port_Sentinel(where, (unsigned int)__builtin_return_address(0))
#else
#define SENTINEL(where)
#endif

q19_12 Math_Sin(q19_12 angle) // 0x8009687C
{
    SENTINEL("Math_Sin");
    return SIN(angle);
}

q19_12 Math_Cos(q19_12 angle) // 0x8009689C
{
    SENTINEL("Math_Cos");
    return COS(angle);
}

/** `M = Rz(angle) * M`: rows 0 and 1 change, row 2 doesn't. */
MATRIX* Math_RotMatrixZ(s32 angle, MATRIX* mat) // 0x800968BC
{
    s16* a = mat->m[0];
    s16* b = mat->m[1];
    s32  c = COS(angle);
    s32  s = SIN(angle);
    s32  a0 = a[0], a1 = a[1], a2 = a[2];
    s32  b0 = b[0], b1 = b[1], b2 = b[2];

    a[0] = FIX12(c, a0) - FIX12(s, b0);
    a[1] = FIX12(c, a1) - FIX12(s, b1);
    a[2] = FIX12(c, a2) - FIX12(s, b2);
    b[0] = FIX12(c, b0) + FIX12(s, a0);
    b[1] = FIX12(c, b1) + FIX12(s, a1);
    b[2] = FIX12(c, b2) + FIX12(s, a2);
    return mat;
}

/** The asm's Math_RotMatrix0 and Math_RotMatrix1: the nine terms of an Euler rotation from angles
 * `x`, `y`, `z` (the callers choose which rotation is which), as products of sums and differences. */
typedef struct
{
    s32 t[5]; /* Math_RotMatrix0: $t0-$t4 */
    s32 u[4]; /* Math_RotMatrix1: $t0-$t3 */
} RotTerms;

static void rot_terms(s32 x, s32 y, s32 z, RotTerms* r)
{
    s32 sy = SIN(y);
    s32 p, q, d, e;

    p = FIX12(SIN(x + z), sy + 0x1000);
    q = FIX12(COS(x + z), sy + 0x1000);
    d = FIX12(-COS(x - z), sy - 0x1000);
    e = FIX12(SIN(x - z), sy - 0x1000);

    r->t[0] = sy;
    r->t[1] = HALF(p + e);
    r->t[2] = HALF(d + q);
    r->t[3] = HALF(d - q);
    r->t[4] = HALF(p - e);

    r->u[0] = FIX12(COS(z), COS(y));
    r->u[1] = FIX12(-SIN(z), COS(y));
    r->u[2] = FIX12(-SIN(x), COS(y));
    r->u[3] = FIX12(COS(x), COS(y));
}

/** Stores terms at byte offsets into the matrix, as the asm's `sh` lists. */
static void store_terms(MATRIX* mat, const s32* v, const u8* offs, int n)
{
    int i;
    for (i = 0; i < n; i++)
    {
        *(s16*)((u8*)mat->m + offs[i]) = v[i];
    }
}

/** `M = Rx * Ry * Rz`. */
void Math_RotMatrixXyz(SVECTOR* rot, MATRIX* mat) // 0x80096A5C
{
    static const u8 OFFS0[5] = { 0x4, 0x6, 0x8, 0xC, 0xE };
    static const u8 OFFS1[4] = { 0x0, 0x2, 0xA, 0x10 };
    RotTerms        r;

    rot_terms(rot->vx, rot->vy, rot->vz, &r);
    store_terms(mat, r.t, OFFS0, 5);
    store_terms(mat, r.u, OFFS1, 4);
}

void Math_RotMatrixZxy(SVECTOR* rot, MATRIX* mat) // 0x80096C4C
{
    static const u8 OFFS0[5] = { 0xE, 0x4, 0x0, 0xA, 0x6 };
    static const u8 OFFS1[4] = { 0x10, 0xC, 0x2, 0x8 };
    RotTerms        r;

    rot_terms(rot->vz, rot->vx, rot->vy, &r);
    store_terms(mat, r.t, OFFS0, 5);
    store_terms(mat, r.u, OFFS1, 4);
}

void Math_RotMatrixZxyNeg(const SVECTOR* rot, MATRIX* mat) // 0x80096C94
{
    static const u8 OFFS0[5] = { 0xA, 0xC, 0x0, 0xE, 0x2 };
    static const u8 OFFS1[4] = { 0x10, 0x4, 0x6, 0x8 };
    RotTerms        r;

    rot_terms(-rot->vz, -rot->vx, -rot->vy, &r);
    store_terms(mat, r.t, OFFS0, 5);
    store_terms(mat, r.u, OFFS1, 4);
}

/** The asm's Math_RotMatrixGte: the same nine terms, five of them through the GTE (GPL for the sums
 * and differences, GPF for the cos(y) products). Results in the asm's $t0-$t8 order. */
static void rot_terms_gte(s32 x, s32 y, s32 z, s32 t[9])
{
    s32 sy = SIN(y), cy = COS(y);
    s32 p, ir1, ir2, ir3;

    p = FIX12(SIN(x + z), sy + 0x1000);
    Gte_DataWrite(GTE_IR0, sy);
    Gte_DataWrite(GTE_IR1, SIN(x - z));
    Gte_DataWrite(GTE_MAC2, COS(x - z));
    Gte_DataWrite(GTE_IR3, COS(x + z));
    Gte_DataWrite(GTE_MAC1, -SIN(x - z));
    Gte_DataWrite(GTE_IR2, -COS(x - z));
    Gte_DataWrite(GTE_MAC3, COS(x + z));
    Gte_Command(GTE_GPL_SF1);

    ir1 = Gte_DataRead(GTE_IR1);
    ir2 = Gte_DataRead(GTE_IR2);
    ir3 = Gte_DataRead(GTE_IR3);
    Gte_DataWrite(GTE_IR0, cy);
    t[0] = HALF(p + ir1);
    t[3] = HALF(p - ir1);
    t[1] = HALF(ir2 + ir3);
    t[2] = HALF(ir2 - ir3);
    t[4] = FIX12(cy, COS(z));

    Gte_DataWrite(GTE_IR1, -SIN(z));
    Gte_DataWrite(GTE_IR2, -SIN(x));
    Gte_DataWrite(GTE_IR3, COS(x));
    Gte_Command(GTE_GPF_SF1);
    t[5] = Gte_DataRead(GTE_IR1);
    t[6] = Gte_DataRead(GTE_IR2);
    t[7] = Gte_DataRead(GTE_IR3);
    t[8] = sy;
}

void Math_RotMatrixXyzGte(SVECTOR* rot, MATRIX* mat) // 0x80096CE8
{
    static const u8 OFFS[9] = { 0x6, 0x8, 0xC, 0xE, 0x0, 0x2, 0xA, 0x10, 0x4 };
    s32             t[9];

    rot_terms_gte(rot->vx, rot->vy, rot->vz, t);
    store_terms(mat, t, OFFS, 9);
}

void Math_RotMatrixZxyNegGte(const SVECTOR* rot, MATRIX* mat) // 0x80096E78
{
    static const u8 OFFS[9] = { 0xC, 0x0, 0xE, 0x2, 0x10, 0x4, 0x6, 0x8, 0xA };
    s32             t[9];

    rot_terms_gte(-rot->vz, -rot->vx, -rot->vy, t);
    store_terms(mat, t, OFFS, 9);
}
