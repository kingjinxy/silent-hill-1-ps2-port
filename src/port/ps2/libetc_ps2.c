/** @brief PS1 libetc (VSync, callbacks, video mode) on the EE's vertical-blank interrupt. */

#include <kernel.h>

static volatile int s_VBlanks;
static int          s_Sema = -1;
static void (*s_Callback)(void);

static int vblank_handler(int cause)
{
    (void)cause;
    s_VBlanks++;
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
    AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0);
    EnableIntc(INTC_VBLANK_S);
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
