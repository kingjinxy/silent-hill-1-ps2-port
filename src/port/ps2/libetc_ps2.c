/** @brief PS1 libetc (VSync, callbacks, video mode) on the EE's vertical-blank interrupt.
 *
 * Also a heartbeat for tools/port/pcsx2_run.py's crash detection: a high-priority thread prints the
 * vertical blank and presented frame counts once per 60 vertical blanks (an emulated second), so a
 * game that stops presenting frames can be told from a slow emulator, and the share of that second
 * spent waiting in VSync (idle: headroom).
 */

#include <kernel.h>
#include <stdio.h>

static volatile int s_VBlanks;
static int          s_Sema = -1;
static int          s_HandlerId = -1;
static void (*s_Callback)(void);
static volatile int s_CallbackManual;
volatile int g_PortInInterrupt; /* set while the game's vertical blank callback runs (profilers) */
static unsigned int s_WaitCycles; /* EE cycles spent waiting in VSync (idle), for the heartbeat */

extern void Pad_Poll(void);        /* libpad_ps2.c */
extern int  Display_FrameCount(void); /* display_ps2.c */

static int s_HeartbeatSema = -1;
static u8  s_HeartbeatStack[4096] __attribute__((aligned(16)));

extern void Port_SpuStats(void); /* spu_ps2.c */

static void heartbeat(void* arg)
{
    unsigned int beats = 0;
    (void)arg;
    for (;;)
    {
        WaitSema(s_HeartbeatSema);
        /* idle: share of the last second the game spent waiting for a vertical blank (EE at
         * 294.912 MHz: 2,949,120 cycles = 1%). */
        printf("heartbeat: vblank %d frame %d idle %u%%\n", s_VBlanks, Display_FrameCount(), s_WaitCycles / 2949120);
        s_WaitCycles = 0;
        if (++beats % 10 == 0)
        {
            Port_SpuStats();
        }
    }
}

static volatile unsigned int s_VBlankCycles; /* CP0 Count at the last vertical blank */

/** Vertical blanks so far, and the EE cycle count when the last one came (for rcnt_ps2.c). */
int Port_VBlanks(unsigned int* cycles)
{
    int n;
    DI();
    n       = s_VBlanks;
    *cycles = s_VBlankCycles;
    EI();
    return n;
}

/* Where the interrupted code was at the last vertical blanks (EPC): a hung main loop shows up here
 * (agent_ps2.c "WH", tools/port/ps2_ctl.py where). */
unsigned int Port_PcSamples[16];
unsigned int Port_PcSampleCount;

static int vblank_handler(int cause)
{
    unsigned int now, epc;
    (void)cause;
    __asm__ volatile("mfc0 %0, $14" : "=r"(epc));
    Port_PcSamples[Port_PcSampleCount++ & 15] = epc;
    __asm__ volatile("mfc0 %0, $9" : "=r"(now));
    s_VBlankCycles = now;
    s_VBlanks++;
    if (s_VBlanks % 60 == 0)
    {
        iSignalSema(s_HeartbeatSema);
    }
    {
        extern void Port_AgentVBlank(void); /* agent_ps2.c: commands from the VM */
        Port_AgentVBlank();
    }
    if (s_Callback && !s_CallbackManual)
    {
        g_PortInInterrupt = 1;
        s_Callback();
        g_PortInInterrupt = 0;
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
    s_HandlerId = AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0);
    EnableIntc(INTC_VBLANK_S);
}

/** The game runs in the main thread at a low priority, so the heartbeat (and anything else that
 * must not starve) still runs if the game busy-waits. */
void Port_MainThreadInit(void)
{
    ChangeThreadPriority(GetThreadId(), 64);
}

/** gsKit's setup (run again on every display mode change) drops our vertical blank handler: the
 * count stopped and VSync no longer waited. Called after it, this installs the handler again. */
void Port_VBlankReinstall(void)
{
    if (s_Sema < 0)
    {
        return;
    }
    if (s_HandlerId >= 0)
    {
        RemoveIntcHandler(INTC_VBLANK_S, s_HandlerId);
    }
    s_HandlerId = AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0);
    EnableIntc(INTC_VBLANK_S);
}

/** As PSY-Q's libetc: only the first call resets the callbacks; later ones do nothing. libgpu's
 * ResetGraph (recompiled) calls it again during boot, on some paths; clearing every time removed the
 * game's vertical blank callback (Screen_VSyncCallback), whose counter the boot logos wait for: the
 * game sometimes stayed on the Konami logo (2026-10-09). */
/** Benchmark mode (game_main.c, SH1_BENCH): while on, the vertical blank callback isn't called from
 * the interrupt; the main loop calls it (Port_VSyncCallbackRun) once per simulated vertical blank. */
void Port_VSyncCallbackManual(int on)
{
    s_CallbackManual = on;
}

void Port_VSyncCallbackRun(void)
{
    if (s_Callback)
    {
        s_Callback();
    }
}

int ResetCallback(void)
{
    static int done;
    init();
    if (!done)
    {
        done       = 1;
        s_Callback = 0;
    }
    return 0;
}

/** PS1 semantics: mode 0 waits for the next vertical blank, n > 1 waits n of them, 1 returns
 * immediately, a negative mode returns the vertical blank count. (The PS1 returns horizontal-blank
 * counts for 0/1; nothing in the game uses those yet.) */
/** The PS1 BIOS refreshes the pad buffers every vertical blank. Here: at most once per vertical blank,
 * from any VSync call (a 60 fps frame running late never waits, but the main loop still asks for
 * the count: polling only after a wait froze the pad in heavy scenes). Not from the interrupt:
 * mode and vibration calls can go through the IOP. */
static void pad_refresh(void)
{
    static int polledAt = -1;
    if (polledAt != s_VBlanks)
    {
        polledAt = s_VBlanks;
        Pad_Poll();
    }
}

int VSync(int mode)
{
    int n, target;

    init();
    if (mode < 0)
    {
        pad_refresh();
        return s_VBlanks;
    }
    if (mode == 1)
    {
        pad_refresh();
        return 0;
    }
    n      = mode == 0 ? 1 : mode;
    target = s_VBlanks + n;
    /* Sleep until the count reaches the target: the semaphore only wakes us up (other code may
     * signal it too: after gsKit's setup runs again, VSync returned without a vertical blank). */
    {
        unsigned int t0, t1;
        __asm__ volatile("mfc0 %0, $9" : "=r"(t0));
        while (s_VBlanks - target < 0)
        {
            WaitSema(s_Sema);
        }
        __asm__ volatile("mfc0 %0, $9" : "=r"(t1));
        s_WaitCycles += t1 - t0;
    }
    pad_refresh();
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
