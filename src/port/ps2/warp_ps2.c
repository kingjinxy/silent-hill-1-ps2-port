/** @brief Map warp for testing: `host:warp.txt` (next to the disc image, with PCSX2's host file
 * system on) holding a map number (0-42, as the VIN/MAPn_Snn.BIN order) or a name such as map1_s02
 * makes the first map load (the attract demo's) load that map instead, as the gdb `warp` command
 * does in DuckStation (tools/port/gdb/sh1.py). Every map load is logged ("port: map load"); used by
 * tools/port/warp_sweep_ps2.py. Without the file (or on hardware), nothing changes.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char* const MAPS[] = {
    "map0_s00", "map0_s01", "map0_s02",
    "map1_s00", "map1_s01", "map1_s02", "map1_s03", "map1_s04", "map1_s05", "map1_s06",
    "map2_s00", "map2_s01", "map2_s02", "map2_s03", "map2_s04",
    "map3_s00", "map3_s01", "map3_s02", "map3_s03", "map3_s04", "map3_s05", "map3_s06",
    "map4_s00", "map4_s01", "map4_s02", "map4_s03", "map4_s04", "map4_s05", "map4_s06",
    "map5_s00", "map5_s01", "map5_s02", "map5_s03",
    "map6_s00", "map6_s01", "map6_s02", "map6_s03", "map6_s04", "map6_s05",
    "map7_s00", "map7_s01", "map7_s02", "map7_s03",
};
#define MAP_COUNT (int)(sizeof(MAPS) / sizeof(MAPS[0]))

static int read_warp(void)
{
    char  buf[32];
    FILE* f = fopen("host:warp.txt", "r");
    int   n, i;
    if (!f)
    {
        return -1;
    }
    n = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n > 0 ? n : 0] = 0;
    for (i = 0; buf[i]; i++)
    {
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' ')
        {
            buf[i] = 0;
            break;
        }
    }
    for (i = 0; i < MAP_COUNT; i++)
    {
        if (!strcmp(buf, MAPS[i]))
        {
            return i;
        }
    }
    if (buf[0] >= '0' && buf[0] <= '9')
    {
        i = atoi(buf);
        return i < MAP_COUNT ? i : -1;
    }
    return -1;
}

/** Called by GameBoot_MapLoad: returns the map to load instead of `mapIdx`. */
int Port_MapLoadWarp(int mapIdx)
{
    static int loads, warped;
    int        warp = loads++ == 0 ? read_warp() : -1;
    if (warp >= 0)
    {
        printf("port: map load %s (warp from %s)\n", MAPS[warp], mapIdx >= 0 && mapIdx < MAP_COUNT ? MAPS[mapIdx] : "?");
        warped = 1;
        return warp;
    }
    printf("port: map load %s%s\n", mapIdx >= 0 && mapIdx < MAP_COUNT ? MAPS[mapIdx] : "?",
           warped ? " (next map load after the warp)" : "");
    warped = 0;
    return mapIdx;
}
