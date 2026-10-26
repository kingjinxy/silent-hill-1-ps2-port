/** @brief Host files of the play recorder (src/port/demo_rec.c). `host:rec.txt` (next to the disc
 * image; on the console, build/port/ served by UDPFS) holds `record NAME` or `play NAME`; recordings
 * are host:rec/NAME.dem. Without the file, nothing happens. */

#include <stdio.h>
#include <string.h>

/** The mode from host:rec.txt (0 none, 1 record, 2 play) and the recording's name (up to 31 chars). */
int Rec_Config(char* name)
{
    char  buf[80], word[16];
    FILE* f = fopen("host:rec.txt", "r");
    int   n;
    name[0] = 0;
    if (!f)
    {
        return 0;
    }
    n = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n > 0 ? n : 0] = 0;
    if (sscanf(buf, "%15s %31s", word, name) != 2)
    {
        return 0;
    }
    return !strcmp(word, "record") ? 1 : !strcmp(word, "play") ? 2 : 0;
}

/** Reads host:rec/NAME.dem into buf; returns the bytes read, -1 without the file. */
int Rec_Read(const char* name, void* buf, int max)
{
    char  path[64];
    FILE* f;
    int   n;
    sprintf(path, "host:rec/%s.dem", name);
    if (!(f = fopen(path, "rb")))
    {
        return -1;
    }
    n = (int)fread(buf, 1, max, f);
    fclose(f);
    return n;
}

/** Writes host:rec/NAME.dem; returns 0 when it all went. */
int Rec_Write(const char* name, const void* buf, int size)
{
    char  path[64];
    FILE* f;
    int   n;
    sprintf(path, "host:rec/%s.dem", name);
    if (!(f = fopen(path, "wb")))
    {
        return -1;
    }
    n = (int)fwrite(buf, 1, size, f);
    fclose(f);
    return n == size ? 0 : -1;
}
