/** @brief PS1 libetc (VSync, callbacks, video mode) on the EE's vertical-blank interrupt.
 *
 * Also a heartbeat for tools/port/pcsx2_run.py's crash detection: a high-priority thread prints the
 * vertical blank and presented frame counts once per 60 vertical blanks (an emulated second), so a
 * game that stops presenting frames can be told from a slow emulator.
 */

#include <kernel.h>
#include <stdio.h>

static volatile int s_VBlanks;
static int          s_Sema = -1;
static void (*s_Callback)(void);

extern void Pad_Poll(void);        /* libpad_ps2.c */
extern int  Display_FrameCount(void); /* display_ps2.c */

static int s_HeartbeatSema = -1;
static u8  s_HeartbeatStack[4096] __attribute__((aligned(16)));

static void heartbeat(void* arg)
{
    (void)arg;
    for (;;)
    {
        WaitSema(s_HeartbeatSema);
        printf("heartbeat: vblank %d frame %d\n", s_VBlanks, Display_FrameCount());
    }
}

static int vblank_handler(int cause)
{
    (void)cause;
    s_VBlanks++;
    if (s_VBlanks % 60 == 0)
    {
        iSignalSema(s_HeartbeatSema);
    }
    if (s_Callback)
    {
        s_Callback();
    }
    iSignalSema(s_Sema);
    ExitHandler();
    return 0;
}

static void init(void)
{
    ee_sema_t sema;

    if (s_Sema >= 0)
    {
        return;
    }
    sema.init_count = 0;
    sema.max_count  = 1;
    sema.option     = 0;
    s_Sema          = CreateSema(&sema);
    {
        static ee_thread_t th;
        s_HeartbeatSema    = CreateSema(&sema);
        th.func             = (void*)heartbeat;
        th.stack            = s_HeartbeatStack;
        th.stack_size       = sizeof(s_HeartbeatStack);
        th.gp_reg           = &_gp;
        th.initial_priority = 1;
        StartThread(CreateThread(&th), NULL);
    }
    AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0);
    EnableIntc(INTC_VBLANK_S);
}

/** The game runs in the main thread at a low priority, so the heartbeat (and anything else that
 * must not starve) still runs if the game busy-waits. */
void Port_MainThreadInit(void)
{
    ChangeThreadPriority(GetThreadId(), 64);
}

int ResetCallback(void)
{
    init();
    s_Callback = 0;
    return 0;
}

/** PS1 semantics: mode 0 waits for the next vertical blank, n > 1 waits n of them, 1 returns
 * immediately, a negative mode returns the vertical blank count. (The PS1 returns horizontal-blank
 * counts for 0/1; nothing in the game uses those yet.) */
int VSync(int mode)
{
    int n;

    init();
    if (mode < 0)
    {
        return s_VBlanks;
    }
    if (mode == 1)
    {
        return 0;
    }
    n = mode == 0 ? 1 : mode;
    PollSema(s_Sema); /* Wait for a fresh vertical blank, not one that already happened. */
    while (n-- > 0)
    {
        WaitSema(s_Sema);
    }
    Pad_Poll(); /* the PS1 BIOS refreshes the pad buffers every vertical blank */
    return 0;
}

int VSyncCallback(void (*f)(void))
{
    init();
    s_Callback = f;
    return 0;
}

long GetVideoMode(void)
{
    return 0; /* MODE_NTSC */
}
