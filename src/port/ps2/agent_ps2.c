/** @brief Remote control for hardware testing: commands from the VM (tools/port/ps2_ctl.py).
 *
 * sh1agent.irx (src/port/iop/sh1agent, embedded in the ELF by tools/port/port_link.py) listens on
 * UDP port 62968 through Neutrino's ministack and copies each command into s_Mailbox by SIF DMA.
 * The vertical blank handler (libetc_ps2.c) checks the mailbox's sequence number and wakes the agent
 * thread, which runs above the game's priority, so commands work while the game is busy or stuck in
 * a loop (not after a hard lockup, or with interrupts off).
 *
 * Commands: "RS" restart the game (Port_Restart: under Neutrino, a reload from the image on the
 * VM), "PI" ping (answered by the IOP module itself). In PCSX2 there's no ministack, so the module
 * doesn't load and nothing changes.
 */

#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <stdio.h>

extern unsigned char sh1agent_irx[];
extern unsigned int  size_sh1agent_irx;
extern void          Port_Restart(void); /* crash_ps2.c */

typedef struct
{
    unsigned int magic, seq, cmd, arg;
} Mailbox;

static Mailbox      s_Mailbox __attribute__((aligned(64)));
static unsigned int s_LastSeq;
static int          s_Sema = -1;
static unsigned char s_Stack[4096] __attribute__((aligned(16)));

/** The mailbox as the IOP's DMA wrote it (uncached: no stale cache lines). */
static volatile Mailbox* mailbox(void)
{
    return (volatile Mailbox*)((unsigned int)&s_Mailbox | 0x20000000);
}

static void agent(void* arg)
{
    (void)arg;
    for (;;)
    {
        unsigned int cmd;
        WaitSema(s_Sema);
        cmd = mailbox()->cmd;
        printf("agent: command %c%c\n", cmd & 0xFF, (cmd >> 8) & 0xFF);
        if (cmd == ('R' | ('S' << 8)))
        {
            Port_Restart();
        }
    }
}

/** From the vertical blank interrupt: a new command wakes the agent thread. */
void Port_AgentVBlank(void)
{
    unsigned int seq;
    if (s_Sema < 0)
    {
        return;
    }
    seq = mailbox()->seq;
    if (seq != s_LastSeq)
    {
        s_LastSeq = seq;
        iSignalSema(s_Sema);
    }
}

void Port_AgentInit(void)
{
    char         args[24];
    int          len, id, result = 0;
    ee_sema_t    sema;
    ee_thread_t  th;

    s_Mailbox.seq = 0;
    FlushCache(0); /* no dirty lines of the mailbox left to overwrite what the IOP writes */
    len = sprintf(args, "mb=0x%08x", (unsigned int)&s_Mailbox & 0x1FFFFFFF) + 1;
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    id = SifExecModuleBuffer(sh1agent_irx, size_sh1agent_irx, len, args, &result);
    if (id < 0 || result == 1) /* 1: MODULE_NO_RESIDENT_END */
    {
        printf("port: remote control not available (sh1agent.irx: %d, %d; no network stack?)\n", id, result);
        return;
    }
    s_LastSeq = mailbox()->seq;

    sema.init_count = 0;
    sema.max_count  = 1;
    sema.option     = 0;
    s_Sema          = CreateSema(&sema);
    th.func             = agent;
    th.stack            = s_Stack;
    th.stack_size       = sizeof(s_Stack);
    th.gp_reg           = &_gp;
    th.initial_priority = 2; /* above the game (64); the heartbeat is 1 */
    StartThread(CreateThread(&th), NULL);
    printf("port: remote control on UDP port 62968 (tools/port/ps2_ctl.py)\n");
}
