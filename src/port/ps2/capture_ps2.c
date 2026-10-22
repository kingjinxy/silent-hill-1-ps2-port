/** @brief Frame capture for comparing with DuckStation (tools/port/compare_frames.py). `host:capture.txt`
 * (next to the disc image) lists frames of an attract demo (g_Demo_DemoStep values), plus optional
 * `demo=N` (the Nth demo played, from 1; default 1) and `mode=N` (the Debug Options rendering bits
 * forced, g_PortHiresOption: 1 2x, 2 32-bit, 4 480p; default: the saved options). With `load=F:D`,
 * the save is loaded instead, file F's Data D on card 1 through Load Game (title.c, saveload.c), and frames are vertical blanks of gameplay
 * (g_SysWork.gameStateCounter in GameState_InGame), each captured at the first frame reaching it.
 * `walk` (with load=): Harry turns 90 degrees right and walks 1.5 s first (joy.c), and frames count
 * from the walk's end. `replay=N` (one or more) instead replays DuckStation's capture of frame N through the GS renderer:
 * host:cap/ps1_N_vram.bin loaded into VRAM, ps1_N_gp0.bin's packets drawn, and VRAM written to
 * ps2r_N_vram.bin (the renderer alone, without game timing differences). For each,
 * writes to host:cap/: ps2_<N>_gp0.bin (every GP0 packet the frame drew: u32 word count, then the
 * words) and, at the next PutDispEnv, ps2_<N>_vram.bin (the 1024x512 VRAM, 16-bit) and
 * ps2_<N>_disp.txt (display x y w h), ps2_<N>_syswork.bin (g_SysWork, to compare game state). Without the file (or on hardware), nothing happens.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#define MAX_FRAMES 64

extern void GpuGs_Commands(const unsigned int* w, int n);                          /* gpu_gs.c */
extern void GpuGs_LoadImage(int x, int y, int w, int h, const unsigned short* src, int stride);
extern void GpuGs_Flush(void);
extern void GpuGs_Sync(void);

static int   s_Frames[MAX_FRAMES];
static int   s_Count;
static int   s_Loaded = -1;
static int   s_Done[MAX_FRAMES];
static FILE* s_Packets;
static int   s_Open = -1; /* frame whose packets are being written */
static int   s_Demo = 1;  /* which demo (1: the first played) */
static int   s_Mode = -1; /* rendering option bits forced, -1 none */
static int   s_LoadSave;  /* load a save instead of a demo */
static int   s_LoadFile, s_LoadData; /* its save: File N, Data N as the Load screen numbers them */
static int   s_Replay[MAX_FRAMES], s_ReplayCount; /* DuckStation frames to replay */
static int   s_StateA = -1, s_StateB = -1; /* `state=A:B`: game state of demo steps A-B (Capture_State) */
static FILE* s_StateFile;
static unsigned int s_DumpAddr[64], s_DumpSize[64]; /* `dump=ADDR:SIZE` (hex): globals appended to each state record */
static int          s_DumpCount;
static int   s_Walk, s_WalkEnd = -1;
static int   s_HoldStep[64], s_HoldFrames[64], s_HoldCount; /* `hold=S:N`: demo step S lasts N frames (PS1 load timing) */ /* `walk`: frames counted from the end of joy.c's walk */


static void load(void)
{
    static char buf[4096];
    char*       tok;
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
        if (!strncmp(tok, "demo=", 5))
        {
            s_Demo = atoi(tok + 5);
        }
        else if (!strncmp(tok, "load=", 5))
        {
            s_LoadSave = 1;
            sscanf(tok + 5, "%d:%d", &s_LoadFile, &s_LoadData);
        }
        else if (!strncmp(tok, "dump=", 5) && s_DumpCount < 64)
        {
            sscanf(tok + 5, "%x:%x", &s_DumpAddr[s_DumpCount], &s_DumpSize[s_DumpCount]);
            s_DumpCount++;
        }
        else if (!strncmp(tok, "state=", 6))
        {
            sscanf(tok + 6, "%d:%d", &s_StateA, &s_StateB);
        }
        else if (!strncmp(tok, "hold=", 5) && s_HoldCount < 64)
        {
            sscanf(tok + 5, "%d:%d", &s_HoldStep[s_HoldCount], &s_HoldFrames[s_HoldCount]);
            s_HoldCount++;
        }
        else if (!strcmp(tok, "walk"))
        {
            s_Walk = 1;
        }
        else if (!strncmp(tok, "replay=", 7))
        {
            if (s_ReplayCount < MAX_FRAMES)
            {
                s_Replay[s_ReplayCount++] = atoi(tok + 7);
            }
        }
        else if (!strncmp(tok, "mode=", 5))
        {
            s_Mode = atoi(tok + 5);
        }
        else
        {
            s_Frames[s_Count++] = atoi(tok);
        }
    }
    s_Loaded = s_Count > 0 || s_ReplayCount > 0 || s_StateA >= 0;
    if (s_Loaded)
    {
        printf("capture: %d frames listed, %s %d, mode %d\n", s_Count, s_LoadSave ? "load" : "demo", s_Demo, s_Mode);
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

/** The demo to capture (1: the first played), 0 without a capture file. */
int Capture_Demo(void)
{
    if (s_Loaded < 0)
    {
        load();
    }
    return s_Loaded > 0 ? s_Demo : 0;
}

/** Whether the capture loads a save (title.c, saveload.c), and which. */
int Capture_Load(int* file, int* data)
{
    if (s_Loaded < 0)
    {
        load();
    }
    if (file)
    {
        *file = s_LoadFile;
        *data = s_LoadData;
    }
    return s_Loaded > 0 && s_LoadSave;
}

/** At PutDispEnv during the captured demo: with `state=A:B`, appends step, RNG seed, g_DeltaTime, g_SysWork and
 * g_GameWork to host:cap/ps2_state.bin for demo steps A-B (finding where the game first diverges). */
void Capture_State(int step, unsigned int seed, int deltaTime, const void* state, int stateSize, const void* state2, int state2Size)
{
    static int done;
    if (s_Loaded <= 0 || s_StateA < 0 || done || step < s_StateA)
    {
        return;
    }
    if (step > s_StateB)
    {
        if (s_StateFile)
        {
            fclose(s_StateFile);
            s_StateFile = NULL;
        }
        done = 1;
        printf("capture: all frames done\n");
        return;
    }
    if (!s_StateFile)
    {
        s_StateFile = fopen("host:cap/ps2_state.bin", "wb");
    }
    if (s_StateFile)
    {
        fwrite(&step, 4, 1, s_StateFile);
        fwrite(&seed, 4, 1, s_StateFile);
        fwrite(&deltaTime, 4, 1, s_StateFile);
        fwrite(state, 1, (size_t)stateSize, s_StateFile);
        fwrite(state2, 1, (size_t)state2Size, s_StateFile);
        int i;
        for (i = 0; i < s_DumpCount; i++)
        {
            fwrite((const void*)(unsigned long)s_DumpAddr[i], 1, s_DumpSize[i], s_StateFile);
        }
    }
}

/** Demo playback with the PS1's load timing (`hold=S:N`, compare_frames.py --ps1-holds): whether demo
 * step `step`, `frames` frames into it, is still held (the PS1 spent N frames on it, loading). */
int Capture_DemoHold(int step, int frames)
{
    extern int Capture_DemoCurrent(void); /* libgpu_port.c: the demo playing (1: the first) */
    int        i;
    if (s_Loaded <= 0 || Capture_DemoCurrent() != s_Demo)
    {
        return 0;
    }
    for (i = 0; i < s_HoldCount; i++)
    {
        if (s_HoldStep[i] == step)
        {
            return frames + 4 < s_HoldFrames[i]; /* the step also lasts the frames after the load (measured: 4) */
        }
    }
    return 0;
}

/** Whether the capture walks first (joy.c), and its end (gameplay vertical blank). */
int Capture_Walk(void)
{
    if (s_Loaded < 0)
    {
        load();
    }
    return s_Loaded > 0 && s_Walk;
}

void Capture_WalkEnd(int vblank)
{
    s_WalkEnd = vblank;
}

/** Save captures: the listed frame a frame at gameplay vertical blank `vblank` captures (the
 * first not yet captured that it has reached), -1 none. */
int Capture_LoadStep(int vblank)
{
    int i;
    if (s_Walk)
    {
        if (s_WalkEnd < 0)
        {
            return -1;
        }
        vblank -= s_WalkEnd;
    }
    for (i = 0; i < s_Count; i++)
    {
        if (!s_Done[i] && s_Frames[i] <= vblank && (s_Open < 0 || s_Open == s_Frames[i]))
        {
            return s_Frames[i];
        }
    }
    return -1;
}

/** The rendering option bits forced for the capture, -1 none. */
int Capture_Mode(void)
{
    if (s_Loaded < 0)
    {
        load();
    }
    return s_Loaded > 0 ? s_Mode : -1;
}

/** A GP0 packet drawn while the captured demo is at `step`. */
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
extern void GpuGs_StoreImage(int x, int y, int w, int h, unsigned short* dst); /* gpu_gs.c */

void Capture_Display(int x, int y, int w, int h, const void* state, int stateSize,
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
        /* The GS's VRAM, read back in bands (no full copy is kept). */
        static unsigned short band[32 * 1024] __attribute__((aligned(64)));
        int row;
        for (row = 0; row < 512; row += 32)
        {
            GpuGs_StoreImage(0, row, 1024, 32, band);
            fwrite(band, 2, 32 * 1024, f);
        }
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
    sprintf(name, "host:cap/ps2_%d_envwork.bin", s_Open);
    if ((f = fopen(name, "wb")))
    {
        extern unsigned char g_WorldEnvWork[]; /* s_WorldEnvWork: the world and flashlight lighting */
        fwrite(g_WorldEnvWork, 1, 0x150, f);
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

static void* read_file(const char* name, int* size)
{
    FILE* f = fopen(name, "rb");
    void* buf;
    if (!f)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *size = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = memalign(64, (size_t)*size);
    if (buf)
    {
        fread(buf, 1, (size_t)*size, f);
    }
    fclose(f);
    return buf;
}

/** At PutDispEnv: the replays (`replay=N`), once, after `frame` frames (the GS set up). */
void Capture_Replay(int frame)
{
    extern void GpuGs_StoreImage(int x, int y, int w, int h, unsigned short* dst); /* gpu_gs.c */
    static int done;
    int        i, size, gsize, pos, row;
    char       name[64];
    if (s_Loaded < 0)
    {
        load();
    }
    if (done || s_ReplayCount == 0 || frame < 60)
    {
        return;
    }
    done = 1;
    for (i = 0; i < s_ReplayCount; i++)
    {
        unsigned short* vram;
        unsigned int*   gp0;
        FILE*           f;
        sprintf(name, "host:cap/ps1_%d_vram.bin", s_Replay[i]);
        vram = read_file(name, &size);
        sprintf(name, "host:cap/ps1_%d_gp0.bin", s_Replay[i]);
        gp0 = read_file(name, &gsize);
        if (!vram || !gp0 || size != 1024 * 512 * 2)
        {
            printf("capture: replay %d: capture missing\n", s_Replay[i]);
            free(vram);
            free(gp0);
            continue;
        }
        GpuGs_LoadImage(0, 0, 1024, 512, vram, 1024);
        for (pos = 0; pos + 1 <= gsize / 4;)
        {
            int n = (int)gp0[pos];
            GpuGs_Commands(gp0 + pos + 1, n);
            pos += 1 + n;
        }
        GpuGs_Flush();
        GpuGs_Sync();
        sprintf(name, "host:cap/ps2r_%d_vram.bin", s_Replay[i]);
        if ((f = fopen(name, "wb")))
        {
            for (row = 0; row < 512; row += 32)
            {
                GpuGs_StoreImage(0, row, 1024, 32, vram + row * 1024);
            }
            fwrite(vram, 2, 1024 * 512, f);
            fclose(f);
        }
        printf("capture: replay %d (%d words)\n", s_Replay[i], gsize / 4);
        free(vram);
        free(gp0);
    }
    printf("capture: all frames done\n");
}

/** Whether a capture is pending at PutDispEnv (so the caller reads VRAM back first). */
int Capture_Pending(void)
{
    return s_Open >= 0;
}
