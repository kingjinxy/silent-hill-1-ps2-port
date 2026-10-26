/** @brief Floatstinger crash on hardware (docs/ToFix.md): Floatstinger_Update keeps the animation
 * header pointer at 0x01FFEB10 across func_800D35F0, and its upper half becomes 0xFC00. SH1_SENTINEL=1:
 * EE timer 1 interrupts about every 50 us and checks the slot; the interrupted pc (CP0 EPC) of the
 * last 16 checks is kept, so the first bad sighting gives the code that ran just before the write.
 * The sound tick thread (spu_ps2.c) and the vertical blank handler also check before and after their
 * work. Nothing is printed from an interrupt: the heartbeat thread prints the record once. */
#ifdef SH_PORT_SENTINEL
#include <kernel.h>
#include <stdio.h>
#include <timer.h>

extern char g_CharaModelAnimsData[];

#define SLOT     (*(volatile unsigned int*)0x01FFEB10)
#define HDR      (*(volatile unsigned int*)(g_CharaModelAnimsData + 0x18 + 8)) /* [1].activeAnmHdr */
#define T1_EVERY 7373                                                         /* BUSCLK 147.456 MHz: 50 us */

static volatile unsigned int s_Ring[16], s_RingPos, s_Checks;
static volatile unsigned int s_Seen, s_SeenSlot, s_SeenHdr, s_SeenChecks, s_SeenRing[16];
static const char* volatile  s_SeenWhere;
static volatile unsigned int s_Printed;

static int bad(void)
{
    unsigned int slot = SLOT, hdr = HDR;
    return (slot >> 16) == 0xFC00 && (slot & 0xFFFF) == (hdr & 0xFFFF) && hdr;
}

/** Interrupts disabled or in a handler: records the first sighting with the EPC ring. */
static void record(const char* where)
{
    unsigned int i;
    if (s_Seen || !bad())
    {
        return;
    }
    s_SeenSlot   = SLOT;
    s_SeenHdr    = HDR;
    s_SeenChecks = s_Checks;
    s_SeenWhere  = where;
    for (i = 0; i < 16; i++)
    {
        s_SeenRing[i] = s_Ring[(s_RingPos + i) & 15]; /* oldest first */
    }
    s_Seen = 1;
}

/** Thread context (sound tick before/after). */
void Port_SentinelCheck(const char* where)
{
    DI();
    record(where);
    EI();
}

/** From the vertical blank handler. */
void Port_SentinelIsr(const char* where)
{
    record(where);
}

static int timer_handler(int cause)
{
    unsigned int epc;
    (void)cause;
    __asm__ volatile("mfc0 %0, $14" : "=r"(epc));
    *T1_MODE = *T1_MODE | (1 << 10); /* clear EQUF */
    s_Checks++;
    if (!s_Seen)
    {
        s_Ring[s_RingPos++ & 15] = epc;
        record("timer");
    }
    ExitHandler();
    return 0;
}

void Port_SentinelStart(void)
{
    *T1_MODE  = 0;
    *T1_COUNT = 0;
    *T1_COMP  = T1_EVERY;
    AddIntcHandler(INTC_TIM1, timer_handler, 0);
    EnableIntc(INTC_TIM1);
    *T1_MODE = (1 << 6) | (1 << 7) | (1 << 8) | (1 << 10); /* BUSCLK, reset on compare, count, compare interrupt */
    printf("SENTINEL: timer 1 every %d bus cycles, slot 0x01FFEB10\n", T1_EVERY);
}

/** Heartbeat thread: prints the record once. */
void Port_SentinelReport(void)
{
    static int alive;
    int        i;
    if (!alive && s_Checks)
    {
        alive = 1;
        printf("SENTINEL: timer running (%u checks so far)\n", s_Checks);
    }
    if (!s_Seen || s_Printed)
    {
        return;
    }
    s_Printed = 1;
    printf("SENTINEL: 0x01FFEB10 = %08X (header %08X) first seen by %s at check %u; interrupted pcs, oldest first:\n",
           s_SeenSlot, s_SeenHdr, s_SeenWhere, s_SeenChecks);
    for (i = 0; i < 16; i++)
    {
        printf("SENTINEL:   %08X\n", s_SeenRing[i]);
    }
}
#endif
