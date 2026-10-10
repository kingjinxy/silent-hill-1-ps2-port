/** @brief Non-blocking console output: stdout and stderr go into a ring buffer that a thread of its own
 * sends on.
 *
 * Every EE write to the console is an RPC to the IOP (under Neutrino: ministack's udptty, sent over
 * the network to tools/port/ps2_log.py). The calling thread waits for the IOP's answer, so when that
 * path stalled, every thread that printed stalled with it: the game, the heartbeat and the remote
 * control agent (a black screen at the title, 2026-10-10, with only the IOP agent's ping answering).
 * Here a write only copies the text into the ring (lines are dropped when it's full, and counted) and
 * the log thread does the waiting. The vertical blank handler wakes it (Port_LogVBlank).
 *
 * On hardware the log thread hands the text to sh1agent.irx, which broadcasts it to
 * tools/port/ps2_log.py itself (agent_ps2.c, src/port/iop/sh1agent): the console device under
 * Neutrino (ministack's udptty) stalled for good during disc loads, and a thread here waiting on it
 * would only have kept the log back. Elsewhere (PCSX2, or before the agent is loaded) it writes
 * to the console.
 *
 * Linked with --wrap=_write (tools/port/port_link.py): ps2sdk's libcglue _write is __real__write.
 * Before Port_LogInit and after Port_LogDirect (the crash reporter), writes go straight through.
 */

#include <kernel.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>

extern int __real__write(int fd, const void* buf, int n);

#define RING 32768 /* a power of two */

static char                  s_Ring[RING];
static volatile unsigned int s_In, s_Out; /* running byte counts */
static volatile unsigned int s_Dropped;   /* bytes dropped since the last report */
static int                   s_Sema = -1;
static volatile int          s_On;
static unsigned char         s_Stack[8192] __attribute__((aligned(16)));

int __wrap__write(int fd, const void* buf, int n)
{
    int was;
    if (!s_On || (fd != 1 && fd != 2) || n <= 0)
    {
        return __real__write(fd, buf, n);
    }
    was = DIntr(); /* a copy only: nothing waits with interrupts off */
    if (RING - (s_In - s_Out) >= (unsigned int)n)
    {
        unsigned int at    = s_In & (RING - 1);
        unsigned int first = RING - at < (unsigned int)n ? RING - at : (unsigned int)n;
        memcpy(&s_Ring[at], buf, first);
        memcpy(s_Ring, (const char*)buf + first, n - first);
        s_In += n;
    }
    else
    {
        s_Dropped += n;
    }
    if (was)
    {
        EIntr();
    }
    return n;
}

extern unsigned int Port_AgentLogBuffer(void); /* agent_ps2.c */
extern unsigned int Port_AgentLogAck(void);

#define CHUNK 2048 /* the IOP agent's buffer */

static unsigned char s_Chunk[16 + CHUNK] __attribute__((aligned(64))); /* header (sequence, length), text */
static unsigned int  s_Seq;
static volatile int  s_Waiting; /* for the agent's acknowledgement: woken every vertical blank */

/** Sends n bytes (at most CHUNK) through the IOP agent: the text, then the header that announces it;
 * returns once the agent has sent it on. */
static void agent_send(unsigned int iop, const char* text, unsigned int n)
{
    SifDmaTransfer_t dt;
    int              id;
    memcpy(s_Chunk + 16, text, n);
    ((unsigned int*)s_Chunk)[0] = ++s_Seq;
    ((unsigned int*)s_Chunk)[1] = n;
    SifWriteBackDCache(s_Chunk, sizeof(s_Chunk));
    dt.src  = s_Chunk + 16;
    dt.dest = (void*)(iop + 16);
    dt.size = (n + 15) & ~15u;
    dt.attr = 0;
    while (SifSetDma(&dt, 1) == 0)
    {
    }
    dt.src  = s_Chunk; /* after the text: SIF DMA transfers go in order */
    dt.dest = (void*)iop;
    dt.size = 16;
    while ((id = SifSetDma(&dt, 1)) == 0)
    {
    }
    while (SifDmaStat(id) >= 0)
    {
    }
    s_Waiting = 1;
    while (Port_AgentLogAck() != s_Seq)
    {
        WaitSema(s_Sema); /* every vertical blank meanwhile (Port_LogVBlank) */
    }
    s_Waiting = 0;
}

static void send(const char* text, unsigned int n)
{
    unsigned int iop = Port_AgentLogBuffer();
    if (iop)
    {
        agent_send(iop, text, n);
    }
    else
    {
        __real__write(1, text, (int)n);
    }
}

static void writer(void* arg)
{
    (void)arg;
    for (;;)
    {
        unsigned int in;
        WaitSema(s_Sema);
        while ((in = s_In) != s_Out)
        {
            unsigned int at = s_Out & (RING - 1);
            unsigned int n  = in - s_Out;
            if (n > RING - at)
            {
                n = RING - at;
            }
            if (n > CHUNK)
            {
                n = CHUNK;
            }
            send(&s_Ring[at], n);
            s_Out += n;
        }
        if (s_Dropped)
        {
            char         line[64];
            unsigned int d = s_Dropped;
            int          len;
            s_Dropped      = 0;
            len            = sprintf(line, "log: %u bytes of output dropped (buffer full)\n", d);
            send(line, (unsigned int)len);
        }
    }
}

/** From the vertical blank interrupt: sends what was written since. */
void Port_LogVBlank(void)
{
    if (s_On && (s_In != s_Out || s_Waiting))
    {
        iSignalSema(s_Sema);
    }
}

void Port_LogInit(void)
{
    ee_sema_t   sema;
    ee_thread_t th;
    sema.init_count     = 0;
    sema.max_count      = 1;
    sema.option         = 0;
    s_Sema              = CreateSema(&sema);
    th.func             = writer;
    th.stack            = s_Stack;
    th.stack_size       = sizeof(s_Stack);
    th.gp_reg           = &_gp;
    th.initial_priority = 30; /* above the game (64), so a busy frame doesn't hold the log back */
    StartThread(CreateThread(&th), NULL);
    s_On = 1;
}

/** Console output not sent yet (bytes), and how much was dropped since the last report. */
unsigned int Port_LogPending(unsigned int* dropped)
{
    *dropped = s_Dropped;
    return s_In - s_Out;
}

/** Writes go straight through from now on (the crash reporter: its lines must come out even if the
 * log thread is what stopped). */
void Port_LogDirect(void)
{
    s_On = 0;
}
