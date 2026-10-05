/** @brief Port profiling counters (include/port/prof.h). Cycles from the EE's CP0 Count register,
 * reliable for computation (not across idle waits under emulation). */

#ifdef SH_PORT_PROF

extern int printf(const char* fmt, ...);

#define SLOTS 256

typedef struct
{
    const char*  name;
    unsigned int cycles; /* in units of 16 cycles, to fit a long report interval */
    unsigned int calls;
} Slot;

static Slot s_Slots[SLOTS];

static unsigned int count(void)
{
    unsigned int c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

void Prof_Begin(const char* name, unsigned int* start)
{
    (void)name;
    *start = count();
}

void Prof_End(const char* name, unsigned int start)
{
    unsigned int h = ((unsigned int)(unsigned long)name >> 2) % SLOTS;
    while (s_Slots[h].name && s_Slots[h].name != name)
    {
        h = (h + 1) % SLOTS;
    }
    s_Slots[h].name = name;
    s_Slots[h].cycles += (count() - start) >> 4;
    s_Slots[h].calls++;
}

/** Prints the counters as kcycles per frame (largest first), then clears them. */
void Prof_Report(unsigned int frames)
{
    int printed, i;
    if (frames == 0)
    {
        return;
    }
    printf("prof: per frame over %u frames (kcycles, calls):\n", frames);
    for (printed = 0; printed < 25; printed++)
    {
        int best = -1;
        for (i = 0; i < SLOTS; i++)
        {
            if (s_Slots[i].name && s_Slots[i].calls && (best < 0 || s_Slots[i].cycles > s_Slots[best].cycles))
            {
                best = i;
            }
        }
        if (best < 0)
        {
            break;
        }
        printf("prof: %7u %6u %s\n", s_Slots[best].cycles * 16 / 1000 / frames, s_Slots[best].calls / frames,
               s_Slots[best].name);
        s_Slots[best].calls = 0; /* printed */
    }
    for (i = 0; i < SLOTS; i++)
    {
        s_Slots[i].cycles = 0;
        s_Slots[i].calls  = 0;
    }
}

#endif
