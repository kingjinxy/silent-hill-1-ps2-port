/** @brief Shows the PS1 display area on the PS2: video modes (gsKit's setup) and the display window.
 *
 * The GS renderer (gpu_gs.c) keeps the PS1's VRAM as a 16-bit image in GS memory; its display area
 * is scanned out from there, 1:1 (GpuGs_DisplayShow). 24-bit display mode (FMV) isn't handled yet.
 *
 * The video mode follows the PS1's DISPENV:
 *  - lines: 480i (interlaced, field mode) when isinter is on and the display has more than 256
 *    lines, otherwise 240p (progressive); DH is the PS1's line count.
 *  - width: the PS1's own horizontal resolution, through the GS's horizontal magnification. An NTSC
 *    line is 2560 video clocks; MAGH = 2560 / width - 1 gives 256 (9), 320 (7), 512 (4) and 640 (3)
 *    exactly; the PS1's 368 mode uses the same divider as MAGH 6 (2560 / 7 = 365.7 clocks per pixel
 *    group, as on the PS1). gsKit derives MAGH from the framebuffer width, so the DISPLAY registers
 *    are rewritten after gsKit's setup, centred on gsKit's own start coordinates.
 */

#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <gsKit.h>
#include <dmaKit.h>

#define VRAM_W 1024
#define VRAM_H 512

static GSGLOBAL* s_Gs;

static int s_Mode[3] = { -1, -1, -1 }; /* current width, lines, interlaced */
/* Frames to keep the output blank after a video mode change (GS renderer): gsKit's setup shows its
 * frame buffer's stale contents, and the first frames in a new mode can be half drawn. */
#define MODE_SWITCH_BLANK_FRAMES 3
static int s_BlankFrames;

extern void GpuGs_StateLost(void);
extern void GpuGs_Sync(void); /* gpu_gs.c: its GIF sends run in the background; gsKit uses the same channel */
extern void Port_VBlankReinstall(void); /* libetc_ps2.c */
extern void GpuGs_DisplayShow(int x, int y, int w, int h, int interlace480, int blank);


/* How a video mode change after the first is made (SH1_MODESWITCH, port_link.py):
 *  0: gsKit's full setup, as the first: a GS reset (CSR bit 9, which also stops the video signal),
 *     SetGsCrt (the BIOS reprograms the video timing), a new frame buffer;
 *  1: SetGsCrt without the GS reset, keeping the frame buffer and GS state;
 *  2: the video timing left running: only SMODE2 (interlace, field/frame) and the display window.
 *     SYNCV isn't rewritten (its values per mode aren't known; the registers can't be read), so
 *     240p keeps 480i's vertical timing or the reverse: for a hardware test only.
 * Every switch logs its setup time and the longest interval between vertical blank interrupts over
 * the next 30 frames (16.7 ms when the video signal never stopped). */
#ifndef SH_PORT_MODESWITCH
#define SH_PORT_MODESWITCH 0
#endif
extern volatile unsigned int g_PortVBlankMaxGap; /* libetc_ps2.c */
static int          s_SetupDone;    /* the first (full) setup has run */
static unsigned int s_SwitchCycles; /* the last switch's setup time */
static int          s_SwitchReport; /* frames until its report (0: none pending) */

static unsigned int cycles(void)
{
    unsigned int c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

static int magh_for(int w)
{
    switch (w)
    {
        case 256: return 9;
        case 320: return 7;
        case 368: return 6;
        case 512: return 4;
        case 640: return 3;
    }
    return 2560 / w - 1;
}

static void init(int w, int h, int interlaced)
{
    int          magh, dw, dh, dx, dy;
    unsigned int t0 = cycles();

    GpuGs_Sync();
    if (s_Gs && SH_PORT_MODESWITCH != 0)
    {
        /* Light switch: gsKit's setup stays (its frame buffer isn't used by the GS renderer). */
        s_Gs->Interlace = interlaced ? GS_INTERLACED : GS_NONINTERLACED;
        s_Gs->Field     = interlaced ? GS_FIELD : GS_FRAME;
        s_Gs->StartY    = interlaced ? 34 : 17;  /* gsKit's values for NTSC (measured) */
        s_Gs->DH        = interlaced ? 480 : 240;
        if (SH_PORT_MODESWITCH == 1)
        {
            SetGsCrt(s_Gs->Interlace, GS_MODE_NTSC, s_Gs->Field);
        }
        else
        {
            GS_SET_SMODE2(s_Gs->Interlace, s_Gs->Field, 0);
        }
        GS_SET_BGCOLOR(0, 0, 0);
        GS_SET_PMODE(0, 0, 1, 0, 0, 0xFF);
        s_BlankFrames = MODE_SWITCH_BLANK_FRAMES;
        goto window;
    }
    if (s_Gs)
    {
        gsKit_deinit_global(s_Gs);
    }
    else
    {
        dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
        dmaKit_chan_init(DMA_CHANNEL_GIF);
    }

    s_Gs                  = gsKit_init_global();
    s_Gs->Mode            = GS_MODE_NTSC;
    s_Gs->Interlace       = interlaced ? GS_INTERLACED : GS_NONINTERLACED;
    s_Gs->Field           = interlaced ? GS_FIELD : GS_FRAME;
    s_Gs->Width           = 640; /* framebuffer; the displayed width is set below */
    s_Gs->Height          = interlaced ? 480 : 240;
    s_Gs->PSM             = GS_PSM_CT16; /* only used by gsKit's setup (gpu_gs.c SNAP_ADDR) */
    s_Gs->DoubleBuffering = GS_SETTING_OFF;
    s_Gs->ZBuffering      = GS_SETTING_OFF;
    s_Gs->PrimAlphaEnable = GS_SETTING_OFF;
    gsKit_init_screen(s_Gs);
    gsKit_mode_switch(s_Gs, GS_ONESHOT);
    gsKit_set_test(s_Gs, GS_ATEST_OFF);
    gsKit_clear(s_Gs, GS_SETREG_RGBAQ(0, 0, 0, 0, 0));
    GS_SET_BGCOLOR(0, 0, 0);
    GS_SET_PMODE(0, 0, 1, 0, 0, 0xFF); /* output off until the first frames in this mode are shown */
    s_BlankFrames = MODE_SWITCH_BLANK_FRAMES;

window:
    /* PS1 width and line count, centred within gsKit's display area. */
    magh = magh_for(w);
    dw   = w * (magh + 1);
    dh   = h;
    dx   = s_Gs->StartX + s_Gs->StartXOffset + (s_Gs->DW - dw) / 2;
    dy   = s_Gs->StartY + s_Gs->StartYOffset + (s_Gs->DH - dh) / 2;
    GS_SET_DISPLAY1(dx, dy, magh, 0, dw - 1, dh - 1);
    GS_SET_DISPLAY2(dx, dy, magh, 0, dw - 1, dh - 1);

    GpuGs_StateLost();       /* gsKit's setup rewrote GS registers */
    Port_VBlankReinstall();  /* ... and dropped our vertical blank handler */

    s_Mode[0] = w;
    s_Mode[1] = h;
    s_Mode[2] = interlaced;
    printf("display: %dx%d%s (MAGH %d)\n", w, h, interlaced ? "i" : "p", magh);
    if (s_SetupDone)
    {
        s_SwitchCycles     = cycles() - t0;
        g_PortVBlankMaxGap = 0;
        s_SwitchReport     = 30;
    }
    s_SetupDone = 1;
}

/** Sets up a default mode (320x240p) if nothing was shown yet: the GS renderer draws before the game's
 * first PutDispEnv. */
void Display_EnsureInit(void)
{
    if (!s_Gs)
    {
        init(320, 240, 0);
    }
}

/* Debug (build with SH1_DUMP=1): frames whose number is listed here are also written to
 * host:frame_<n>.ppm (PCSX2 with HostFs on writes them next to the ELF; tools/port/pcsx2_run.py
 * enables it). Off by default: each dump stalls the game for about a second of emulated time, which
 * spoils frame rate measurements. Display_RequestDump (host:input.txt "dump") works either way. */
#ifdef SH_PORT_DUMP_FRAMES
static const int DUMP_FRAMES[] = { 30, 120, 200, 300, 600, 960, 1200, 2400 };
#else
static const int DUMP_FRAMES[] = { -1 };
#endif
static int s_Frame;
static int s_DumpRequested; /* Display_RequestDump: dump the next frame */

void Display_RequestDump(void)
{
    s_DumpRequested = 1;
}

/* Frame dumps read the GS's VRAM back in bands of DUMP_BAND rows (no full copy is kept). */
#define DUMP_BAND 32
static unsigned short s_DumpBand[DUMP_BAND * VRAM_W] __attribute__((aligned(64)));
extern void GpuGs_StoreImage(int x, int y, int w, int h, unsigned short* dst); /* gpu_gs.c */

/** Writes VRAM's rectangle (x, y, w, h) to host:<what>_<frame>.ppm. */
static void dump_rect(const char* what, int x, int y, int w, int h)
{
    char  name[64];
    FILE* f;
    int   row, i;
    sprintf(name, "host:%s_%04d.ppm", what, s_Frame);
    f = fopen(name, "wb");
    if (!f)
    {
        printf("display: can't open %s\n", name);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (row = 0; row < h; row += DUMP_BAND)
    {
        int n = h - row < DUMP_BAND ? h - row : DUMP_BAND;
        GpuGs_StoreImage(x, y + row, w, n, s_DumpBand);
        for (i = 0; i < w * n; i++)
        {
            unsigned short c = s_DumpBand[i];
            unsigned char  rgb[3];
            rgb[0] = (unsigned char)((c & 31) << 3);
            rgb[1] = (unsigned char)(((c >> 5) & 31) << 3);
            rgb[2] = (unsigned char)(((c >> 10) & 31) << 3);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    printf("display: wrote %s (%dx%d)\n", name, w, h);
}

int Display_FrameCount(void)
{
    return s_Frame;
}

/** GS renderer: the display area is already in GS memory (gpu_gs.c) and is shown from there
 * (GpuGs_DisplayShow). */
void Display_PresentGs(int x, int y, int w, int h, int rgb24, int isinter)
{
    unsigned int k;
    int interlaced = isinter && h > 256;
    {
        extern void Port_InputCrashCheck(void); /* input_ps2.c: "crash" test event */
        Port_InputCrashCheck();
    }

    if (w <= 0 || h <= 0)
    {
        return;
    }
    if (w > 640) w = 640;
    if (h > 480) h = 480;
    if (w != s_Mode[0] || h != s_Mode[1] || interlaced != s_Mode[2])
    {
        init(w, h, interlaced);
    }
    if (rgb24)
    {
        static int warned;
        if (!warned++)
        {
            printf("display: 24-bit display mode not supported yet\n");
        }
        return;
    }

    s_Frame++;
    if (s_Frame == 1 || s_Frame % 60 == 0)
    {
        printf("display: frame %d, %dx%d at (%d,%d)\n", s_Frame, w, h, x, y);
    }
    for (k = 0; k <= sizeof(DUMP_FRAMES) / sizeof(DUMP_FRAMES[0]); k++)
    {
        int wanted = k < sizeof(DUMP_FRAMES) / sizeof(DUMP_FRAMES[0]) ? DUMP_FRAMES[k] == s_Frame : s_DumpRequested;
        if (k == sizeof(DUMP_FRAMES) / sizeof(DUMP_FRAMES[0]))
        {
            s_DumpRequested = 0;
        }
        if (!wanted)
        {
            continue;
        }
        dump_rect("frame", x, y, w, h);
        dump_rect("vram", 0, 0, VRAM_W, VRAM_H);
    }

    if (s_SwitchReport > 0 && --s_SwitchReport == 0)
    {
        printf("display: mode switch (method %d): setup %u us, longest vertical blank interval %u us over 30 frames "
               "(16683 when the signal never stopped)\n", SH_PORT_MODESWITCH, s_SwitchCycles / 295, g_PortVBlankMaxGap / 295);
    }
    GpuGs_DisplayShow(x, y, w, h, interlaced, s_BlankFrames > 0);
    if (s_BlankFrames > 0)
    {
        s_BlankFrames--;
    }
}
