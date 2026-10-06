/** @brief Map warp for testing. `host:warp.txt` (next to the disc image, with PCSX2's host file system
 * on) holds map names (map1_s02) or numbers (0-42, in VIN/MAPn_Snn.BIN order), and optionally
 * `seconds=N`. The first attract demo (and every later run of the same demo) loads the next map from
 * the list instead of its own, as the gdb `warp` command does in DuckStation (tools/port/gdb/sh1.py),
 * so one boot visits every listed map, always with the same demo's state. With `seconds=N`, the port
 * presses L2 after N emulated seconds in a map (any button ends a demo; L2 does nothing on the title
 * screen); the other demos in the cycle are ended after 5 seconds. Map loads are logged ("port: map load"); used by tools/port/warp_sweep_ps2.py.
 * Without the file (or on hardware), nothing changes.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern int Port_VBlanks(unsigned int* cycles); /* libetc_ps2.c */

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
#define LIST_MAX  64

static int s_List[LIST_MAX];
static int s_ListCount;
static int s_ListNext;
static int s_Seconds;     /* 0: let each demo run to its end */
static int s_Loaded = -1; /* list read: -1 not yet, 0 no file, 1 yes */
static int s_WarpedAt = -1; /* vertical blank count at the current warped map's load, -1 none */
static int s_DemoMap = -1;  /* the warped demo's own map (the first map load seen) */
static int s_SkipAt = -1;   /* another demo: vertical blank count at its load */
static int s_Ended;         /* "ending demo" logged for the current map */

static const char* map_name(int idx)
{
    return idx >= 0 && idx < MAP_COUNT ? MAPS[idx] : "?";
}

static int parse_map(const char* tok)
{
    int i;
    for (i = 0; i < MAP_COUNT; i++)
    {
        if (!strcmp(tok, MAPS[i]))
        {
            return i;
        }
    }
    if (tok[0] >= '0' && tok[0] <= '9')
    {
        i = atoi(tok);
        return i < MAP_COUNT ? i : -1;
    }
    return -1;
}

static void read_list(void)
{
    static char buf[1024];
    FILE*       f = fopen("host:warp.txt", "r");
    char*       tok;
    int         n;
    s_Loaded = 0;
    if (!f)
    {
        return;
    }
    n = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n > 0 ? n : 0] = 0;
    for (tok = strtok(buf, " ,\t\r\n"); tok; tok = strtok(NULL, " ,\t\r\n"))
    {
        int idx;
        if (!strncmp(tok, "seconds=", 8))
        {
            s_Seconds = atoi(tok + 8);
            continue;
        }
        idx = parse_map(tok);
        if (idx >= 0 && s_ListCount < LIST_MAX)
        {
            s_List[s_ListCount++] = idx;
        }
    }
    s_Loaded = s_ListCount > 0;
    if (s_Loaded)
    {
        printf("port: warp list of %d maps, %d s each\n", s_ListCount, s_Seconds);
    }
}

/** Called by GameBoot_MapLoad: returns the map to load instead of `mapIdx`. */
int Port_MapLoadWarp(int mapIdx)
{
    unsigned int cycles;
    if (s_Loaded < 0)
    {
        read_list();
    }
    if (s_WarpedAt >= 0)
    {
        printf("port: map load %s (next map load after the warp)\n", map_name(mapIdx));
        s_WarpedAt = -1;
    }
    if (s_DemoMap < 0)
    {
        s_DemoMap = mapIdx;
    }
    s_Ended = 0;
    if (s_Loaded > 0 && s_ListNext < s_ListCount && mapIdx != s_DemoMap)
    {
        /* Another demo of the cycle: end it soon. */
        printf("port: map load %s (other demo, ending it)\n", map_name(mapIdx));
        s_SkipAt = Port_VBlanks(&cycles);
        return mapIdx;
    }
    if (s_Loaded > 0 && s_ListNext < s_ListCount)
    {
        int warp = s_List[s_ListNext++];
        printf("port: map load %s (warp from %s, %d of %d)\n", map_name(warp), map_name(mapIdx), s_ListNext, s_ListCount);
        s_WarpedAt = Port_VBlanks(&cycles);
        return warp;
    }
    if (s_Loaded > 0 && s_ListNext == s_ListCount)
    {
        printf("port: warp list done\n");
        s_ListNext++;
    }
    printf("port: map load %s\n", map_name(mapIdx));
    return mapIdx;
}

/** Buttons for libpad_ps2.c to press (PS1 bit order, set = pressed): L2 for half a second once the
 * current warped map has run for `seconds=N`, to end the demo. */
unsigned int Port_WarpButtons(void)
{
    unsigned int cycles;
    int          t;
    if (s_SkipAt >= 0)
    {
        t = Port_VBlanks(&cycles) - s_SkipAt - 5 * 60; /* once the demo plays (pressing while it loads hung) */
        if (t >= 30)
        {
            s_SkipAt = -1;
        }
        return t >= 0 && t < 30 ? 1u << 8 : 0; /* L2 */
    }
    if (s_WarpedAt < 0 || s_Seconds <= 0)
    {
        return 0;
    }
    t = Port_VBlanks(&cycles) - s_WarpedAt - s_Seconds * 60;
    if (t >= 0 && !s_Ended)
    {
        printf("port: ending demo\n");
        s_Ended = 1;
    }
    return t >= 0 && t < 30 ? 1u << 8 : 0; /* L2 */
}
