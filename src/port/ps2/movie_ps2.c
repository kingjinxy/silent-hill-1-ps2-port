/** @brief FMV playback for the port (src/screens/stream/stream.c movie_main under SH_PORT).
 *
 * On the PS1 the game streams a movie with libcd (StSetRing/StSetStream), decodes each frame's
 * bitstream on the CPU and with the MDEC, and shows it in 24-bit display mode, while the drive plays
 * the interleaved XA audio. Here:
 * - One reader, as the PS1's drive: the IOP's XA reader (sh1spu xa.c) plays the movie's audio (file
 *   1, channel 1) and forwards its video sectors (file 0) to the EE: each sector's 2048 data bytes
 *   into a ring here, and a count of sectors sent (Port_XaForward; the EE acknowledges what it has
 *   used, Port_XaAck). A second reader on the EE confused the drive's busy status between the two
 *   and got stale buffers. The reader runs at the audio's pace (it waits while the audio queue is
 *   full), so the video arrives in step with the sound.
 * - Video sectors (the 32-byte STR header: chunk, chunks, frame number, size, width, height) are
 *   assembled into frames, decoded by strdec.c into 32-bit RGB, and shown from a 32-bit GS buffer
 *   (display_ps2.c Display_PresentMovie), not the PS1's VRAM.
 * - Frame n is shown 4(n - 1) vertical blanks after the start: 10 sectors a frame at a 2x drive's
 *   150 sectors a second.
 * The game's loop (stream.c) calls Port_MovieTick once a frame and keeps its own controls (skip,
 * soft reset, frame limit). */

#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include "strdec.h"

#define SECTOR    2048 /* forwarded data per video sector: 32-byte STR header + 2016 */
#define PAYLOAD   2016
#define RING      256  /* sectors */
#define FRAME_MAX 32   /* video sectors per frame */
#define W_MAX     320
#define H_MAX     240
#define LINES     224  /* the display: 320x224 progressive, as in gameplay */
#define VBLANKS_PER_FRAME 4
#define STARVED   180  /* vertical blanks without a new sector at the end: the stream is over */

extern int  Port_CdHill(unsigned int* lsn, unsigned int* ps1Start);                                    /* libcd_ps2.c */
extern void Port_XaStart(unsigned int hillLsn, unsigned int index, unsigned int file, unsigned int chan); /* spu_ps2.c */
extern void Port_XaStop(void);
extern void Port_XaForward(void* ring, unsigned int sectors, void* count);
extern void Port_XaAck(unsigned int used);
extern int  Port_VBlanks(unsigned int* cycles);                                                        /* libetc_ps2.c */
extern void Display_PresentMovie(const unsigned int* rgba, int w, int h, int lines);                   /* display_ps2.c */
extern void Display_MovieEnd(void);

static unsigned char s_Ring[RING][SECTOR] __attribute__((aligned(64)));
static unsigned int  s_Count[16] __attribute__((aligned(64))); /* [0] sectors sent, [1] stream generation (xa.c) */
static unsigned char s_Frame[FRAME_MAX * PAYLOAD] __attribute__((aligned(64)));
static unsigned int  s_Rgba[W_MAX * H_MAX] __attribute__((aligned(64)));

static int s_Open;
static int s_Used;    /* forwarded sectors used (a running count) */
static int s_Acked;
static int s_FrameNo = -1, s_FrameChunks, s_FrameGot, s_W, s_H;
static int s_Ready;   /* a decoded frame waits in s_Rgba */
static int s_ReadyNo;
static int s_Shown;   /* last shown frame number */
static int s_T0;      /* vertical blank at the start */
static int s_LastNew; /* vertical blank when a sector last arrived */
static int          s_Decoded;   /* frames decoded since boot */
static unsigned int s_DecCycles; /* EE cycles decoding, since the last report */

static int vblanks(void)
{
    unsigned int c;
    return Port_VBlanks(&c);
}

static unsigned int rd16(const unsigned char* p)
{
    return p[0] | (p[1] << 8);
}

/** Sectors the IOP has sent (written into s_Count by DMA: read past the data cache). */
static int sent(void)
{
    return (int)*(volatile unsigned int*)((unsigned int)s_Count | 0x20000000);
}

/** Uses forwarded sectors until a frame is complete (decoded into s_Rgba: s_Ready) or none are left. */
static void parse(void)
{
    int n = sent();
    if (n != s_Used)
    {
        s_LastNew = vblanks();
    }
    while (s_Used < n && !s_Ready)
    {
        /* Through the uncached mirror: the IOP's DMA wrote it behind the data cache. */
        const unsigned char* d = (const unsigned char*)((unsigned int)s_Ring[s_Used % RING] | 0x20000000);
        s_Used++;
        if (rd16(d) == 0x0160 && rd16(d + 2) == 0x8001)
        {
            int chunk = (int)rd16(d + 4), chunks = (int)rd16(d + 6);
            int no    = (int)(rd16(d + 8) | (rd16(d + 10) << 16));
            if (no != s_FrameNo)
            {
                s_FrameNo     = no;
                s_FrameChunks = chunks;
                s_FrameGot    = 0;
            }
            if (chunk < FRAME_MAX && chunks <= FRAME_MAX)
            {
                memcpy(s_Frame + chunk * PAYLOAD, d + 32, PAYLOAD);
                s_FrameGot++;
            }
            if (chunk == chunks - 1)
            {
                s_W = (int)rd16(d + 16);
                s_H = (int)rd16(d + 18);
                unsigned int t0, t1;
                int          r = -1;
                __asm__ volatile("mfc0 %0, $9" : "=r"(t0));
                if (s_FrameGot == s_FrameChunks && s_W > 0 && s_W <= W_MAX && s_H > 0 && s_H <= H_MAX)
                {
                    r = StrDec_Frame(s_Frame, s_FrameChunks * PAYLOAD, s_W, s_H, (unsigned char*)s_Rgba, 4);
                }
                __asm__ volatile("mfc0 %0, $9" : "=r"(t1));
                s_DecCycles += t1 - t0;
                if (++s_Decoded % 150 == 0)
                {
                    printf("movie: %d frames decoded, %u kcycles a frame (the last 150)\n", s_Decoded, s_DecCycles / 150 / 1000);
                    s_DecCycles = 0;
                }
                if (r >= 0)
                {
                    s_Ready   = 1;
                    s_ReadyNo = s_FrameNo;
                }
                else
                {
                    printf("movie: frame %d dropped (%d of %d sectors, %dx%d)\n", s_FrameNo, s_FrameGot, s_FrameChunks, s_W, s_H);
                }
            }
        }
    }
    if (s_Used - s_Acked >= 16)
    {
        Port_XaAck((unsigned int)s_Used);
        s_Acked = s_Used;
    }
}

/** Starts the movie at PS1 sector `sector`: its audio, and its video sectors forwarded here. */
int Port_MovieOpen(int sector, int sectors)
{
    unsigned int lsn, ps1;
    (void)sectors;
    if (!Port_CdHill(&lsn, &ps1) || sector < (int)ps1)
    {
        printf("movie: sector %d isn't in HILL.\n", sector);
        return 0;
    }
    *(volatile unsigned int*)((unsigned int)s_Count | 0x20000000) = 0;
    s_Used    = 0;
    s_Acked   = 0;
    s_FrameNo = -1;
    s_Ready   = 0;
    s_Shown   = 0;
    s_Open    = 1;
    Port_XaForward(s_Ring, RING, s_Count);
    Port_XaStart(lsn, (unsigned int)(sector - (int)ps1), 1, 1);
    s_T0      = vblanks();
    s_LastNew = s_T0;
    printf("movie: started at sector %d\n", sector);
    return 1;
}

/** Once a frame: decodes ahead and shows the next frame when its time has come. Returns the last
 * shown frame number (0 before the first), -1 when the stream has ended. */
int Port_MovieTick(void)
{
    if (!s_Open)
    {
        return -1;
    }
    parse();
    if (s_Ready && vblanks() - s_T0 >= (s_ReadyNo - 1) * VBLANKS_PER_FRAME)
    {
        Display_PresentMovie(s_Rgba, s_W, s_H, LINES);
        s_Shown = s_ReadyNo;
        s_Ready = 0;
        parse();
    }
    if (!s_Ready && sent() == s_Used && vblanks() - s_LastNew > STARVED)
    {
        return -1; /* no data for 3 s: the stream has ended */
    }
    return s_Shown;
}

void Port_MovieClose(void)
{
    if (!s_Open)
    {
        return;
    }
    Port_XaStop();
    Display_MovieEnd();
    s_Open = 0;
    printf("movie: stopped at frame %d\n", s_Shown);
}
