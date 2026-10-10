/** @brief Non-blocking messages for sh1spu.irx.
 *
 * A printf on the IOP waits for the console device, which under Neutrino is ministack's udptty; on
 * hardware that path stalled for good during disc loads (2026-10-10), and the sound threads that
 * printed stopped with it: the command thread no longer drained the ring, and the EE waited for
 * room in it (the game froze when a voice line started). Here a message only goes into a ring
 * buffer; a thread of the lowest priority prints it, and only that thread waits if the console
 * stalls. (This file doesn't include log.h: printf here is the real one.)
 */

#include "irx_imports.h"

#define RING 4096 /* a power of two */

static char         s_Ring[RING];
static volatile u32 s_In, s_Out, s_Dropped; /* running byte counts */
static int          s_Sema = -1;

void spu_log_put(const char* line, int n)
{
    int i, state;
    CpuSuspendIntr(&state);
    if (n > 0 && RING - (s_In - s_Out) >= (u32)n)
    {
        for (i = 0; i < n; i++)
        {
            s_Ring[(s_In + i) & (RING - 1)] = line[i];
        }
        s_In += n;
    }
    else if (n > 0)
    {
        s_Dropped += n;
    }
    CpuResumeIntr(state);
    if (s_Sema >= 0)
    {
        SignalSema(s_Sema);
    }
}

static char s_Chunk[257];

static void printer(void* arg)
{
    char* chunk = s_Chunk;
    (void)arg;
    for (;;)
    {
        WaitSema(s_Sema);
        while (s_Out != s_In)
        {
            u32 at = s_Out & (RING - 1), n = s_In - s_Out, i;
            if (n > RING - at)
            {
                n = RING - at;
            }
            if (n > 256)
            {
                n = 256;
            }
            for (i = 0; i < n; i++)
            {
                chunk[i] = s_Ring[at + i];
            }
            chunk[n] = 0;
            s_Out += n;
            printf("%s", chunk);
        }
        if (s_Dropped)
        {
            u32 d     = s_Dropped;
            s_Dropped = 0;
            printf("sh1spu: %u bytes of messages dropped (buffer full)\n", (unsigned)d);
        }
    }
}

void spu_log_init(void)
{
    iop_sema_t   sema;
    iop_thread_t th;
    sema.attr    = 0;
    sema.option  = 0;
    sema.initial = 0;
    sema.max     = 1;
    s_Sema       = CreateSema(&sema);
    th.attr      = TH_C;
    th.option    = 0;
    th.thread    = printer;
    th.stacksize = 0x1000; /* the IOP's printf needs room */
    th.priority  = 120; /* the lowest: below every sound thread */
    StartThread(CreateThread(&th), NULL);
}
