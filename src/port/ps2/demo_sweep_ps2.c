/** @brief Demo sweep settings from host:demo.txt (PCSX2: the folder of the ISO or ELF).
 *
 * "sweep FIRST [TIMEOUT [LAST]]": play the Demo list from cutscene FIRST to LAST (default: the end),
 * one after another, giving each TIMEOUT seconds (default 300) to return to gameplay (src/port/demo_menu.c). Used by
 * tools/port/demo_sweep.py. Without the file (on hardware, no host: device) nothing happens.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int Port_DemoSweepConfig(int* first, int* timeout, int* last)
{
    char  buf[64];
    FILE* f = fopen("host:demo.txt", "r");
    int   n;
    if (!f)
    {
        return 0;
    }
    n = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n > 0 ? n : 0] = 0;
    *first   = 0;
    *timeout = 300;
    *last    = 9999;
    if (sscanf(buf, "sweep %d %d %d", first, timeout, last) < 1)
    {
        return 0;
    }
    return 1;
}
