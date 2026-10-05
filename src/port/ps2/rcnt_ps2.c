/** @brief PS1 root counters (libapi SetRCnt/GetRCnt/...) derived from the EE's cycle counter.
 *
 * The game uses counter 1 (horizontal blanks) for frame timing: libgs GsGetVcount() is
 * GetRCnt(RCntCNT1), and the frame's delta time comes from it. Counters run at the PS1 rates
 * (0: system clock, 1: NTSC horizontal blanks, 2: system clock / 8, 3: vertical blanks), counted
 * from the last ResetRCnt/SetRCnt/StartRCnt, as 16-bit values that wrap at the target when one is set.
 * Counter 1 counts whole fields from the actual vertical blank interrupts (262.5 lines each): the
 * game's frame time comes from it, read right after waiting for a vertical blank, and the EE's cycle
 * counter doesn't track time between vertical blanks reliably under emulation (frames measured only
 * a few lines long, so fades took minutes).
 * Counter interrupts (used by the sound driver's tick, through events) aren't implemented yet.
 */

extern int Port_VBlanks(unsigned int* cycles); /* libetc_ps2.c */

#define EE_CLOCK   294912000ULL
#define PS1_CLOCK  33868800ULL
#define RCNT_COUNT 4

static const unsigned long long RATE[RCNT_COUNT] = {
    PS1_CLOCK,       /* RCntCNT0: system clock (dot clock not modelled) */
    15734ULL,        /* RCntCNT1: NTSC horizontal blanks (262.5 lines x 59.94 Hz) */
    PS1_CLOCK / 8,   /* RCntCNT2: system clock / 8 */
    60ULL,           /* RCntCNT3: vertical blanks */
};

typedef struct
{
    unsigned long long base;
    unsigned int       target;
    long               mode;
} RCnt;

static RCnt s_Cnt[RCNT_COUNT];

/** 64-bit EE cycle count, extended from the 32-bit CP0 Count register (must be read at least every
 * ~14 s, which the game's frame loop does). */
static unsigned long long ee_cycles(void)
{
    static unsigned int       last;
    static unsigned long long high;
    unsigned int              now;
    __asm__ volatile("mfc0 %0, $9" : "=r"(now));
    if (now < last)
    {
        high += 1ULL << 32;
    }
    last = now;
    return high | now;
}

/** Horizontal blanks so far, from the vertical blank count (NTSC: 262.5 lines per field). */
static unsigned long long hblanks(void)
{
    unsigned int at;
    return (unsigned long long)Port_VBlanks(&at) * 525 / 2;
}

/** Current count source for counter i, in counter units. */
static unsigned long long source(int i)
{
    return i == 1 ? hblanks() : ee_cycles() * RATE[i] / EE_CLOCK;
}

static int index_of(unsigned long spec)
{
    int i = (int)(spec & 0xF);
    return ((spec & 0xFFFF0000UL) == 0xF2000000UL && i < RCNT_COUNT) ? i : -1;
}

long SetRCnt(unsigned long spec, unsigned short target, long mode)
{
    int i = index_of(spec);
    if (i < 0)
    {
        return 0;
    }
    s_Cnt[i].target = target;
    s_Cnt[i].mode   = mode;
    s_Cnt[i].base   = source(i);
    return 1;
}

long GetRCnt(unsigned long spec)
{
    int i = index_of(spec);
    unsigned long long v;
    if (i < 0)
    {
        return 0;
    }
    v = source(i) - s_Cnt[i].base;
    if (s_Cnt[i].target)
    {
        v %= s_Cnt[i].target;
    }
    return (long)(v & 0xFFFF);
}

long ResetRCnt(unsigned long spec)
{
    int i = index_of(spec);
    if (i < 0)
    {
        return 0;
    }
    s_Cnt[i].base = source(i);
    return 1;
}

long StartRCnt(unsigned long spec)
{
    return ResetRCnt(spec);
}

long StopRCnt(unsigned long spec)
{
    return index_of(spec) >= 0;
}
