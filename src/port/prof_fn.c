/** @brief Function-level profiler (build the game with SH1_EXTRA_CFLAGS=-finstrument-functions and
 * SH1_PROF=1): exclusive EE cycles per instrumented function, from a shadow stack. Reported with
 * Prof_Report (include/port/prof.h). Interrupt handlers (the vertical blank callback) are skipped:
 * the main thread's entries/exits only. */

#ifdef SH_PORT_PROF

extern int printf(const char* fmt, ...);
extern volatile int g_PortInInterrupt; /* src/port/ps2/libetc_ps2.c */

#define DEPTH 256
#define SLOTS 1024

typedef struct
{
    void*        fn;
    unsigned int cycles; /* units of 16 */
    unsigned int calls;
} FnSlot;

static FnSlot       s_Fn[SLOTS];
static void*        s_StackFn[DEPTH];
static unsigned int s_StackStart[DEPTH];
static unsigned int s_StackChild[DEPTH];
static int          s_Top;

__attribute__((no_instrument_function)) static unsigned int now(void)
{
    unsigned int c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void* fn, void* site)
{
    (void)site;
    if (g_PortInInterrupt)
    {
        return;
    }
    if (s_Top < DEPTH)
    {
        s_StackFn[s_Top]    = fn;
        s_StackStart[s_Top] = now();
        s_StackChild[s_Top] = 0;
    }
    s_Top++;
}

__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void* fn, void* site)
{
    unsigned int t, total, h;
    (void)site;
    if (g_PortInInterrupt || s_Top <= 0)
    {
        return;
    }
    s_Top--;
    if (s_Top >= DEPTH)
    {
        return;
    }
    t     = now();
    total = t - s_StackStart[s_Top];
    if (s_Top > 0 && s_Top - 1 < DEPTH)
    {
        s_StackChild[s_Top - 1] += total;
    }
    h = ((unsigned int)fn >> 3) % SLOTS;
    while (s_Fn[h].fn && s_Fn[h].fn != fn)
    {
        h = (h + 1) % SLOTS;
    }
    s_Fn[h].fn = fn;
    s_Fn[h].cycles += (total - s_StackChild[s_Top]) >> 4;
    s_Fn[h].calls++;
}

/** Prints the top functions by exclusive kcycles per frame, then clears. */
__attribute__((no_instrument_function)) void ProfFn_Report(unsigned int frames)
{
    int printed, i;
    if (!frames)
    {
        return;
    }
    printf("proffn: exclusive kcycles per frame over %u frames (fn address, calls per frame):\n", frames);
    for (printed = 0; printed < 30; printed++)
    {
        int best = -1;
        for (i = 0; i < SLOTS; i++)
        {
            if (s_Fn[i].fn && s_Fn[i].calls && (best < 0 || s_Fn[i].cycles > s_Fn[best].cycles))
            {
                best = i;
            }
        }
        if (best < 0)
        {
            break;
        }
        printf("proffn: %6u %6u %p\n", s_Fn[best].cycles * 16 / 1000 / frames, s_Fn[best].calls / frames, s_Fn[best].fn);
        s_Fn[best].calls = 0;
    }
    for (i = 0; i < SLOTS; i++)
    {
        s_Fn[i].cycles = 0;
        s_Fn[i].calls  = 0;
    }
}

#endif
