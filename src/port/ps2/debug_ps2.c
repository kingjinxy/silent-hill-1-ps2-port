/** @brief Debug helpers for test runs in PCSX2 (host file system on; nothing happens without it).
 *
 * Port_RamDump writes the EE's RAM to a host file (the first megabyte, the kernel's, as zeros), as a
 * PCSX2 save state would hold it: tools/port/world_diag.py --ps2-ram reads it.
 */

#include <stdio.h>
#include <string.h>

/** Writes EE RAM (32 MB) to `path` (e.g. "host:ramdump.bin"); returns 0 on failure. */
int Port_RamDump(const char* path)
{
    static char zeros[0x10000];
    FILE*       f = fopen(path, "wb");
    int         i;
    if (!f)
    {
        return 0;
    }
    for (i = 0; i < 0x100000; i += sizeof(zeros))
    {
        fwrite(zeros, 1, sizeof(zeros), f);
    }
    fwrite((const void*)0x100000, 1, 0x2000000 - 0x100000, f);
    fclose(f);
    return 1;
}
