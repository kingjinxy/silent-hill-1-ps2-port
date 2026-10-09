/** @brief PS1 libspu for the port, on SPU2 core 0 (src/port/ps2/spu_ps2.c, src/port/iop/sh1spu).
 *
 * Implements the calls the sound driver (src/bodyprog/libsd, src/bodyprog/sound) makes, with
 * libspu's semantics. Each one becomes a register write to core 0:
 * - Sound memory: PS1 address A (bytes, 0-512 KB) is SPU2 byte address A + SPU_BASE. SPU_BASE keeps
 *   clear of the SPU2's reserved low area, and puts the end of the PS1's 512 KB (where libspu places
 *   the reverb work area) on a 128 KB boundary, which is what the reverb end register (EEA) needs.
 *   SPU2 address registers count halfwords.
 * - Pitch: SPU2 voices play at 48 kHz for pitch 0x1000, the PS1's at 44.1 kHz, so pitches are scaled
 *   by 44100 / 48000.
 * - Volumes, ADSR and the reverb parameters are in the same formats. The reverb presets are
 *   libspu's (_spu_rev_param, read from BODYPROG.BIN). Their offsets are in 8-byte units on the PS1
 *   and in halfwords on the SPU2.
 * - Key status: libspu keeps the key state itself and reads the envelope from the hardware. Here the
 *   envelope comes from the IOP's status block. Until the IOP has run a voice's key on, its envelope
 *   counts as still sounding, as on the PS1 right after a key on.
 */

#include "common.h"
#include "libspu.h"

extern void         Port_SpuStart(void);
extern void         Port_SpuReg(unsigned int off, unsigned int value);
extern void         Port_SpuFlush(void);
extern void         Port_SpuUpload(unsigned int addr, const void* data, unsigned int size);
extern void         Port_SpuClear(unsigned int addr, unsigned int size);
extern unsigned int Port_SpuSent(void);
extern unsigned int Port_SpuDone(void);
extern unsigned int Port_SpuEnvx(int v);

#define SPU_BASE  0x20000         /* SPU2 byte address of PS1 sound memory address 0 */
#define PS1_RAM   0x80000
#define VOICES    24

/* SPU2 core 0 registers (offsets from 0xBF900000). */
#define R_VOICE(v, r) ((v) * 0x10 + (r)) /* r: 0 VOLL, 2 VOLR, 4 PITCH, 6 ADSR1, 8 ADSR2 */
#define R_ADDR(v, r)  (0x1C0 + (v) * 0xC + (r)) /* r: 0 SSA, 4 LSAX (hi, lo) */
#define R_VMIXEL      0x18C
#define R_VMIXER      0x194
#define R_ATTR        0x19A
#define R_KON         0x1A0
#define R_KOFF        0x1A4
#define R_ESA         0x2E0
#define R_REVADDR     0x2E4 /* 22 address registers (hi, lo), then EEA */
#define R_EEA         0x33C
#define R_MVOLL       0x760
#define R_MVOLR       0x762
#define R_EVOLL       0x764
#define R_EVOLR       0x766
#define R_REVVOL      0x774 /* vIIR, vCOMB1-4, vWALL, vAPF1, vAPF2, vLIN, vRIN */

/* libspu's reverb presets (_spu_rev_param, BODYPROG.BIN 0x800B1478): the PS1's 32 reverb registers
 * (0x1F801DC0..) per mode, and each mode's work area start in 8-byte units (_spu_rev_startaddr). */
static const u16 REV_PARAM[SPU_REV_MODE_MAX][32] = {
    { 0 },
    { 0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227,
      0x0374, 0x01EF, 0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x01B4, 0x0136,
      0x00B8, 0x005C, 0x8000, 0x8000 },
    { 0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF,
      0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080,
      0x004C, 0x0026, 0x8000, 0x8000 },
    { 0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F,
      0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2,
      0x0100, 0x0080, 0x8000, 0x8000 },
    { 0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C,
      0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238,
      0x0154, 0x00AA, 0x8000, 0x8000 },
    { 0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD,
      0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A,
      0x0274, 0x013A, 0x8000, 0x8000 },
    { 0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B,
      0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2,
      0x0464, 0x0232, 0x8000, 0x8000 },
    { 0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005, 0x0005,
      0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1004, 0x1002,
      0x0004, 0x0002, 0x8000, 0x8000 },
    { 0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005, 0x0005,
      0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1004, 0x1002,
      0x0004, 0x0002, 0x8000, 0x8000 },
    { 0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF,
      0x02B0, 0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059, 0x01A0, 0x00E3, 0x0058, 0x0040,
      0x0028, 0x0014, 0x8000, 0x8000 },
};
static const u16 REV_START[SPU_REV_MODE_MAX] = { 0xFFFE, 0xFB28, 0xFC18, 0xF6F8, 0xF204,
                                                 0xEA44, 0xE128, 0xCFF8, 0xCFF8, 0xF880 };

typedef struct
{
    SpuVoiceAttr attr;    /* as last set, for SpuGetVoiceAttr */
    u16          adsr1;
    u16          adsr2;
    u32          konSent; /* Port_SpuSent() after its last key on */
} Voice;

static Voice         s_Voices[VOICES];
static unsigned long s_KeyOn;      /* voices keyed on (libspu's key state) */
static unsigned long s_RevVoices;
static long          s_RevMode;
static long          s_RevOn;
static u32           s_TransAddr;  /* SpuSetTransferStartAddr */
static u32           s_TransSent;  /* Port_SpuSent() after the last transfer */
static long          s_TransMode;
static SpuCommonAttr s_Common;

static void reg(unsigned int off, unsigned int value)
{
    Port_SpuReg(off, value);
}

/** An SPU2 address register pair (hi, lo) for PS1 sound memory byte address a. */
static void reg_addr(unsigned int off, u32 a)
{
    u32 hw = (a + SPU_BASE) >> 1;
    reg(off, hw >> 16);
    reg(off + 2, hw & 0xFFFF);
}

static u16 volume(s16 vol, s16 mode)
{
    static const u16 SWEEP[8] = { 0, 0x8000, 0x9000, 0xA000, 0xB000, 0xC000, 0xD000, 0xE000 };
    if (mode <= SPU_VOICE_DIRECT || mode > SPU_VOICE_EXPDec)
    {
        return (u16)vol & 0x7FFF;
    }
    return SWEEP[mode] | ((u16)vol & 0x7F);
}

static void keys(unsigned int off, unsigned long bits)
{
    reg(off, bits & 0xFFFF);
    reg(off + 2, (bits >> 16) & 0xFF);
}

void SpuInit(void)
{
    int v;
    Port_SpuStart();
    keys(R_KOFF, 0xFFFFFF);
    for (v = 0; v < VOICES; v++)
    {
        reg(R_VOICE(v, 0), 0);
        reg(R_VOICE(v, 2), 0);
    }
    s_KeyOn = 0;
    keys(R_VMIXEL, 0);
    keys(R_VMIXER, 0);
    s_RevVoices = 0;
    reg(R_ATTR, 0x8000); /* core on, reverb off */
    s_RevOn = 0;
    reg(R_MVOLL, 0);
    reg(R_MVOLR, 0);
    reg(R_EVOLL, 0);
    reg(R_EVOLR, 0);
    reg(R_EEA, ((SPU_BASE + PS1_RAM - 1) >> 1) >> 16);
    Port_SpuFlush();
}

void SpuQuit(void)
{
    keys(R_KOFF, 0xFFFFFF);
    Port_SpuFlush();
}

long SpuSetReverb(long on_off)
{
    s_RevOn = on_off ? 1 : 0;
    reg(R_ATTR, 0x8000 | (s_RevOn << 7));
    return s_RevOn;
}

long SpuSetReverbModeParam(SpuReverbAttr* attr)
{
    int i;
    if (attr->mask == 0 || (attr->mask & SPU_REV_MODE))
    {
        long mode = attr->mode & ~SPU_REV_MODE_CLEAR_WA;
        if (mode < 0 || mode >= SPU_REV_MODE_MAX)
        {
            return -1;
        }
        s_RevMode = mode;
        reg(R_ATTR, 0x8000); /* reverb off while its parameters change */
        if (mode != SPU_REV_MODE_OFF)
        {
            reg_addr(R_ESA, (u32)REV_START[mode] * 8);
        }
        reg(R_REVADDR + 0, ((u32)REV_PARAM[mode][0] * 4) >> 16);
        reg(R_REVADDR + 2, ((u32)REV_PARAM[mode][0] * 4) & 0xFFFF);
        reg(R_REVADDR + 4, ((u32)REV_PARAM[mode][1] * 4) >> 16);
        reg(R_REVADDR + 6, ((u32)REV_PARAM[mode][1] * 4) & 0xFFFF);
        for (i = 0; i < 20; i++)
        {
            u32 v = (u32)REV_PARAM[mode][10 + i] * 4;
            reg(R_REVADDR + 8 + i * 4, v >> 16);
            reg(R_REVADDR + 10 + i * 4, v & 0xFFFF);
        }
        for (i = 0; i < 8; i++) /* vIIR, vCOMB1-4, vWALL, vAPF1, vAPF2 */
        {
            reg(R_REVVOL + i * 2, REV_PARAM[mode][2 + i]);
        }
        reg(R_REVVOL + 16, REV_PARAM[mode][30]);
        reg(R_REVVOL + 18, REV_PARAM[mode][31]);
        if ((attr->mode & SPU_REV_MODE_CLEAR_WA) && mode != SPU_REV_MODE_OFF)
        {
            SpuClearReverbWorkArea(mode);
        }
        reg(R_ATTR, 0x8000 | (s_RevOn << 7));
    }
    if (attr->mask == 0 || (attr->mask & SPU_REV_DEPTHL))
    {
        reg(R_EVOLL, (u16)attr->depth.left);
    }
    if (attr->mask == 0 || (attr->mask & SPU_REV_DEPTHR))
    {
        reg(R_EVOLR, (u16)attr->depth.right);
    }
    return 0;
}

long SpuReserveReverbWorkArea(long on_off)
{
    return on_off;
}

unsigned long SpuSetReverbVoice(long on_off, unsigned long voice_bit)
{
    if (on_off)
    {
        s_RevVoices |= voice_bit & 0xFFFFFF;
    }
    else
    {
        s_RevVoices &= ~voice_bit;
    }
    keys(R_VMIXEL, s_RevVoices);
    keys(R_VMIXER, s_RevVoices);
    return s_RevVoices;
}

unsigned long SpuGetReverbVoice(void)
{
    return s_RevVoices;
}

long SpuClearReverbWorkArea(long mode)
{
    if (mode <= SPU_REV_MODE_OFF || mode >= SPU_REV_MODE_MAX)
    {
        return 0;
    }
    Port_SpuClear(SPU_BASE + (u32)REV_START[mode] * 8, PS1_RAM - (u32)REV_START[mode] * 8);
    return 1;
}

unsigned long SpuWrite(unsigned char* addr, unsigned long size)
{
    if (s_TransAddr + size > PS1_RAM)
    {
        size = PS1_RAM - s_TransAddr;
    }
    Port_SpuUpload(SPU_BASE + s_TransAddr, addr, size);
    s_TransSent = Port_SpuSent();
    return size;
}

long SpuSetTransferMode(long mode)
{
    s_TransMode = mode;
    return mode;
}

unsigned long SpuSetTransferStartAddr(unsigned long addr)
{
    if (addr < 0x1010 || addr >= PS1_RAM)
    {
        return 0;
    }
    s_TransAddr = addr & ~7UL;
    return s_TransAddr;
}

long SpuIsTransferCompleted(long flag)
{
    Port_SpuFlush();
    if (flag == SPU_TRANSFER_WAIT)
    {
        while ((s32)(Port_SpuDone() - s_TransSent) < 0)
        {
        }
        return 1;
    }
    return (s32)(Port_SpuDone() - s_TransSent) >= 0;
}

void SpuSetVoiceAttr(SpuVoiceAttr* arg)
{
    unsigned long m = arg->mask ? arg->mask : 0xFFFFFFFF;
    int           v;
    for (v = 0; v < VOICES; v++)
    {
        Voice*        p = &s_Voices[v];
        SpuVoiceAttr* a = &p->attr;
        if (!(arg->voice & (1UL << v)))
        {
            continue;
        }
        if (m & (SPU_VOICE_VOLL | SPU_VOICE_VOLMODEL))
        {
            if (m & SPU_VOICE_VOLL)
            {
                a->volume.left = arg->volume.left;
            }
            if (m & SPU_VOICE_VOLMODEL)
            {
                a->volmode.left = arg->volmode.left;
            }
            reg(R_VOICE(v, 0), volume(a->volume.left, a->volmode.left));
        }
        if (m & (SPU_VOICE_VOLR | SPU_VOICE_VOLMODER))
        {
            if (m & SPU_VOICE_VOLR)
            {
                a->volume.right = arg->volume.right;
            }
            if (m & SPU_VOICE_VOLMODER)
            {
                a->volmode.right = arg->volmode.right;
            }
            reg(R_VOICE(v, 2), volume(a->volume.right, a->volmode.right));
        }
        if (m & SPU_VOICE_PITCH)
        {
            u32 pitch = (u32)arg->pitch * 44100 / 48000;
            a->pitch  = arg->pitch;
            reg(R_VOICE(v, 4), pitch > 0x3FFF ? 0x3FFF : pitch);
        }
        if (m & SPU_VOICE_WDSA)
        {
            a->addr = arg->addr;
            reg_addr(R_ADDR(v, 0), arg->addr);
        }
        if (m & SPU_VOICE_LSAX)
        {
            a->loop_addr = arg->loop_addr;
            reg_addr(R_ADDR(v, 4), arg->loop_addr);
        }
        if (m & SPU_VOICE_ADSR_ADSR1)
        {
            p->adsr1 = arg->adsr1;
        }
        if (m & SPU_VOICE_ADSR_ADSR2)
        {
            p->adsr2 = arg->adsr2;
        }
        if (m & SPU_VOICE_ADSR_AMODE)
        {
            p->adsr1 = (p->adsr1 & 0x7FFF) | (arg->a_mode == SPU_VOICE_EXPIncN ? 0x8000 : 0);
        }
        if (m & SPU_VOICE_ADSR_AR)
        {
            p->adsr1 = (p->adsr1 & ~0x7F00) | ((arg->ar & 0x7F) << 8);
        }
        if (m & SPU_VOICE_ADSR_DR)
        {
            p->adsr1 = (p->adsr1 & ~0x00F0) | ((arg->dr & 0xF) << 4);
        }
        if (m & SPU_VOICE_ADSR_SL)
        {
            p->adsr1 = (p->adsr1 & ~0x000F) | (arg->sl & 0xF);
        }
        if (m & SPU_VOICE_ADSR_SMODE)
        {
            static const u16 SMODE[8] = { 0, 0, 0, 0x4000, 0x4000, 0x8000, 0x8000, 0xC000 };
            p->adsr2 = (p->adsr2 & 0x3FFF) | SMODE[arg->s_mode & 7];
        }
        if (m & SPU_VOICE_ADSR_SR)
        {
            p->adsr2 = (p->adsr2 & ~0x1FC0) | ((arg->sr & 0x7F) << 6);
        }
        if (m & SPU_VOICE_ADSR_RMODE)
        {
            p->adsr2 = (p->adsr2 & ~0x0020) | (arg->r_mode == SPU_VOICE_EXPDec ? 0x20 : 0);
        }
        if (m & SPU_VOICE_ADSR_RR)
        {
            p->adsr2 = (p->adsr2 & ~0x001F) | (arg->rr & 0x1F);
        }
        if (m & (SPU_VOICE_ADSR_ADSR1 | SPU_VOICE_ADSR_AMODE | SPU_VOICE_ADSR_AR | SPU_VOICE_ADSR_DR |
                 SPU_VOICE_ADSR_SL))
        {
            a->adsr1 = p->adsr1;
            reg(R_VOICE(v, 6), p->adsr1);
        }
        if (m & (SPU_VOICE_ADSR_ADSR2 | SPU_VOICE_ADSR_SMODE | SPU_VOICE_ADSR_SR | SPU_VOICE_ADSR_RMODE |
                 SPU_VOICE_ADSR_RR))
        {
            a->adsr2 = p->adsr2;
            reg(R_VOICE(v, 8), p->adsr2);
        }
    }
}

void SpuGetVoiceAttr(SpuVoiceAttr* arg)
{
    int v;
    for (v = 0; v < VOICES; v++)
    {
        if (arg->voice & (1UL << v))
        {
            unsigned long voice = arg->voice;
            *arg                = s_Voices[v].attr;
            arg->voice          = voice;
            arg->volumex        = arg->volume;
            arg->envx           = (short)Port_SpuEnvx(v);
            return;
        }
    }
}

void SpuSetKey(long on_off, unsigned long voice_bit)
{
    int v;
    voice_bit &= 0xFFFFFF;
    if (on_off)
    {
        keys(R_KON, voice_bit);
        Port_SpuFlush();
        s_KeyOn |= voice_bit;
        for (v = 0; v < VOICES; v++)
        {
            if (voice_bit & (1UL << v))
            {
                s_Voices[v].konSent = Port_SpuSent();
            }
        }
    }
    else
    {
        keys(R_KOFF, voice_bit);
        s_KeyOn &= ~voice_bit;
    }
}

void SpuSetKeyOnWithAttr(SpuVoiceAttr* attr)
{
    SpuSetVoiceAttr(attr);
    SpuSetKey(SPU_ON, attr->voice);
}

long SpuGetKeyStatus(unsigned long voice_bit)
{
    int v, sounding;
    voice_bit &= 0xFFFFFF;
    if (voice_bit == 0)
    {
        return -1;
    }
    for (v = 0; !(voice_bit & (1UL << v)); v++)
    {
    }
    Port_SpuFlush();
    sounding = Port_SpuEnvx(v) != 0 || (s32)(Port_SpuDone() - s_Voices[v].konSent) < 0;
    if (s_KeyOn & (1UL << v))
    {
        return sounding ? SPU_ON : SPU_ON_ENV_OFF;
    }
    return sounding ? SPU_OFF_ENV_ON : SPU_OFF;
}

void SpuSetCommonAttr(SpuCommonAttr* attr)
{
    unsigned long m = attr->mask ? attr->mask : 0xFFFFFFFF;
    if (m & (SPU_COMMON_MVOLL | SPU_COMMON_MVOLMODEL))
    {
        if (m & SPU_COMMON_MVOLL)
        {
            s_Common.mvol.left = attr->mvol.left;
        }
        if (m & SPU_COMMON_MVOLMODEL)
        {
            s_Common.mvolmode.left = attr->mvolmode.left;
        }
        reg(R_MVOLL, volume(s_Common.mvol.left, s_Common.mvolmode.left));
    }
    if (m & (SPU_COMMON_MVOLR | SPU_COMMON_MVOLMODER))
    {
        if (m & SPU_COMMON_MVOLR)
        {
            s_Common.mvol.right = attr->mvol.right;
        }
        if (m & SPU_COMMON_MVOLMODER)
        {
            s_Common.mvolmode.right = attr->mvolmode.right;
        }
        reg(R_MVOLR, volume(s_Common.mvol.right, s_Common.mvolmode.right));
    }
    /* CD input (XA voices) and external input: not played yet. */
    if (m & SPU_COMMON_CDVOLL)
    {
        s_Common.cd.volume.left = attr->cd.volume.left;
    }
    if (m & SPU_COMMON_CDVOLR)
    {
        s_Common.cd.volume.right = attr->cd.volume.right;
    }
    if (m & SPU_COMMON_CDMIX)
    {
        s_Common.cd.mix = attr->cd.mix;
    }
}

long SpuInitMalloc(long num, char* top)
{
    (void)top;
    return num;
}
