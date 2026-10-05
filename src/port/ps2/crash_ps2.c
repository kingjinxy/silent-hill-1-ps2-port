/** @brief Crash reporter: on a TLB miss or address error, prints the faulting state and stops.
 *
 * The level-1 exception handler (ps2sdk's ee_debug) saves the register frame, then returns to
 * crash_report() instead of the faulting instruction, so the report is printed from normal thread
 * context (where printf works).
 */

#include <kernel.h>
#include <ee_debug.h>
#include <stdio.h>

static EE_RegFrame s_Frame;

static void crash_report(void)
{
    static const char* const CAUSE[] = { "int", "TLB mod", "TLB load", "TLB store", "addr load", "addr store" };
    int code = (s_Frame.cause >> 2) & 31;
    printf("CRASH: %s at pc=%08X badvaddr=%08X ra=%08X sp=%08X\n", code < 6 ? CAUSE[code] : "?", s_Frame.epc,
           s_Frame.badvaddr, s_Frame.ra[0], s_Frame.sp[0]);
    printf("CRASH: a0=%08X a1=%08X a2=%08X a3=%08X v0=%08X v1=%08X\n", s_Frame.a0[0], s_Frame.a1[0], s_Frame.a2[0],
           s_Frame.a3[0], s_Frame.v0[0], s_Frame.v1[0]);
    printf("CRASH: s0=%08X s1=%08X s2=%08X s3=%08X s4=%08X s5=%08X s6=%08X s7=%08X\n", s_Frame.s0[0], s_Frame.s1[0],
           s_Frame.s2[0], s_Frame.s3[0], s_Frame.s4[0], s_Frame.s5[0], s_Frame.s6[0], s_Frame.s7[0]);
    for (;;)
    {
        SleepThread();
    }
}

static int crash_handler(EE_RegFrame* frame)
{
    s_Frame   = *frame;
    frame->epc = (u32)crash_report;
    return 1;
}

void Crash_Install(void)
{
    int cause;
    ee_dbg_install(1);
    for (cause = 1; cause <= 5; cause++)
    {
        ee_dbg_set_level1_handler(cause, crash_handler);
    }
}
