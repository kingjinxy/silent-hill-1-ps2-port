/** @brief The Floatstinger check run from the HAL's GTE commands (gte.c SENTINEL, SH1_SENTINEL=1).
 * Moved out of gte.c when the HAL was split off; the timer-based check is in ps2/sentinel_ps2.c. */

#include "port/spr.h"

#ifdef SH_PORT_SENTINEL
extern int printf(const char* fmt, ...);

/* Floatstinger crash on hardware (docs/ToFix.md): Floatstinger_Update keeps the animation header
 * pointer at 0x01FFEB10 across func_800D35F0, and its upper half becomes 0xFC00. Checked from port code
 * that the collision code calls (GTE commands, Math_Sin/Cos), which is linked after the game's, so the
 * game's code and stack layout stay as in the crashing build. Reports the first sighting once, with
 * the caller. (The hardware data breakpoint doesn't fire under Neutrino.) */
extern char           g_CharaModelAnimsData[];
static SPR unsigned int s_SentinelSeen;
void Port_Sentinel(const char* where, unsigned int ra)
{
    unsigned int slot = *(volatile unsigned int*)0x01FFEB10;
    unsigned int hdr  = *(unsigned int*)(g_CharaModelAnimsData + 0x18 + 8); /* [1].activeAnmHdr */
    if (!s_SentinelSeen && (slot >> 16) == 0xFC00 && (slot & 0xFFFF) == (hdr & 0xFFFF) && hdr)
    {
        s_SentinelSeen = 1;
        printf("SENTINEL: 0x01FFEB10 = %08X (header %08X) first seen in %s, called from %08X\n", slot, hdr, where, ra);
        {
            extern signed char g_CharaAnimDataIdxs[];
            int                i;
            printf("SENTINEL: Floatstinger anim data index %d\n", g_CharaAnimDataIdxs[15]);
            for (i = 0; i < 4; i++)
            {
                unsigned int* e = (unsigned int*)(g_CharaModelAnimsData + i * 0x18);
                printf("SENTINEL: [%d] chara %d/%d alloc %08X hdr %08X size %08X/%08X bones %08X\n", i,
                       ((signed char*)e)[0], ((signed char*)e)[1], e[1], e[2], e[3], e[4], e[5]);
            }
        }
    }
}
/** Layout-independent: scans the stack above the caller (1 KB) for the corrupted header pointer
 * (upper half FC00, lower half the header's); prints the first checkpoint that sees it, and the address. */
void Port_SentinelScan(const char* where)
{
    static unsigned int s_Scans;
    unsigned int        hdr = *(unsigned int*)(g_CharaModelAnimsData + 0x18 + 8), *p;
    s_Scans++;
    if (s_SentinelSeen || !hdr)
    {
        return;
    }
    for (p = (unsigned int*)__builtin_frame_address(0); p < (unsigned int*)__builtin_frame_address(0) + 256; p++)
    {
        if ((*p >> 16) == 0xFC00 && (*p & 0xFFFF) == (hdr & 0xFFFF))
        {
            s_SentinelSeen = 1;
            printf("SENTINEL: %08X at %08X first seen at %s (scan %u, sp %08X)\n", *p, (unsigned int)p, where, s_Scans,
                   (unsigned int)__builtin_frame_address(0));
            return;
        }
    }
}
#endif
