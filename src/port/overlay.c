/** @brief Port: overlay switching. Every overlay is linked in (tools/port/port_link.py); the game's
 * overlay loads still read the file (to the RAM arena, at the PS1 load address), and this makes the
 * matching linked-in overlay current.
 *
 * Maps: each keeps its own header, renamed `g_MapOverlayHdr_<map>` when the maps are merged; code
 * outside the maps reads the current one through `g_MapOverlayHdrPtr` (see bodyprog.h).
 *
 * Overlay reset (off; SH_PORT_OVERLAY_RESET): loading an overlay again would reset its variables,
 * as reading the file does on PS1 (.data restored from a copy taken at startup, .bss cleared; ranges
 * from port_link.py's markers). It fixes a map loaded twice in a row (map0_s01 in the warp sweep),
 * but breaks the second attract demo (map2_s00 again: world object lookups then read garbage), so
 * something about the PS1 overlay's memory isn't modelled yet. TODO.
 */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "main/fileinfo.h"
#include "port/overlay.h"

extern void* malloc(unsigned long size);
extern void* memcpy(void* dst, const void* src, unsigned long n);
extern void* memset(void* dst, int c, unsigned long n);
extern int   printf(const char* fmt, ...);

static char* s_Pristine[64]; /* initial .data of each overlay in g_PortOverlayRanges */

void Port_OverlaySnapshot(void)
{
    s32 i;
    u32 total = 0;
    for (i = 0; i < g_PortOverlayRangeCount && i < 64; i++)
    {
        const Port_OverlayRange* o = &g_PortOverlayRanges[i];
        u32 size = (u32)(o->dataEnd - o->dataStart);
        s_Pristine[i] = (char*)malloc(size ? size : 1);
        if (s_Pristine[i])
        {
            memcpy(s_Pristine[i], o->dataStart, size);
        }
        total += size;
    }
    printf("port: %d overlays, %u bytes of initial data saved\n", g_PortOverlayRangeCount, total);
}

/** Resets the overlay loaded by `fileIdx`, if any. */
static void overlay_reset(s32 fileIdx)
{
    s32 i;
    for (i = 0; i < g_PortOverlayRangeCount && i < 64; i++)
    {
        const Port_OverlayRange* o = &g_PortOverlayRanges[i];
        if (o->fileIdx == fileIdx)
        {
            if (s_Pristine[i])
            {
                memcpy(o->dataStart, s_Pristine[i], (u32)(o->dataEnd - o->dataStart));
            }
            memset(o->bssStart, 0, (u32)(o->bssEnd - o->bssStart));
            return;
        }
    }
}

extern s_MapOverlayHdr g_MapOverlayHdr_map0_s00;
extern s_MapOverlayHdr g_MapOverlayHdr_map0_s01;
extern s_MapOverlayHdr g_MapOverlayHdr_map0_s02;
extern s_MapOverlayHdr g_MapOverlayHdr_map1_s00;
extern s_MapOverlayHdr g_MapOverlayHdr_map1_s01;
extern s_MapOverlayHdr g_MapOverlayHdr_map1_s02;
extern s_MapOverlayHdr g_MapOverlayHdr_map1_s03;
extern s_MapOverlayHdr g_MapOverlayHdr_map1_s04;
extern s_MapOverlayHdr g_MapOverlayHdr_map1_s05;
extern s_MapOverlayHdr g_MapOverlayHdr_map1_s06;
extern s_MapOverlayHdr g_MapOverlayHdr_map2_s00;
extern s_MapOverlayHdr g_MapOverlayHdr_map2_s01;
extern s_MapOverlayHdr g_MapOverlayHdr_map2_s02;
extern s_MapOverlayHdr g_MapOverlayHdr_map2_s03;
extern s_MapOverlayHdr g_MapOverlayHdr_map2_s04;
extern s_MapOverlayHdr g_MapOverlayHdr_map3_s00;
extern s_MapOverlayHdr g_MapOverlayHdr_map3_s01;
extern s_MapOverlayHdr g_MapOverlayHdr_map3_s02;
extern s_MapOverlayHdr g_MapOverlayHdr_map3_s03;
extern s_MapOverlayHdr g_MapOverlayHdr_map3_s04;
extern s_MapOverlayHdr g_MapOverlayHdr_map3_s05;
extern s_MapOverlayHdr g_MapOverlayHdr_map3_s06;
extern s_MapOverlayHdr g_MapOverlayHdr_map4_s00;
extern s_MapOverlayHdr g_MapOverlayHdr_map4_s01;
extern s_MapOverlayHdr g_MapOverlayHdr_map4_s02;
extern s_MapOverlayHdr g_MapOverlayHdr_map4_s03;
extern s_MapOverlayHdr g_MapOverlayHdr_map4_s04;
extern s_MapOverlayHdr g_MapOverlayHdr_map4_s05;
extern s_MapOverlayHdr g_MapOverlayHdr_map4_s06;
extern s_MapOverlayHdr g_MapOverlayHdr_map5_s00;
extern s_MapOverlayHdr g_MapOverlayHdr_map5_s01;
extern s_MapOverlayHdr g_MapOverlayHdr_map5_s02;
extern s_MapOverlayHdr g_MapOverlayHdr_map5_s03;
extern s_MapOverlayHdr g_MapOverlayHdr_map6_s00;
extern s_MapOverlayHdr g_MapOverlayHdr_map6_s01;
extern s_MapOverlayHdr g_MapOverlayHdr_map6_s02;
extern s_MapOverlayHdr g_MapOverlayHdr_map6_s03;
extern s_MapOverlayHdr g_MapOverlayHdr_map6_s04;
extern s_MapOverlayHdr g_MapOverlayHdr_map6_s05;
extern s_MapOverlayHdr g_MapOverlayHdr_map7_s00;
extern s_MapOverlayHdr g_MapOverlayHdr_map7_s01;
extern s_MapOverlayHdr g_MapOverlayHdr_map7_s02;
extern s_MapOverlayHdr g_MapOverlayHdr_map7_s03;

static s_MapOverlayHdr* const MAP_HEADERS[] = {
    &g_MapOverlayHdr_map0_s00,
    &g_MapOverlayHdr_map0_s01,
    &g_MapOverlayHdr_map0_s02,
    &g_MapOverlayHdr_map1_s00,
    &g_MapOverlayHdr_map1_s01,
    &g_MapOverlayHdr_map1_s02,
    &g_MapOverlayHdr_map1_s03,
    &g_MapOverlayHdr_map1_s04,
    &g_MapOverlayHdr_map1_s05,
    &g_MapOverlayHdr_map1_s06,
    &g_MapOverlayHdr_map2_s00,
    &g_MapOverlayHdr_map2_s01,
    &g_MapOverlayHdr_map2_s02,
    &g_MapOverlayHdr_map2_s03,
    &g_MapOverlayHdr_map2_s04,
    &g_MapOverlayHdr_map3_s00,
    &g_MapOverlayHdr_map3_s01,
    &g_MapOverlayHdr_map3_s02,
    &g_MapOverlayHdr_map3_s03,
    &g_MapOverlayHdr_map3_s04,
    &g_MapOverlayHdr_map3_s05,
    &g_MapOverlayHdr_map3_s06,
    &g_MapOverlayHdr_map4_s00,
    &g_MapOverlayHdr_map4_s01,
    &g_MapOverlayHdr_map4_s02,
    &g_MapOverlayHdr_map4_s03,
    &g_MapOverlayHdr_map4_s04,
    &g_MapOverlayHdr_map4_s05,
    &g_MapOverlayHdr_map4_s06,
    &g_MapOverlayHdr_map5_s00,
    &g_MapOverlayHdr_map5_s01,
    &g_MapOverlayHdr_map5_s02,
    &g_MapOverlayHdr_map5_s03,
    &g_MapOverlayHdr_map6_s00,
    &g_MapOverlayHdr_map6_s01,
    &g_MapOverlayHdr_map6_s02,
    &g_MapOverlayHdr_map6_s03,
    &g_MapOverlayHdr_map6_s04,
    &g_MapOverlayHdr_map6_s05,
    &g_MapOverlayHdr_map7_s00,
    &g_MapOverlayHdr_map7_s01,
    &g_MapOverlayHdr_map7_s02,
    &g_MapOverlayHdr_map7_s03,
};

/** Before the first map load, PS1 would read B_KONAMI's bytes here; a valid header is safer. */
s_MapOverlayHdr* g_MapOverlayHdrPtr = &g_MapOverlayHdr_map0_s00;

void Port_OverlayActivate(s32 fileIdx)
{
    s32 mapIdx = fileIdx - FILE_VIN_MAP0_S00_BIN;

#ifdef SH_PORT_OVERLAY_RESET
    overlay_reset(fileIdx);
#else
    (void)overlay_reset;
#endif

    if (mapIdx >= 0 && mapIdx < (s32)ARRAY_SIZE(MAP_HEADERS))
    {
        g_MapOverlayHdrPtr = MAP_HEADERS[mapIdx];
    }
}
