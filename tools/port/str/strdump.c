/** @brief Host test of the STR decoder (src/port/ps2/strdec.c): reads a movie's raw sectors from HILL. (2336-byte
 * PS1 sectors: 8-byte subheader + 2328), assembles each frame from its video sectors (the 32-byte STR
 * header gives the chunk number and count, frame number, size and dimensions) and writes chosen
 * frames as PPM files.
 *
 *     strdump HILL. <first raw sector> <frames to read> <every Nth frame> <out prefix>
 * Build: gcc -O2 -o strdump tools/port/str/strdump.c src/port/ps2/strdec.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../src/port/ps2/strdec.h"

#define RAW     2336
#define PAYLOAD 2016

static unsigned int u16(const unsigned char* p)
{
    return p[0] | (p[1] << 8);
}

static unsigned int u32(const unsigned char* p)
{
    return u16(p) | (u16(p + 2) << 16);
}

int main(int argc, char** argv)
{
    FILE*          f;
    unsigned char  sec[RAW], frame[64 * PAYLOAD], *rgb = NULL;
    long           first;
    int            frames, every, got = 0, have = 0, cur = -1, n = 0, w = 0, h = 0;
    if (argc < 6)
    {
        fprintf(stderr, "usage: %s HILL. first_raw_sector frames every out_prefix\n", argv[0]);
        return 1;
    }
    f      = fopen(argv[1], "rb");
    first  = atol(argv[2]);
    frames = atoi(argv[3]);
    every  = atoi(argv[4]);
    if (!f || fseek(f, first * RAW, SEEK_SET))
    {
        perror(argv[1]);
        return 1;
    }
    while (got < frames && fread(sec, 1, RAW, f) == RAW)
    {
        const unsigned char* d = sec + 8;
        unsigned int         chunk, chunks, fno;
        if (!(sec[2] & 0x08) || u16(d) != 0x0160 || u16(d + 2) != 0x8001)
        {
            continue; /* audio (or not video) */
        }
        chunk  = u16(d + 4);
        chunks = u16(d + 6);
        fno    = u32(d + 8);
        if ((int)fno != cur)
        {
            cur  = (int)fno;
            have = 0;
        }
        if (chunk >= 64)
        {
            continue;
        }
        memcpy(frame + chunk * PAYLOAD, d + 32, PAYLOAD);
        have |= 1;
        n = (int)chunks;
        if (chunk == chunks - 1)
        {
            w = (int)u16(d + 16);
            h = (int)u16(d + 18);
            if (!rgb)
            {
                rgb = malloc((size_t)w * h * 3);
            }
            got++;
            if ((got - 1) % every == 0)
            {
                char  name[256];
                FILE* o;
                int   r = StrDec_Frame(frame, n * PAYLOAD, w, h, rgb, 3);
                printf("frame %u (%dx%d, %d sectors): %d\n", fno, w, h, n, r);
                snprintf(name, sizeof(name), "%s%04u.ppm", argv[5], fno);
                o = fopen(name, "wb");
                fprintf(o, "P6\n%d %d\n255\n", w, h);
                fwrite(rgb, 1, (size_t)w * h * 3, o);
                fclose(o);
            }
        }
    }
    fclose(f);
    return 0;
}
