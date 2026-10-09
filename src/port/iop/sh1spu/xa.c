/** @brief XA audio (the cutscenes' voice lines) for sh1spu.irx: the PS1 CD drive's XA playback,
 * done on the IOP.
 *
 * On the PS1, the game seeks to an XA file, sets a file/channel filter and starts a real-time read.
 * The drive then decodes that channel's audio sectors and feeds them into the SPU's CD input. Here:
 * - A reader thread reads the original raw sectors from HILL. on the DVD (2336 bytes each: an
 *   8-byte subheader, then 2304 bytes of sound groups) and keeps the sectors of the chosen file and
 *   channel that carry audio. It stops at the end-of-file flag or when the game stops the stream.
 * - SPU2 core 0's sound data input (ADMA) plays a looping buffer of two halves. At the end of each
 *   half, a mixer thread refills it: it decodes XA-ADPCM (4-bit; mono or stereo; 37.8 or 18.9 kHz)
 *   and resamples to 48 kHz with the PS1 SPU's 4-point Gaussian interpolation. With no stream, it
 *   writes silence.
 * The input's volume and mix (the PS1's CD volume and CD mix) are set by libspu on the EE.
 */

#include "irx_imports.h"
#include "gauss_table.h"

#define PS1_RAW     2336 /* raw HILL. sector: 8-byte subheader + 2328 */
#define XA_DATA     2304 /* 18 sound groups of 128 bytes */
#define BATCH       16   /* PS1 sectors per disc read */
#define READ_SECS   ((BATCH * PS1_RAW + 2047) / 2048 + 1)
#define FIFO_N      12   /* audio sectors waiting (about 0.6 s of stereo 37.8 kHz) */
#define HALF_FRAMES 1024 /* frames per ADMA half (8 blocks of 128; about 21 ms) */
#define DEC_MAX     4032 /* frames from one sector (mono) */

typedef struct
{
    u8  data[XA_DATA];
    u8  coding;
} sector_t;

static u8       s_ReadSpace[READ_SECS * 2048 + 64];
static u8*      s_Read;
static sector_t s_Fifo[FIFO_N];
static volatile int s_FifoIn, s_FifoOut; /* running counts */
static u8       s_AdmaSpace[HALF_FRAMES * 4 * 2 + 64];
static u8*      s_Adma;                  /* two halves of blocks: 256 bytes left, 256 bytes right */
static int      s_MixSema, s_ReadSema;

/* Stream request (set by xa_start, read by the reader). */
static volatile int s_Active;
static volatile u32 s_Gen;
static u32          s_HillLsn, s_Index, s_File, s_Chan;
static int          s_Started; /* s_FifoIn when the stream started */

/* Decoder and resampler (mixer thread only). */
static s16  s_DecL[DEC_MAX + 3], s_DecR[DEC_MAX + 3]; /* 3 frames of history, then a sector */
static int  s_DecCount = 3;                          /* frames in s_Dec (history included) */
static u32  s_Pos;                                   /* 16.16 position in s_Dec */
static u32  s_Step;                                  /* 16.16 source frames per output frame */
static int  s_OldL, s_OlderL, s_OldR, s_OlderR;
static u32  s_MixGen;
static u32  s_Fills;
static u32  s_Madr[4]; /* DMA address at the last interrupts */
static int  s_Peak; /* loudest output sample since the last diagnostic line */

static const int K0[4] = { 0, 60, 115, 98 };
static const int K1[4] = { 0, 0, -52, -55 };

/** Decodes one 4-bit sound unit (28 samples) into out[0], out[step], ... */
static void decode_unit(const u8* group, int unit, s16* out, int step, int* old, int* older)
{
    int param  = group[4 + unit];
    int shift  = param & 0xF;
    int filter = (param >> 4) & 3;
    int j;
    if (shift > 12)
    {
        shift = 9;
    }
    for (j = 0; j < 28; j++)
    {
        int b = group[16 + j * 4 + (unit >> 1)];
        int n = (unit & 1) ? b >> 4 : b & 0xF;
        int s = (s16)(n << 12) >> shift;
        s += (*old * K0[filter] + *older * K1[filter] + 32) >> 6;
        if (s > 32767)
        {
            s = 32767;
        }
        if (s < -32768)
        {
            s = -32768;
        }
        *older         = *old;
        *old           = s;
        out[j * step] = (s16)s;
    }
}

/** Decodes the next sector into s_Dec (after the 3 frames of history); 0 if none is waiting. */
static int decode_next(void)
{
    sector_t* sec;
    int       g, u, frames, stereo, i;
    if (s_FifoOut == s_FifoIn)
    {
        return 0;
    }
    sec    = &s_Fifo[s_FifoOut % FIFO_N];
    stereo = (sec->coding & 3) == 1;
    s_Step = ((sec->coding >> 2) & 3) ? (18900u << 16) / 48000 : (37800u << 16) / 48000;
    for (i = 0; i < 3; i++)
    {
        s_DecL[i] = s_DecL[s_DecCount - 3 + i];
        s_DecR[i] = s_DecR[s_DecCount - 3 + i];
    }
    frames = 0;
    for (g = 0; g < 18; g++)
    {
        const u8* group = &sec->data[g * 128];
        if (stereo)
        {
            for (u = 0; u < 8; u += 2)
            {
                decode_unit(group, u, &s_DecL[3 + frames], 1, &s_OldL, &s_OlderL);
                decode_unit(group, u + 1, &s_DecR[3 + frames], 1, &s_OldR, &s_OlderR);
                frames += 28;
            }
        }
        else
        {
            for (u = 0; u < 8; u++)
            {
                decode_unit(group, u, &s_DecL[3 + frames], 1, &s_OldL, &s_OlderL);
                for (i = 0; i < 28; i++)
                {
                    s_DecR[3 + frames + i] = s_DecL[3 + frames + i];
                }
                frames += 28;
            }
        }
    }
    s_FifoOut++;
    s_DecCount = 3 + frames;
    return 1;
}

static void reset_decoder(void)
{
    int i;
    for (i = 0; i < 3; i++)
    {
        s_DecL[i] = s_DecR[i] = 0;
    }
    s_DecCount = 3;
    s_Pos      = 0;
    s_OldL = s_OlderL = s_OldR = s_OlderR = 0;
}

/** Fills one ADMA half: blocks of 128 left samples, then 128 right ones. */
static void fill(u8* half)
{
    int f;
    if (s_MixGen != s_Gen) /* a new stream (or a stop) since the last fill */
    {
        s_MixGen = s_Gen;
        reset_decoder();
    }
    for (f = 0; f < HALF_FRAMES; f++)
    {
        s16* l = (s16*)(half + (f >> 7) * 512) + (f & 127);
        int  n, i, outL, outR;
        /* Frames n-3..n of s_Dec (n = 3 + whole part of the position) are the four taps. */
        while ((n = 3 + (int)(s_Pos >> 16)) >= s_DecCount)
        {
            u32 played = (u32)(s_DecCount - 3); /* the new sector follows the frames played */
            if (!decode_next())
            {
                break;
            }
            s_Pos -= played << 16;
        }
        if (n >= s_DecCount)
        {
            l[0] = l[128] = 0; /* nothing to play */
            continue;
        }
        i    = (s_Pos >> 8) & 0xFF;
        outL = ((GAUSS[0xFF - i] * s_DecL[n - 3]) >> 15) + ((GAUSS[0x1FF - i] * s_DecL[n - 2]) >> 15) +
               ((GAUSS[0x100 + i] * s_DecL[n - 1]) >> 15) + ((GAUSS[i] * s_DecL[n]) >> 15);
        outR = ((GAUSS[0xFF - i] * s_DecR[n - 3]) >> 15) + ((GAUSS[0x1FF - i] * s_DecR[n - 2]) >> 15) +
               ((GAUSS[0x100 + i] * s_DecR[n - 1]) >> 15) + ((GAUSS[i] * s_DecR[n]) >> 15);
        if (outL > s_Peak)
        {
            s_Peak = outL;
        }
        if (-outL > s_Peak)
        {
            s_Peak = -outL;
        }
        l[0]   = (s16)(outL > 32767 ? 32767 : outL < -32768 ? -32768 : outL);
        l[128] = (s16)(outR > 32767 ? 32767 : outR < -32768 ? -32768 : outR);
        s_Pos += s_Step;
    }
}

static volatile u32 s_Interrupts; /* ADMA half interrupts (about 47 a second) */
static volatile int s_Stopped;

static int adma_done(int core, void* arg)
{
    (void)core;
    (void)arg;
    s_Interrupts++;
    if (!s_Stopped)
    {
        iSignalSema(s_MixSema);
    }
    return 1;
}

/** Once a second (main.c's loop): checks the interrupt rate. Far more than 47 a second means the
 * input stream isn't running as set up; it is then stopped so it can't starve the IOP. */
void xa_check(void)
{
    static u32 last;
    static int seconds;
    u32        n = s_Interrupts - last;
    last         = s_Interrupts;
    if (s_Active || s_FifoOut != s_FifoIn)
    {
        printf("sh1spu: XA diag: MMIX %04x AVOL %04x/%04x BVOL %04x/%04x ATTR %04x MVOL %04x, %u sectors decoded, %u interrupts/s\n",
               *(volatile u16*)0xBF900198, *(volatile u16*)0xBF900768, *(volatile u16*)0xBF90076A,
               *(volatile u16*)0xBF90076C, *(volatile u16*)0xBF90076E, *(volatile u16*)0xBF90019A,
               *(volatile u16*)0xBF900760, (unsigned)s_FifoOut, (unsigned)n);
        printf("sh1spu: XA diag: peak %d; core 0 ADMAS %04x; core 1 MMIX %04x AVOL %04x BVOL %04x MVOL %04x ATTR %04x\n",
               s_Peak, *(volatile u16*)0xBF9001B0, *(volatile u16*)0xBF900598, *(volatile u16*)0xBF900790,
               *(volatile u16*)0xBF900794, *(volatile u16*)0xBF900788, *(volatile u16*)0xBF90059A);
        printf("sh1spu: XA diag: %u fills, coding %02x, buffer %08x, DMA at %08x %08x %08x %08x\n", (unsigned)s_Fills,
               s_Fifo[(s_FifoOut + FIFO_N - 1) % FIFO_N].coding, (unsigned)s_Adma, (unsigned)s_Madr[0],
               (unsigned)s_Madr[1], (unsigned)s_Madr[2], (unsigned)s_Madr[3]);
        s_Peak = 0;
    }
    if (++seconds <= 3)
    {
        printf("sh1spu: XA input: %u interrupts in the last second\n", (unsigned)n);
    }
    if (n > 200 && !s_Stopped)
    {
        s_Stopped = 1;
        sceSdBlockTrans(0, SD_TRANS_STOP, NULL, 0);
        printf("sh1spu: XA input stopped (%u interrupts a second)\n", (unsigned)n);
    }
}

#define D4_MADR (*(volatile u32*)0xBF8010C0) /* IOP DMA channel 4 (core 0's input): address being read */

/** Refills the half the input DMA isn't reading, once per pass. The interrupt doesn't come exactly
 * once per half (59 a second in PCSX2, 77 on a PS2, for 47 halves a second), so the mixer goes by the
 * DMA's position: filling on every interrupt played the stream ahead of the SPU2, skipping chunks. */
static void mixer(void* arg)
{
    int filled = 1; /* half 1 starts out silent and is due after half 0 */
    (void)arg;
    for (;;)
    {
        int playing, other;
        WaitSema(s_MixSema);
        s_Madr[s_Fills & 3] = D4_MADR;
        playing = (int)((D4_MADR & 0x1FFFFF) - ((u32)s_Adma & 0x1FFFFF)) >= HALF_FRAMES * 4;
        other   = playing ^ 1;
        if (other != filled)
        {
            fill(s_Adma + other * HALF_FRAMES * 4);
            filled = other;
            s_Fills++;
        }
    }
}

static void reader(void* arg)
{
    sceCdRMode mode;
    (void)arg;
    mode.trycount    = 0;
    mode.spindlctrl  = SCECdSpinNom;
    mode.datapattern = SCECdSecS2048;
    for (;;)
    {
        u32 gen, off, lsn, skip, count, k;
        if (!s_Active)
        {
            WaitSema(s_ReadSema);
            continue;
        }
        gen   = s_Gen;
        off   = s_Index * PS1_RAW;
        lsn   = s_HillLsn + off / 2048;
        skip  = off % 2048;
        count = (skip + BATCH * PS1_RAW + 2047) / 2048;
        if (!sceCdRead(lsn, count, s_Read, &mode))
        {
            DelayThread(2000); /* drive busy (a data read from the EE): try again */
            continue;
        }
        sceCdSync(0);
        for (k = 0; k < BATCH && s_Active && gen == s_Gen; k++)
        {
            const u8* p = s_Read + skip + k * PS1_RAW;
            if (p[0] != s_File || p[1] != s_Chan || !(p[2] & 0x04)) /* file, channel, audio */
            {
                continue;
            }
            while (s_FifoIn - s_FifoOut >= FIFO_N && s_Active && gen == s_Gen)
            {
                DelayThread(5000);
            }
            if (!s_Active || gen != s_Gen)
            {
                break;
            }
            memcpy(s_Fifo[s_FifoIn % FIFO_N].data, p + 8, XA_DATA);
            s_Fifo[s_FifoIn % FIFO_N].coding = p[3];
            s_FifoIn++;
            if (p[2] & 0x80) /* end of file */
            {
                s_Active = 0;
                printf("sh1spu: XA end of file (%d sectors played)\n", s_FifoIn - s_Started);
            }
        }
        if (gen == s_Gen)
        {
            s_Index += BATCH;
        }
    }
}

/** Starts playing file/channel of the XA data at PS1 sector `index` of HILL. (DVD sector hillLsn). */
void xa_start(u32 hillLsn, u32 index, u32 file, u32 chan)
{
    s_Active  = 0;
    s_Gen++;
    s_FifoOut = s_FifoIn;
    s_HillLsn = hillLsn;
    s_Index   = index;
    s_File    = file;
    s_Chan    = chan;
    s_Started = s_FifoIn;
    s_Active  = 1;
    printf("sh1spu: XA file %u channel %u from HILL. sector %u\n", (unsigned)file, (unsigned)chan, (unsigned)index);
    SignalSema(s_ReadSema);
}

void xa_stop(void)
{
    s_Active = 0;
    s_Gen++;
    s_FifoOut = s_FifoIn;
}

/** Sets up the threads and starts core 0's sound data input, playing silence. Channel 0's DMA then
 * stays with it: sample uploads use channel 1 (sh1spu main.c). */
void xa_init(void)
{
    iop_sema_t   sema;
    iop_thread_t th;
    s_Read = (u8*)(((u32)s_ReadSpace + 63) & ~63u);
    s_Adma = (u8*)(((u32)s_AdmaSpace + 63) & ~63u);
    memset(s_Adma, 0, HALF_FRAMES * 4 * 2);
    reset_decoder();
    sema.attr    = 0;
    sema.option  = 0;
    sema.initial = 0;
    sema.max     = 2;
    s_MixSema    = CreateSema(&sema);
    sema.max     = 1;
    s_ReadSema   = CreateSema(&sema);
    th.attr      = TH_C;
    th.option    = 0;
    th.thread    = mixer;
    th.priority  = 50;
    th.stacksize = 0x800;
    StartThread(CreateThread(&th), NULL);
    th.thread    = reader;
    th.priority  = 55;
    th.stacksize = 0x800;
    StartThread(CreateThread(&th), NULL);
    sceSdSetTransIntrHandler(0, adma_done, NULL);
    sceSdBlockTrans(0, SD_TRANS_WRITE | SD_TRANS_LOOP, s_Adma, HALF_FRAMES * 4 * 2);
}
