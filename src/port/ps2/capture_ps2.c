/** @brief Frame capture for comparing with DuckStation (tools/port/compare_frames.py). `host:capture.txt`
 * (next to the disc image) lists frames of the first attract demo (g_Demo_DemoStep values). For each,
 * writes to host:cap/: ps2_<N>_gp0.bin (every GP0 packet the frame drew: u32 word count, then the
 * words) and, at the next PutDispEnv, ps2_<N>_vram.bin (the 1024x512 VRAM, 16-bit) and
 * ps2_<N>_disp.txt (display x y w h), ps2_<N>_syswork.bin (g_SysWork, to compare game state). Without the file (or on hardware), nothing happens.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 64

static int   s_Frames[MAX_FRAMES];
static int   s_Count;
static int   s_Loaded = -1;
static int   s_Done[MAX_FRAMES];
static FILE* s_Packets;
static int   s_Open = -1; /* frame whose packets are being written */

static void load(void)
{
    char  buf[512], *tok;
    FILE* f = fopen("host:capture.txt", "r");
    int   n;
    s_Loaded = 0;
    if (!f)
    {
        return;
    }
    n = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n > 0 ? n : 0] = 0;
    for (tok = strtok(buf, " ,\t\r\n"); tok && s_Count < MAX_FRAMES; tok = strtok(NULL, " ,\t\r\n"))
    {
        s_Frames[s_Count++] = atoi(tok);
    }
    s_Loaded = s_Count > 0;
    if (s_Loaded)
    {
        printf("capture: %d frames listed\n", s_Count);
    }
}

static int index_of(int step)
{
    int i;
    for (i = 0; i < s_Count; i++)
    {
        if (s_Frames[i] == step)
        {
            return i;
        }
    }
    return -1;
}

/** A GP0 packet drawn while the first demo is at `step`. */
void Capture_Packet(int step, const unsigned int* words, int count)
{
    int i;
    if (s_Loaded < 0)
    {
        load();
    }
    if (s_Loaded <= 0 || (i = index_of(step)) < 0 || s_Done[i])
    {
        return;
    }
    if (s_Open != step)
    {
        char name[64];
        if (s_Packets)
        {
            fclose(s_Packets);
        }
        sprintf(name, "host:cap/ps2_%d_gp0.bin", step);
        s_Packets = fopen(name, "wb");
        s_Open    = step;
    }
    if (s_Packets)
    {
        unsigned int n = (unsigned int)count;
        fwrite(&n, 4, 1, s_Packets);
        fwrite(words, 4, (size_t)count, s_Packets);
    }
}

/** At PutDispEnv: finishes a frame whose packets were captured (its VRAM is now complete). */
void Capture_Display(const unsigned short* vram, int x, int y, int w, int h, const void* state, int stateSize,
                     const void* state2, int state2Size, int deltaTime)
{
    char  name[64];
    FILE* f;
    int   i;
    if (s_Open < 0 || (i = index_of(s_Open)) < 0)
    {
        return;
    }
    if (s_Packets)
    {
        fclose(s_Packets);
        s_Packets = NULL;
    }
    sprintf(name, "host:cap/ps2_%d_vram.bin", s_Open);
    if ((f = fopen(name, "wb")))
    {
        fwrite(vram, 2, 1024 * 512, f);
        fclose(f);
    }
    sprintf(name, "host:cap/ps2_%d_syswork.bin", s_Open);
    if ((f = fopen(name, "wb")))
    {
        fwrite(state, 1, (size_t)stateSize, f);
        fclose(f);
    }
    sprintf(name, "host:cap/ps2_%d_gamework.bin", s_Open);
    if ((f = fopen(name, "wb")))
    {
        fwrite(state2, 1, (size_t)state2Size, f);
        fclose(f);
    }
    sprintf(name, "host:cap/ps2_%d_disp.txt", s_Open);
    if ((f = fopen(name, "w")))
    {
        fprintf(f, "%d %d %d %d\n%d\n", x, y, w, h, deltaTime);
        fclose(f);
    }
    printf("capture: frame %d\n", s_Open);
    s_Done[i] = 1;
    s_Open    = -1;
    for (i = 0; i < s_Count && s_Done[i]; i++)
    {
    }
    if (i == s_Count)
    {
        printf("capture: all frames done\n");
    }
}

/** Whether a capture is pending at PutDispEnv (so the caller reads VRAM back first). */
int Capture_Pending(void)
{
    return s_Open >= 0;
}
