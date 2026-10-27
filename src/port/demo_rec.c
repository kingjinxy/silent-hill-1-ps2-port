/** @brief Play recorder: record a session on the console, replay it exactly anywhere (PCSX2, the
 * console), for performance runs on real routes.
 *
 * Built on the game's attract demo player (src/bodyprog/demo.c), whose replays are exact: a recording
 * is an `s_DemoState` (options, savegame, RNG seed) and one `s_DemoPlaybackFrame` per demo step
 * (pad, expected game state, the step's vertical blanks, RNG seed). Recording runs the game in the
 * same demo mode as playback (SysFlag_DemoActive: the RNG reseeds, the fixed time step, neutral pad
 * outside steps), only taking the pad from the controller and writing the frames instead of reading
 * them; so a recording plays the same frames as the session it came from.
 *
 * On the console, `tools/port/ps2_ctl.py rec NAME` arms recording (`rec stop` stops it): the host:
 * files below don't exist there. Every recording is also streamed through the console log as
 * `recdata NAME OFFSET HEX` lines (a second's worth at a time, so a crash loses at most a second) and
 * `recend NAME SIZE`; tools/port/rec_extract.py rebuilds rec/NAME.dem from build/port/ps2.log or a
 * PCSX2 log.
 *
 * `host:rec.txt` (src/port/ps2/rec_ps2.c): `record NAME` starts recording when a save is loaded
 * (SaveScreen_Continue), until Select + Start + L1 + R1 are held, the save screen, a game over or
 * 18 minutes; it's written to host:rec/NAME.dem (also every minute). `play NAME` replays it from the
 * next save loaded (which save doesn't matter: the recording's replaces it; PCSX2 runs: capture.txt
 * `999999 load=F:D`), then prints the frame rate and gives control back.
 *
 * Console replay: the title's REPLAY entry (options.c, s_PortReplay) shows the Debug Options settings;
 * Start there waits for a recording. `tools/port/ps2_ctl.py replay NAME` uploads rec/NAME.dem
 * (Port_RecUpload, a chunk at a time) and starts it (Port_RecRemotePlay): the game starts from the
 * recording's save (Port_RecReplayBegin, as SaveScreen_Continue), with the REPLAY screen's settings in
 * place of the recording's Debug Options. In PCSX2, Start on that screen plays host:rec.txt's `play NAME`.
 *
 * File loads: the step at which each file finishes loading (its post-processing, which is when the game
 * sees it) is recorded (field_A[1]: the queue's postLoad index at the step, % 255 + 1) and held to on
 * playback (Port_RecFileSync, Port_RecFileHold): a file landing one step later started an enemy
 * differently (docks1, step 1844) and the run went elsewhere. 0: not recorded (older recordings).
 *
 * Disc timing: a chunk load's length in steps is recorded (field_A[0]) and held to on playback (waited
 * for when slower). Other waits on the disc or on voice clips aren't: a divergence shows as STEP
 * ERROR, which stops the replay.
 */

#include "game.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/demo.h"
#include "bodyprog/game_boot/game_boot.h"
#include "bodyprog/memcard.h"
#include "bodyprog/sys/joy.h"
#include "main/fsqueue.h"
#include "main/rng.h"
#include "port/demo_rec.h"

#include <string.h>

extern int Rec_Config(char* name);                            /* src/port/ps2/rec_ps2.c */
extern int Rec_Read(const char* name, void* buf, int max);
extern int Rec_Write(const char* name, const void* buf, int size);
extern s32 Port_DemoStepFrames(void);                         /* demo.c */
extern int Display_FrameCount(void);                          /* src/port/ps2/display_ps2.c */

#define REC_STEPS (1 << 16) /* 18 minutes at 60 fps */
#define REC_FLUSH 3600      /* steps between writes while recording */
#define REC_STOP  (ControllerFlag_Select | ControllerFlag_Start | ControllerFlag_L1 | ControllerFlag_R1)

static struct
{
    s_DemoState         state;
    s_DemoPlaybackFrame frames[REC_STEPS];
} s_Rec;

static int  s_Mode = -1; /* 0 none, 1 record, 2 play */
static char s_Name[32];
static int  s_Active, s_Steps, s_Waits;
static int  s_Frame0, s_VBlank0;
static int  s_Sent;                 /* bytes of the recording streamed to the log */
static volatile int s_Remote;       /* armed by ps2_ctl.py: no host: files */
static volatile int s_StopRequest;  /* ps2_ctl.py rec stop */
static volatile int s_Uploaded;     /* bytes of s_Rec received from ps2_ctl.py replay (Port_RecUpload) */
static volatile int s_RemotePlay;   /* ps2_ctl.py replay: s_Rec holds the recording, start it */
int                 g_PortReplayMenu;
int                 g_PortReplayBits = -1;
static int          s_Override = -1; /* Debug Options rendering bits replacing the recording's (REPLAY screen) */

static int mode(void)
{
    if (s_Mode < 0)
    {
        s_Mode = Rec_Config(s_Name);
    }
    return s_Mode;
}

static u16 hold_get(int step)
{
    return (u8)s_Rec.frames[step].field_A[0];
}

/** The file queue's post-load index as stored per step: 1..255 (0: none recorded). */
static u8 file_mark(void)
{
    return (u8)(g_FsQueue.postLoad.idx % 255 + 1);
}

/** How far the queue's post-loads are past step `step`'s recorded mark (negative: behind); 0 without one. */
static int file_delta(int step)
{
    int r = (u8)s_Rec.frames[step].field_A[1], d;
    if (r == 0)
    {
        return 0;
    }
    d = ((int)file_mark() - r) % 255;
    return d > 127 ? d - 255 : d < -127 ? d + 255 : d;
}

/** Demo step start: records the file queue's post-load mark, or on playback waits for the files the
 * recording had loaded by this step (more can't have loaded: Port_RecFileHold). */
static void file_sync(void)
{
    int step = g_Demo_DemoStep, n = 0;
    if (!s_Active || step >= s_Steps)
    {
        return;
    }
    if (s_Mode == 1)
    {
        s_Rec.frames[step].field_A[1] = (s8)file_mark();
        return;
    }
    if (file_delta(step) < 0)
    {
        s_Waits++;
        while (file_delta(step) < 0 && n++ < 600)
        {
            VSync(SyncMode_Wait);
            Fs_QueueUpdate();
        }
        if (file_delta(step) < 0)
        {
            printf("rec: step %d: file loads still behind the recording after 10 s\n", step);
        }
    }
}

/** Fs_QueueUpdate: 1 to hold a file's post-processing on playback (the recording hadn't loaded it yet
 * at the next step). */
int Port_RecFileHold(void)
{
    int step = g_Demo_DemoStep;
    return s_Active && s_Mode == 2 && step < s_Steps && s_Rec.frames[step].field_A[1] != 0 && file_delta(step) >= 0 &&
           g_FsQueue.postLoadState == FsQueuePostLoadState_Init; /* between files, never part-way through one */
}

/** Streams the recording's bytes up to `end` through the log. */
static void stream(int end)
{
    static const char HEX[] = "0123456789ABCDEF";
    char              line[300];
    int               n, i;
    while (s_Sent < end)
    {
        n = MIN(end - s_Sent, 128);
        for (i = 0; i < n; i++)
        {
            u8 b            = ((u8*)&s_Rec)[s_Sent + i];
            line[i * 2]     = HEX[b >> 4];
            line[i * 2 + 1] = HEX[b & 15];
        }
        line[n * 2] = 0;
        printf("recdata %s %d %s\n", s_Name, s_Sent, line);
        s_Sent += n;
    }
}

static void write_out(int steps)
{
    stream(sizeof(s_DemoState) + steps * sizeof(s_DemoPlaybackFrame));
    if (s_Remote)
    {
        return;
    }
    s_Rec.state.frameCount = steps;
    if (Rec_Write(s_Name, &s_Rec, sizeof(s_DemoState) + steps * sizeof(s_DemoPlaybackFrame)) != 0)
    {
        printf("rec: writing host:rec/%s.dem failed\n", s_Name);
    }
}

static void stop(const char* why)
{
    int frames = Display_FrameCount() - s_Frame0, vbs = VSync(SyncMode_Count) - s_VBlank0;
    if (s_Mode == 1)
    {
        write_out(g_Demo_DemoStep);
        printf("recend %s %d\n", s_Name, s_Sent);
        printf("rec: recorded %s: %d steps (%s)\n", s_Name, (int)g_Demo_DemoStep, why);
    }
    else
    {
        printf("rec: replay %s finished at step %d of %d (%s): %d frames in %d vertical blanks = %d.%02d fps, %d load waits\n",
               s_Name, (int)g_Demo_DemoStep, s_Steps, why, frames, vbs, frames * 5994 / (vbs > 0 ? vbs : 1) / 100,
               frames * 5994 / (vbs > 0 ? vbs : 1) % 100, s_Waits);
    }
    s_Active                   = 0;
    s_Mode                     = 0; /* once per boot */
    g_SysWork.sysFlags        &= ~SysFlag_DemoActive;
    g_Demo_ActivePlaybackFrame = NULL;
    g_Demo_DemoStep            = 0;
}

/** SaveScreen_Continue, before the loaded save is used: starts recording or playback. */
void Port_RecContinue(void)
{
    int n;
    if (mode() == 0 || s_Active)
    {
        return;
    }
    if (s_Mode == 1)
    {
        memset(&s_Rec, 0, sizeof(s_Rec));
        s_Rec.state.config   = g_GameWork.config;
        s_Rec.state.savegame = g_GameWork.savegame;
        s_Rec.state.randSeed = (u16)Rng_GetSeed();
        s_Rec.state.frameCount = ~0u; /* streamed before the end is known; rec_extract.py sets it */
        s_Steps              = REC_STEPS;
        s_Sent               = 0;
        s_StopRequest        = 0;
        stream(sizeof(s_DemoState));
    }
    else
    {
        n = s_RemotePlay ? s_Uploaded : Rec_Read(s_Name, &s_Rec, sizeof(s_Rec));
        s_RemotePlay = 0;
        if (n < (int)sizeof(s_DemoState))
        {
            printf("rec: host:rec/%s.dem not found\n", s_Name);
            s_Mode = 0;
            return;
        }
        s_Steps              = MIN((int)s_Rec.state.frameCount, (n - (int)sizeof(s_DemoState)) / (int)sizeof(s_DemoPlaybackFrame));
        g_GameWork.config    = s_Rec.state.config;
        g_GameWork.savegame  = s_Rec.state.savegame;
        if (s_Override >= 0)
        {
            g_GameWork.config.unused_26 = (g_GameWork.config.unused_26 & ~7) | s_Override;
        }
    }
    g_Demo_RandSeed             = s_Rec.state.randSeed;
    g_Demo_PrevRandSeed         = Rng_GetSeed();
    g_Demo_OptionsConfigBackup  = g_GameWork.config;
    g_Demo_PlaybackFrames       = s_Rec.frames;
    g_Demo_ActivePlaybackFrame  = NULL;
    g_Demo_DemoStep             = 0;
    g_SysWork.sysFlags         |= SysFlag_DemoActive;
    s_Active                    = 1;
    s_Waits                     = 0;
    s_Frame0                    = Display_FrameCount();
    s_VBlank0                   = VSync(SyncMode_Count);
    if (s_Mode == 1)
    {
        printf("rec: recording %s (stop: Select + Start + L1 + R1)\n", s_Name);
    }
    else
    {
        printf("rec: replaying %s (%d steps; Debug Options rendering bits %d%s)\n", s_Name, s_Steps,
               g_GameWork.config.unused_26 & 7, s_Override >= 0 ? ", from the REPLAY screen" : "");
    }
}

int Port_RecActive(void)
{
    return s_Active;
}

int Port_RecSteps(void)
{
    return s_Steps;
}

/** Demo_ControllerDataUpdate while recording: the controller stays at a step (Port_RecPad records it). */
int Port_RecController(void)
{
    if (!s_Active || s_Mode != 1)
    {
        return 0;
    }
    g_Demo_FrameCount = 0;
    if (g_Demo_ActivePlaybackFrame != NULL)
    {
        return 1;
    }
    // As playback with an idle pad (Demo_ControllerDataUpdate).
    *(u16*)&g_Controller0->analogController.status = 0x7300;
    g_Controller0->analogController.buttonFlags    = ControllerFlag_FaceButtons;
    *(u32*)&g_Controller0->analogController.rightX = 0x80808080;
    return 1;
}

/** Joy_ControllerDataUpdate, after test input (joy.c walk): the step's pad, as the game uses it. */
void Port_RecPad(void)
{
    if (!s_Active || s_Mode != 1 || g_Demo_ActivePlaybackFrame == NULL)
    {
        return;
    }
    g_Demo_ActivePlaybackFrame->analogController = g_Controller0->analogController;
    if (!(g_Controller0->analogController.buttonFlags & REC_STOP))
    {
        stop("stop buttons");
    }
    else if (s_StopRequest)
    {
        stop("ps2_ctl.py rec stop");
    }
}

/** Demo_Update at a demo step while recording: the step's frame. */
int Port_RecStep(s32 gameState)
{
    s_DemoPlaybackFrame* frame;
    file_sync();
    if (s_Active)
    {
        // Dense trace (same when recording and replaying): the first step that differs, and the system
        // state (events, doors) around it.
        static s32 lastSysState = -1;
        if (g_Demo_DemoStep % 30 == 0 || g_SysWork.sysState != lastSysState)
        {
            printf("rec step %d: sys %d/%d, Harry %d %d %d hp %d, delta %d, seed %08X, files %d queued, read %d\n",
                   (int)g_Demo_DemoStep, (int)g_SysWork.sysState, (int)g_SysWork.sysStateSteps[0],
                   g_SysWork.playerWork.player.position.vx, g_SysWork.playerWork.player.position.vy,
                   g_SysWork.playerWork.player.position.vz, (int)g_SysWork.playerWork.player.health, (int)g_DeltaTime,
                   (unsigned)Rng_GetSeed(), Fs_QueueGetLength(), (int)g_FsQueue.read.idx);
            lastSysState = g_SysWork.sysState;
        }
    }
    if (s_Active && g_Demo_DemoStep % 600 == 0)
    {
        // Same lines when recording and replaying: the replay is exact.
        printf("rec trace: step %d, game state %d, Harry at %d %d %d, seed %08X\n", (int)g_Demo_DemoStep, (int)gameState,
               g_SysWork.playerWork.player.position.vx, g_SysWork.playerWork.player.position.vy,
               g_SysWork.playerWork.player.position.vz, (unsigned)Rng_GetSeed());
    }
    if (!s_Active || s_Mode != 1)
    {
        return 0;
    }
    frame                       = &s_Rec.frames[g_Demo_DemoStep];
    frame->expectedGameState    = gameState;
    frame->videoPresentInterval = 1;
    g_Demo_ActivePlaybackFrame  = frame;
    g_Demo_DemoStep++;
    if (g_Demo_DemoStep % 60 == 0)
    {
        stream(sizeof(s_DemoState) + (g_Demo_DemoStep - 1) * sizeof(s_DemoPlaybackFrame)); /* the last frame's pad comes next frame */
    }
    if (g_Demo_DemoStep % REC_FLUSH == 0 && !s_Remote)
    {
        write_out(g_Demo_DemoStep - 1);
    }
    return 1;
}

/** Demo_GameRandSeedSet while recording: the seed goes into the step's frame. */
int Port_RecSeed(void)
{
    if (!s_Active || s_Mode != 1 || g_Demo_ActivePlaybackFrame == NULL)
    {
        return 0;
    }
    g_Demo_ActivePlaybackFrame->randSeed = Rng_GetSeed();
    return 1;
}

/** Main loop, demo branch, after the wait: while recording, the step lasts the vertical blanks it took. */
void Port_RecInterval(void)
{
    if (!s_Active || s_Mode != 1 || g_Demo_ActivePlaybackFrame == NULL)
    {
        return;
    }
    g_Demo_VideoPresentInterval                      = MIN(MAX(g_VBlanks, 1), 4);
    g_Demo_ActivePlaybackFrame->videoPresentInterval = g_Demo_VideoPresentInterval;
}

/** Fs_QueueChunksLoad: a chunk load lasts as many frames of its step as when recorded. */
int Port_RecHold(int isFinished)
{
    int step = g_Demo_DemoStep;
    u16 fr   = Port_DemoStepFrames() + 1; /* 0: no load recorded */
    u16 v;
    if (!s_Active || step >= s_Steps)
    {
        return isFinished;
    }
    v = hold_get(step);
    if (s_Mode == 1)
    {
        if (isFinished && v == 0)
        {
            s_Rec.frames[step].field_A[0] = (s8)MIN(fr, 255);
        }
        return isFinished;
    }
    if (v == 0)
    {
        return isFinished;
    }
    if (fr < v)
    {
        return 0;
    }
    if (!isFinished)
    {
        s_Waits++;
        while (!(Fs_QueueGetLength() == 0 && WorldGfx_ChunkInitCheck()))
        {
            VSync(SyncMode_Wait);
            Fs_QueueUpdate();
        }
    }
    return 1;
}

/** Demo_ExitDemo (the end, a save screen, a game over): stops instead of the warm reset. */
int Port_RecExit(void)
{
    if (!s_Active)
    {
        return 0;
    }
    stop(g_Demo_DemoStep >= s_Steps ? "end" : "save screen or game over");
    return 1;
}

/** Demo_Update's STEP ERROR. */
void Port_RecDiverged(void)
{
    if (s_Active)
    {
        stop("diverged: STEP ERROR");
    }
}

/** ps2_ctl.py rec NAME (agent thread): arms recording for the next save loaded; an empty name stops it. */
void Port_RecRemote(const char* name8)
{
    if (!name8[0])
    {
        if (s_Active && s_Mode == 1)
        {
            s_StopRequest = 1;
        }
        else
        {
            s_Mode = 0;
            printf("rec: disarmed\n");
        }
        return;
    }
    if (s_Active)
    {
        printf("rec: already %s %s\n", s_Mode == 1 ? "recording" : "replaying", s_Name);
        return;
    }
    memcpy(s_Name, name8, 8);
    s_Name[8] = 0;
    s_Remote  = 1;
    s_Mode    = 1;
    printf("rec: armed %s: load a save to start (stop: Select + Start + L1 + R1, or ps2_ctl.py rec stop)\n", s_Name);
}

/** ps2_ctl.py replay (agent thread): `len` bytes of the recording at `offset`, acknowledged in the log. */
void Port_RecUpload(unsigned int offset, const void* data, unsigned int len)
{
    if (s_Active || offset + len > sizeof(s_Rec) || len > 1024)
    {
        printf("rec: upload at %u refused (%s)\n", offset, s_Active ? "recording or replaying" : "too large");
        return;
    }
    if (offset == 0)
    {
        s_Uploaded = 0;
    }
    memcpy((u8*)&s_Rec + offset, data, len);
    s_Uploaded = MAX(s_Uploaded, (int)(offset + len));
    printf("rec upload %u %u\n", offset, len);
}

/** ps2_ctl.py replay NAME (agent thread), after the upload: the REPLAY screen starts it. */
void Port_RecRemotePlay(const char* name8)
{
    u32 sum = 0;
    int i;
    if (s_Active || s_Uploaded < (int)sizeof(s_DemoState))
    {
        printf("rec: replay %s refused (%s)\n", name8, s_Active ? "recording or replaying" : "nothing uploaded");
        return;
    }
    for (i = 0; i < s_Uploaded; i++)
    {
        sum = sum * 31 + ((u8*)&s_Rec)[i];
    }
    memcpy(s_Name, name8, 8);
    s_Name[8]    = 0;
    s_Remote     = 1;
    s_Mode       = 2;
    s_RemotePlay = 1;
    printf("rec: received %s: %d bytes, sum %08X; waiting for the REPLAY screen\n", s_Name, s_Uploaded, (unsigned)sum);
}

/** The REPLAY screen while waiting: a recording to start (uploaded, or host:rec.txt's `play NAME`). */
int Port_RecReplayReady(void)
{
    return !s_Active && (s_RemotePlay || mode() == 2);
}

/** The REPLAY screen: starts the replay with Debug Options rendering bits `bits`, as SaveScreen_Continue
 * does a loaded save; the screen then fades out and goes to the load screen. 0 when it couldn't start. */
int Port_RecReplayBegin(int bits)
{
    s_Override = bits;
    Port_RecContinue();
    s_Override = -1;
    if (!s_Active)
    {
        return 0;
    }
    MemCard_SysDisable();
    D_800A97D7          = 1;
    g_GameWork.autosave = g_GameWork.savegame;
    GameBoot_WorldInit();
    g_SysWork.processFlags = ProcessFlag_LoadSave;
    GameBoot_MapLoad(g_SavegamePtr->mapIdx);
    return 1;
}
