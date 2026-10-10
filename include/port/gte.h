#ifndef _PORT_GTE_H
#define _PORT_GTE_H

/** @brief Software implementation of the PS1 Geometry Transformation Engine (COP2).
 *
 * Bit-exact model of the hardware (fixed-point maths, 44-bit MAC overflow checks, saturation and
 * FLAG bits, the UNR division used by RTPS/RTPT), written from the psx-spx documentation
 * ("Geometry Transformation Engine (GTE)"). The interface mirrors the CPU's view of COP2:
 * register moves (MTC2/MFC2/CTC2/CFC2, LWC2/SWC2 are moves through memory) and command words
 * (the 25-bit immediate of a COP2 instruction).
 *
 * Port build only (`SH_PORT`). Uses plain C types: the port compiles with `-nostdinc`.
 */

/** @brief Writes data register `reg` (cop2r0..31), as MTC2/LWC2 would. */
void Gte_DataWrite(unsigned int reg, unsigned int value);

/** @brief Reads data register `reg` (cop2r0..31), as MFC2/SWC2 would. */
unsigned int Gte_DataRead(unsigned int reg);

/** @brief Writes control register `reg` (cop2r32..63 as 0..31), as CTC2 would. */
void Gte_CtrlWrite(unsigned int reg, unsigned int value);

/** @brief Reads control register `reg` (cop2r32..63 as 0..31), as CFC2 would. */
unsigned int Gte_CtrlRead(unsigned int reg);

/** @brief Executes a GTE command (COP2 immediate: opcode in bits 0-5, lm bit 10, cv 13-14, v 15-16, mx 17-18, sf 19). */
void Gte_Command(unsigned int cmd);

/** @brief Resets all GTE registers to zero. */
void Gte_Reset(void);

/* Direct entry points for frequent command words (gte.c): constant Gte_Command() calls with these
 * words go straight to them, skipping the decode (not in profiling builds, which count commands). */
void Gte_CmdRtps(void);  /* 0x0180001 */
void Gte_CmdRtpt(void);  /* 0x0280030 */
void Gte_CmdNclip(void); /* 0x1400006 */
void Gte_CmdDpcs(void);  /* 0x0780010 */
/** Batched exact RTPS over `count` vertices in place (see gte.c); 0 when it can't (use the GTE). */
int Gte_RtpBatch(unsigned int* xy, short* z, int count);
int Gte_NcBatch(const short* v, unsigned int* out, int count);
#if !defined(GTE_IMPLEMENTATION) && !defined(SH_PORT_PROF)
static inline void Gte_CommandDirect(unsigned int cmd)
{
    if (__builtin_constant_p(cmd))
    {
        switch (cmd)
        {
            case 0x0180001: Gte_CmdRtps(); return;
            case 0x0280030: Gte_CmdRtpt(); return;
            case 0x1400006: Gte_CmdNclip(); return;
            case 0x0780010: Gte_CmdDpcs(); return;
            default: break;
        }
    }
    Gte_Command(cmd);
}
#define Gte_Command(cmd) Gte_CommandDirect(cmd)
#endif

/* Register moves inlined at their call sites (the game's GTE macros always name a constant register,
 * so a plain register becomes one load or store): the GTE's register files, and wrappers that fall
 * back to the functions above for registers with side effects. gte.c (GTE_IMPLEMENTATION) defines
 * the functions themselves. */
extern unsigned int g_GteData[32];
extern unsigned int g_GteCtrl[32];
/** Set by control register writes that gte.c caches (matrices, TR/BK/FC): the next command
 * refreshes its cache once instead of on every write. */
extern unsigned int g_GteCtrlDirty;

#ifndef GTE_IMPLEMENTATION

static inline void Gte_DataWriteInline(unsigned int reg, unsigned int value)
{
    switch (reg)
    {
        case 0: case 2: case 4: case 6: case 12: case 13: case 14: /* VXYn, RGBC, SXYn */
        case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 27: /* RGBn, RES1, MACn */
            g_GteData[reg] = value;
            break;
        case 1: case 3: case 5: case 8: case 9: case 10: case 11: /* VZn, IRn: signed 16-bit */
            g_GteData[reg] = (unsigned int)(int)(short)value;
            break;
        case 7: case 16: case 17: case 18: case 19: /* OTZ, SZn: unsigned 16-bit */
            g_GteData[reg] = value & 0xFFFF;
            break;
        default:
            (Gte_DataWrite)(reg, value);
            break;
    }
}

static inline unsigned int Gte_DataReadInline(unsigned int reg)
{
    if (reg == 15)
    {
        return g_GteData[14]; /* SXYP reads SXY2 */
    }
    if (reg == 28 || reg == 29)
    {
        return (Gte_DataRead)(reg); /* IRGB/ORGB */
    }
    return g_GteData[reg & 31];
}

static inline void Gte_CtrlWriteInline(unsigned int reg, unsigned int value)
{
    switch (reg)
    {
        case 24: case 25: case 28: /* OFX, OFY, DQB */
            g_GteCtrl[reg] = value;
            break;
        case 27: case 29: case 30: /* DQA, ZSF3, ZSF4 */
            g_GteCtrl[reg] = (unsigned int)(int)(short)value;
            break;
        case 26: /* H */
            g_GteCtrl[reg] = value & 0xFFFF;
            break;
        case 4: case 12: case 20: /* RT33, LLM33, LCM33: signed 16-bit */
            g_GteCtrl[reg] = (unsigned int)(int)(short)value;
            g_GteCtrlDirty = 1;
            break;
        case 0: case 1: case 2: case 3: case 5: case 6: case 7: case 8: case 9: case 10: case 11:
        case 13: case 14: case 15: case 16: case 17: case 18: case 19: case 21: case 22: case 23:
            g_GteCtrl[reg] = value; /* matrices, TR, BK, FC */
            g_GteCtrlDirty = 1;
            break;
        default: /* FLAG */
            (Gte_CtrlWrite)(reg, value);
            break;
    }
}

static inline unsigned int Gte_CtrlReadInline(unsigned int reg)
{
    if (reg == 26)
    {
        return (unsigned int)(int)(short)g_GteCtrl[26]; /* H reads back sign-expanded */
    }
    return g_GteCtrl[reg & 31];
}

#define Gte_DataWrite(reg, value) Gte_DataWriteInline((reg), (value))
#define Gte_DataRead(reg)         Gte_DataReadInline(reg)
#define Gte_CtrlWrite(reg, value) Gte_CtrlWriteInline((reg), (value))
#define Gte_CtrlRead(reg)         Gte_CtrlReadInline(reg)

#endif

#endif
