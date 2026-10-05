/** @brief Shows the PS1 display area (from the emulated VRAM) on the PS2 via the GS, using gsKit.
 *
 * The display rectangle is copied out of VRAM and uploaded as a PSMCT16 texture each frame: PS1
 * 15-bit pixels (R 0-4, G 5-9, B 10-14, mask bit 15) have the same layout as the GS's 16-bit format.
 * It's drawn as one full-screen sprite, without alpha test or blending (bit 15 is the PS1 mask bit,
 * not transparency here). 24-bit display mode (FMV) isn't handled yet.
 *
 * The output mode follows the PS1's: 480i (640x448, interlaced field mode) when the game's DISPENV
 * has interlace on and more than 256 lines, otherwise 240p (640x240, progressive). The PS1's
 * horizontal resolutions (256-640) are scaled to the 640-pixel output.
 */

#include <stdio.h>
#include <string.h>
#include <gsKit.h>
#include <dmaKit.h>

#define VRAM_W 1024
#define VRAM_H 512

static GSGLOBAL* s_Gs;
static GSTEXTURE s_Tex;
static unsigned short s_Buffer[VRAM_H * VRAM_W] __attribute__((aligned(128)));

static int s_Interlaced = -1; /* current output mode: 0 = 240p, 1 = 480i */

static void init(int interlaced)
{
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
    s_Gs->Width           = 640;
    s_Gs->Height          = interlaced ? 448 : 240;
    s_Gs->PSM             = GS_PSM_CT32;
    s_Gs->DoubleBuffering = GS_SETTING_OFF;
    s_Gs->ZBuffering      = GS_SETTING_OFF;
    s_Gs->PrimAlphaEnable = GS_SETTING_OFF;
    gsKit_init_screen(s_Gs);
    gsKit_mode_switch(s_Gs, GS_ONESHOT);
    gsKit_set_test(s_Gs, GS_ATEST_OFF);

    s_Tex.PSM    = GS_PSM_CT16;
    s_Tex.Mem    = (u32*)s_Buffer;
    s_Tex.Filter = GS_FILTER_LINEAR;
    s_Tex.Vram   = gsKit_vram_alloc(s_Gs, gsKit_texture_size(VRAM_W, VRAM_H, GS_PSM_CT16), GSKIT_ALLOC_USERBUFFER);

    s_Interlaced = interlaced;
    printf("display: %s\n", interlaced ? "480i (640x448 interlaced)" : "240p (640x240 progressive)");
}

/* Debug: frames whose number is listed here are also written to host:frame_<n>.ppm (PCSX2 with
 * HostFs on writes them next to the ELF; tools/port/pcsx2_run.py enables it). */
static const int DUMP_FRAMES[] = { 30, 120, 300, 600, 900, 1200 };
static int s_Frame;

static void dump_vram(const unsigned short* vram)
{
    char  name[64];
    FILE* f;
    int   i;
    sprintf(name, "host:vram_%04d.ppm", s_Frame);
    f = fopen(name, "wb");
    if (!f)
    {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", VRAM_W, VRAM_H);
    for (i = 0; i < VRAM_W * VRAM_H; i++)
    {
        unsigned short c = vram[i];
        unsigned char  rgb[3];
        rgb[0] = (unsigned char)((c & 31) << 3);
        rgb[1] = (unsigned char)(((c >> 5) & 31) << 3);
        rgb[2] = (unsigned char)(((c >> 10) & 31) << 3);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void dump(int w, int h)
{
    char  name[64];
    FILE* f;
    int   i;
    sprintf(name, "host:frame_%04d.ppm", s_Frame);
    f = fopen(name, "wb");
    if (!f)
    {
        printf("display: can't open %s\n", name);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++)
    {
        unsigned short c = s_Buffer[i];
        unsigned char  rgb[3];
        rgb[0] = (unsigned char)((c & 31) << 3);
        rgb[1] = (unsigned char)(((c >> 5) & 31) << 3);
        rgb[2] = (unsigned char)(((c >> 10) & 31) << 3);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("display: wrote %s (%dx%d)\n", name, w, h);
}

void Display_Present(const unsigned short* vram, int x, int y, int w, int h, int rgb24, int isinter)
{
    int row;
    unsigned int k;
    int interlaced = isinter && h > 256;

    if (w <= 0 || h <= 0)
    {
        return;
    }
    if (interlaced != s_Interlaced)
    {
        init(interlaced);
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
    if (w > VRAM_W) w = VRAM_W;
    if (h > VRAM_H) h = VRAM_H;

    for (row = 0; row < h; row++)
    {
        int sy = (y + row) & (VRAM_H - 1);
        if (x + w <= VRAM_W)
        {
            memcpy(&s_Buffer[row * w], &vram[sy * VRAM_W + x], w * 2);
        }
        else
        {
            int col;
            for (col = 0; col < w; col++)
            {
                s_Buffer[row * w + col] = vram[sy * VRAM_W + ((x + col) & (VRAM_W - 1))];
            }
        }
    }

    s_Frame++;
    if (s_Frame == 1 || s_Frame % 60 == 0)
    {
        printf("display: frame %d, %dx%d at (%d,%d)\n", s_Frame, w, h, x, y);
    }
    for (k = 0; k < sizeof(DUMP_FRAMES) / sizeof(DUMP_FRAMES[0]); k++)
    {
        if (DUMP_FRAMES[k] == s_Frame)
        {
            dump(w, h);
            dump_vram(vram);
        }
    }

    s_Tex.Width  = w;
    s_Tex.Height = h;
    gsKit_texture_upload(s_Gs, &s_Tex);
    gsKit_prim_sprite_texture(s_Gs, &s_Tex, 0.0f, 0.0f, 0.0f, 0.0f, (float)s_Gs->Width, (float)s_Gs->Height,
                              (float)w, (float)h, 2, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00));
    gsKit_queue_exec(s_Gs);
}
