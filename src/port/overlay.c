/** @brief Port: overlay switching. Every overlay is linked in (tools/port/port_link.py); the game's
 * overlay loads still read the file (to the RAM arena, at the PS1 load address), and this makes the
 * matching linked-in overlay current.
 *
 * Maps: each keeps its own header, renamed `g_MapOverlayHdr_<map>` when the maps are merged; code
 * outside the maps reads the current one through `g_MapOverlayHdrPtr` (see bodyprog.h).
 *
 * TODO: restore an overlay's .data/.bss to their initial values when it's "loaded" again, as reading
 * the file does on PS1.
 */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "main/fileinfo.h"

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

    if (mapIdx >= 0 && mapIdx < (s32)ARRAY_SIZE(MAP_HEADERS))
    {
        g_MapOverlayHdrPtr = MAP_HEADERS[mapIdx];
    }
}
