/** @brief Crash reporter: on a TLB miss or address error, prints the faulting state, then restarts.
 *
 * The level-1 exception handler (ps2sdk's ee_debug) saves the register frame, then returns to
 * crash_report() instead of the faulting instruction, so the report is printed from normal thread
 * context (where printf works; on hardware under Neutrino it goes over the network: ministack's
 * udptty, tools/port/ps2_log.py).
 *
 * Then, for hardware testing, it counts down 6 s and restarts the game from its disc (Port_Restart(), also used by agent_ps2.c):
 * under Neutrino that reloads it from the image on the VM. Holding Triangle for 3 s during the
 * countdown keeps the crashed state instead (RESET restarts). An IOP reset of our own (to restart
 * Neutrino from the USB stick) doesn't work under Neutrino: the module loader never answers after
 * it. */

#include <kernel.h>
#include <ee_debug.h>
#include <sifrpc.h>
#include <stdio.h>

#ifndef RELAUNCH_ELF
#define RELAUNCH_ELF "cdrom0:\\SHPS_000.01;1" /* the disc's BOOT2 (SYSTEM.CNF) */
#endif
#define PAD_TRIANGLE 0x1000 /* PS1 bit order */

extern int          VSync(int mode);           /* libetc_ps2.c */
/** TV background colour (GS BGCOLOR): which restart step was reached, without a network. */
static void stage_color(unsigned int rgb)
{
    *(volatile unsigned long long*)0x12000000 = 4;   /* PMODE: both read circuits off (CRTMD = 1) */
    *(volatile unsigned long long*)0x120000E0 = rgb; /* BGCOLOR: then the whole screen shows it */
}
extern unsigned int Port_PadButtons(void);     /* libpad_ps2.c */

static EE_RegFrame s_Frame;

static void stay(void)
{
    for (;;)
    {
        SleepThread();
    }
}

/** Restarts the game: LoadExecPS2 of the disc's boot ELF. Under Neutrino (hardware testing) its EE
 * core handles that as a game starting another ELF from its disc: the IOP is rebooted into the
 * emulation environment and the ELF is loaded from the image on the VM again. Returns only if that
 * fails. The TV background shows the step reached (stage_color). */
void Port_Restart(void)
{
    printf("port: restarting %s\n", RELAUNCH_ELF);
    VSync(0); /* let the message go out before the network modules go away */
    VSync(0);
    stage_color(0xC00000); /* blue: restarting */
    LoadExecPS2(RELAUNCH_ELF, 0, NULL);
    stage_color(0xC000C0); /* magenta: LoadExecPS2 returned */
}

/** 6 s countdown (over the network log); Triangle held for 3 s keeps the crashed state. */
static void countdown_and_restart(void)
{
    int t, held = 0;
    printf("CRASH: restarting in 6 s; hold Triangle for 3 s to keep this state\n");
    for (t = 0; t < 360 || held > 0; t++)
    {
        VSync(0);
        if (Port_PadButtons() & PAD_TRIANGLE)
        {
            if (++held == 180)
            {
                printf("CRASH: Triangle held: staying in the crashed state (RESET restarts)\n");
                stay();
            }
        }
        else
        {
            held = 0;
        }
        if (t % 60 == 0 && t < 360)
        {
            printf("CRASH: restarting in %d s\n", 6 - t / 60);
        }
    }
    Port_Restart();
    printf("CRASH: could not start %s\n", RELAUNCH_ELF);
    stay();
}

static void crash_report(void)
{
    static const char* const CAUSE[] = { "int", "TLB mod", "TLB load", "TLB store", "addr load", "addr store",
                                         "bus ifetch", "bus data", "syscall", "break" };
    int code = (s_Frame.cause >> 2) & 31;
    printf("CRASH: %s at pc=%08X badvaddr=%08X ra=%08X sp=%08X\n", code < 10 ? CAUSE[code] : "?", s_Frame.epc,
           s_Frame.badvaddr, s_Frame.ra[0], s_Frame.sp[0]);
    printf("CRASH: a0=%08X a1=%08X a2=%08X a3=%08X v0=%08X v1=%08X\n", s_Frame.a0[0], s_Frame.a1[0], s_Frame.a2[0],
           s_Frame.a3[0], s_Frame.v0[0], s_Frame.v1[0]);
    printf("CRASH: s0=%08X s1=%08X s2=%08X s3=%08X s4=%08X s5=%08X s6=%08X s7=%08X\n", s_Frame.s0[0], s_Frame.s1[0],
           s_Frame.s2[0], s_Frame.s3[0], s_Frame.s4[0], s_Frame.s5[0], s_Frame.s6[0], s_Frame.s7[0]);
    countdown_and_restart();
}

static int crash_handler(EE_RegFrame* frame)
{
    s_Frame   = *frame;
    frame->epc = (u32)crash_report;
    return 1;
}

/* Safety net for real hardware: decompiled PS1 code sometimes reads through NULL at a small offset
 * (the PS1 has RAM there; PCSX2 logs "TLB Miss" and returns 0; a real PS2 faults). The lowest 8 KB of
 * the address space is mapped read-only to a page of zeros, so such reads return 0 as in PCSX2, while
 * writes through NULL still fault (TLB modified: reported above). Not in PCSX2 (detected by its host
 * file system), where the TLB Miss lines show the sites to fix. */
static unsigned char s_ZeroPage[4096] __attribute__((aligned(4096)));

static void map_zero_page(void)
{
    FILE*        f = fopen("host:sh1_ps2.iso", "rb");
    unsigned int lo;
    int          entry;
    if (f)
    {
        fclose(f);
        printf("port: emulator host file system found: NULL reads stay faults (logged by PCSX2)\n");
        return;
    }
    /* EntryLo: PFN, cache mode 3 (cached), valid, global; no dirty bit: read only. */
    lo    = (((unsigned int)s_ZeroPage & 0x1FFFFFFF) >> 12) << 6 | (3 << 3) | (1 << 1) | 1;
    entry = PutTLBEntry(0, 0, lo, lo); /* 4 KB pages; VPN2 0: addresses 0-0x1FFF */
    printf("port: addresses 0-0x1FFF read as zeros (TLB entry %d): NULL reads don't fault\n", entry);
}

void Crash_Install(void)
{
    int cause;
    map_zero_page();
    {
        extern void Port_AgentInit(void); /* agent_ps2.c: remote control on hardware */
        Port_AgentInit();
    }
    ee_dbg_install(1);
    for (cause = 1; cause <= 5; cause++)
    {
        ee_dbg_set_level1_handler(cause, crash_handler);
    }
}
