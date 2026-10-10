/** @brief Crash reporter: on a TLB miss or address error, prints the faulting state, then restarts.
 *
 * The level-1 exception handler (ps2sdk's ee_debug) saves the register frame, then returns to
 * crash_report() instead of the faulting instruction, so the report is printed from normal thread
 * context (where printf works; on hardware under Neutrino it goes over the network: ministack's
 * udptty, tools/port/ps2_log.py).
 *
 * Then it keeps the crashed state (tools/port/ps2_ctl.py md reads memory, ps2_ctl.py restart
 * restarts). With SH1_CRASH_RESTART=1 it instead counts down 6 s and restarts the game from its disc (Port_Restart(), also used by agent_ps2.c):
 * under Neutrino that reloads it from the image on the VM. Holding Triangle for 3 s during the
 * countdown keeps the crashed state instead (RESET restarts). An IOP reset of our own (to restart
 * Neutrino from the USB stick) doesn't work under Neutrino: the module loader never answers after
 * it. */

#include <kernel.h>
#include <ee_debug.h>
#include <sifrpc.h>
#include <libcdvd.h>
#include <stdio.h>
#include <string.h>

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
#ifdef SH_PORT_WATCH_PACKET
extern void* GsOUT_PACKET_P; /* libgs: the watched pointer */
static int   s_Watch;        /* the report is for the watch (watch_handler), not a crash */
#endif

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
    extern void Port_SpuShutdown(void);   /* spu_ps2.c */
    extern void Port_AgentQuiesce(void);  /* agent_ps2.c */
    extern void Port_PadShutdown(void);   /* libpad_ps2.c */
    unsigned int t0, t;
    int          i;
    printf("port: restarting %s\n", RELAUNCH_ELF);
    /* Quiet first. Under Neutrino the next ELF is loaded before the IOP reboots, so anything on the
     * IOP still writing into EE memory (PADMAN's pad data every vertical blank, a disc read, sound
     * status) writes into the new program; with a new build, into its code or data (the boot freezes
     * after deploys). And the SPU2 isn't reset by the IOP reboot: its sound data input is stopped. */
    Port_PadShutdown();
    for (i = 0; i < 60 && sceCdSync(1); i++) /* a disc read in flight (up to a second) */
    {
        VSync(0);
    }
    Port_SpuShutdown();
    VSync(0); /* let the messages go out before the network modules go away */
    VSync(0);
    Port_AgentQuiesce();
    __asm__ volatile("mfc0 %0, $9" : "=r"(t0));
    do /* the EE-to-IOP SIF DMA idle (up to 100 ms) */
    {
        __asm__ volatile("mfc0 %0, $9" : "=r"(t));
    } while ((*(volatile unsigned int*)0x1000C400 & 0x100) && t - t0 < 29491200u);
    stage_color(0xC00000); /* blue: restarting */
    LoadExecPS2(RELAUNCH_ELF, 0, NULL);
    stage_color(0xC000C0); /* magenta: LoadExecPS2 returned */
}

/** 6 s countdown (over the network log); Triangle held for 3 s keeps the crashed state. */
#ifdef SH_PORT_CRASH_RESTART
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

#endif

static int crash_handler(EE_RegFrame* frame);

static void crash_report(void)
{
    {
        extern void Port_LogDirect(void); /* log_ps2.c */
        Port_LogDirect();
    }
#ifdef SH_PORT_WATCH_PACKET
    if (s_Watch)
    {
        printf("CRASH: WATCH: GsOUT_PACKET_P set to %08X by the store at pc=%08X (ra=%08X)\n",
               *(unsigned int*)&GsOUT_PACKET_P, s_Frame.epc, s_Frame.ra[0]);
    }
#endif
    static const char* const CAUSE[] = { "int", "TLB mod", "TLB load", "TLB store", "addr load", "addr store",
                                         "bus ifetch", "bus data", "syscall", "break", "reserved instruction",
                                         "coprocessor unusable", "overflow", "trap" };
    int code = (s_Frame.cause >> 2) & 31;
    printf("CRASH: %s at pc=%08X badvaddr=%08X ra=%08X sp=%08X\n", code < 14 ? CAUSE[code] : "?", s_Frame.epc,
           s_Frame.badvaddr, s_Frame.ra[0], s_Frame.sp[0]);
    printf("CRASH: a0=%08X a1=%08X a2=%08X a3=%08X v0=%08X v1=%08X\n", s_Frame.a0[0], s_Frame.a1[0], s_Frame.a2[0],
           s_Frame.a3[0], s_Frame.v0[0], s_Frame.v1[0]);
    printf("CRASH: s0=%08X s1=%08X s2=%08X s3=%08X s4=%08X s5=%08X s6=%08X s7=%08X\n", s_Frame.s0[0], s_Frame.s1[0],
           s_Frame.s2[0], s_Frame.s3[0], s_Frame.s4[0], s_Frame.s5[0], s_Frame.s6[0], s_Frame.s7[0]);
#ifdef SH_PORT_CRASH_RESTART
    countdown_and_restart();
#else
    /* Default: keep the crashed state for inspection (tools/port/ps2_ctl.py md reads memory; the
     * agent thread still runs), restart with ps2_ctl.py restart (or deploy). SH1_CRASH_RESTART=1
     * builds count down and restart by themselves instead. */
    printf("CRASH: state kept (ps2_ctl.py md to read memory, ps2_ctl.py restart to restart)\n");
    stay();
#endif
}

#ifdef SH_PORT_WATCH_PACKET
/* Debugging (SH1_WATCH_PACKET=1): a hardware data breakpoint on stores of 0x0058xxxx to
 * GsOUT_PACKET_P (on a real PS2 the packet pointer was moved into the game's variables around
 * g_OtTags1, and meshes overwrote g_WorldMapWork). The debug exception reports the storing
 * instruction through the crash reporter. */
extern unsigned char g_PsxRam[]; /* src/port/psx_mem.c */

static int in_psx_ram(unsigned int p)
{
    return p >= (unsigned int)g_PsxRam && p < (unsigned int)g_PsxRam + 0x200000;
}

/* Also watched: the first primitive header of each model GsSortObject4J draws (on a real PS2 one
 * got the object's primitive count written over its first two bytes). */
#define WATCH_MAX 32
static unsigned int s_WatchAddr[WATCH_MAX], s_WatchVal[WATCH_MAX];
static int          s_WatchCount;
static const char*  s_WatchLast = "(start)";

static void watch_stop(void)
{
    printf("CRASH: state kept (ps2_ctl.py md to read memory, ps2_ctl.py restart to restart)\n");
    for (;;)
    {
        SleepThread();
    }
}

static void watch_check(const char* when, const char* name)
{
    int i;
    for (i = 0; i < s_WatchCount; i++)
    {
        unsigned int v = *(volatile unsigned int*)s_WatchAddr[i];
        if (v != s_WatchVal[i])
        {
            printf("CRASH: WATCH: model primitive header at %08X changed from %08X to %08X %s %s (previous call: %s)\n",
                   s_WatchAddr[i], s_WatchVal[i], v, when, name, s_WatchLast);
            watch_stop();
        }
    }
}

/** Around every wrapped recompiled call (include/port/recomp.h): the packet pointer before. */
unsigned int Port_WatchBegin(void)
{
    watch_check("before", "the next recompiled call");
    return (unsigned int)GsOUT_PACKET_P;
}

/** ...and after: moved out of the PS1 RAM area by this call: report and keep the state. */
void Port_WatchEnd(unsigned int before, const char* name, unsigned int a0, unsigned int a1, unsigned int a2,
                   unsigned int a3, unsigned int ret)
{
    if (!strncmp(name, "GsTMDfast", 9))
    {
        printf("port: %s(%08X, %08X, %08X, %08X) = %08X\n", name, a0, a1, a2, a3, ret);
    }
    unsigned int after = (unsigned int)GsOUT_PACKET_P;
    if (!strcmp(name, "GsLinkObject4"))
    {
        /* GsLinkObject4 rewrites primitive headers on purpose (groups of one type: count + mode). */
        int i;
        for (i = 0; i < s_WatchCount; i++)
        {
            s_WatchVal[i] = *(volatile unsigned int*)s_WatchAddr[i];
        }
    }
    watch_check("during", name);
    s_WatchLast = name;
    if (!strcmp(name, "GsSortObject4J") && a0 >= 0x00100000 && a0 < 0x02000000)
    {
        /* GsSortObject4J(GsDOBJ2*): its TMD object entry (tmd field), primitive table (+16). */
        unsigned int obj = *(unsigned int*)(a0 + 8), prim, hdr;
        int          i, known = 0;
        if (in_psx_ram(obj))
        {
            prim = *(unsigned int*)(obj + 16);
            if (in_psx_ram(prim))
            {
                hdr = *(unsigned int*)prim;
                for (i = 0; i < s_WatchCount; i++)
                {
                    known |= s_WatchAddr[i] == prim;
                }
                if (!known && s_WatchCount < WATCH_MAX && (hdr & 0xFF) <= 32 && ((hdr >> 8) & 0xFF) <= 32)
                {
                    s_WatchAddr[s_WatchCount] = prim;
                    s_WatchVal[s_WatchCount]  = hdr;
                    s_WatchCount++;
                    printf("port: watching model primitive header at %08X (%08X)\n", prim, hdr);
                }
            }
        }
    }
    if (!strcmp(name, "GsMapModelingData") && a0 >= 0x00100000 && a0 < 0x02000000)
    {
        /* a0: the TMD's flags word (id before it); objects from a0 + 8, 7 words each, primitive
         * table pointer at +16 (relocated by now). Watch every object's first primitive header. */
        unsigned int nobj = *(unsigned int*)(a0 + 4), k;
        for (k = 0; k < nobj && k < 64 && s_WatchCount < WATCH_MAX; k++)
        {
            unsigned int prim = *(unsigned int*)(a0 + 8 + k * 28 + 16);
            if (in_psx_ram(prim))
            {
                s_WatchAddr[s_WatchCount] = prim;
                s_WatchVal[s_WatchCount]  = *(unsigned int*)prim;
                s_WatchCount++;
            }
        }
        printf("port: watching the first primitive headers of a TMD at %08X (%u objects; %d watched)\n", a0 - 4,
               nobj, s_WatchCount);
    }
    if (in_psx_ram(before) && !in_psx_ram(after))
    {
        printf("CRASH: WATCH: %s(%08X, %08X, %08X, %08X) moved GsOUT_PACKET_P from %08X to %08X\n", name, a0, a1,
               a2, a3, before, after);
        watch_stop();
    }
}

static int watch_handler(EE_RegFrame* frame)
{
    s_Watch    = 1;
    s_Frame    = *frame;
    frame->epc = (u32)crash_report;
    ee_dbg_clr_bpda();
    ee_dbg_clr_bpdv();
    return 1;
}
#endif

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
#ifdef SH_PORT_WATCH_PACKET
    ee_dbg_install(3); /* level 1 (crashes) and level 2 (debug exception: the watch) */
    ee_dbg_set_level2_handler(EE_EXC2_DBG, watch_handler);
    ee_dbg_set_bpw((u32)&GsOUT_PACKET_P, 0xFFFFFFFF, EE_BPC_DUE | EE_BPC_DSE | EE_BPC_DKE | EE_BPC_DXE);
    ee_dbg_set_bpv(0x00580000, 0xFFFF0000, EE_BPC_DUE | EE_BPC_DSE | EE_BPC_DKE | EE_BPC_DXE);
    printf("port: watching stores of 0x0058xxxx to GsOUT_PACKET_P (hardware breakpoint)\n");
#ifdef SH_PORT_WATCH_TEST
    {
        void* saved = GsOUT_PACKET_P;
        printf("port: watch test: storing 0x00585555\n");
        *(void* volatile*)&GsOUT_PACKET_P = (void*)0x00585555;
        GsOUT_PACKET_P = saved;
        printf("port: watch test: the breakpoint did NOT fire\n");
    }
#endif
#else
    ee_dbg_install(1);
#endif
    /* TLB and address errors, bus errors, reserved instruction, overflow, and traps: GCC turns a
     * provable NULL dereference into `teq zero, zero`, and an unhandled trap left the game thread
     * spinning in the kernel (Demo cutscene map6_s04 func_800E2950). */
    for (cause = 1; cause <= 13; cause++)
    {
        if (cause != 8 && cause != 9 && cause != 11) /* not syscall, break or coprocessor unusable */
        {
            ee_dbg_set_level1_handler(cause, crash_handler);
        }
    }
}
