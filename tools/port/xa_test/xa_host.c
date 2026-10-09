/* Runs sh1spu's XA decoder and Gaussian resampler (src/port/iop/sh1spu/xa.c, built natively with
 * tools/port/xa_test/host/ stand-ins) on one file/channel of HILL. and writes the 48 kHz output as a
 * raw stereo s16 file, for comparison with tools/port/xa_test/xa_ref.py. Built and run by
 * tools/port/xa_test/run.sh. Usage: xa_host HILL. index file chan out.raw */
#include "xa.c"
#include <stdlib.h>

int main(int argc, char** argv)
{
    FILE* in  = fopen(argv[1], "rb");
    long  idx = atol(argv[2]);
    int   file = atoi(argv[3]), chan = atoi(argv[4]);
    FILE* out = fopen(argv[5], "wb");
    u8    raw[2336];
    int   eof = 0, empty = 0;
    static u8 half[HALF_FRAMES * 4];
    s_Adma = half;
    reset_decoder();
    while (empty < 2)
    {
        /* Keep the FIFO topped up, then render one half as the mixer would. */
        while (!eof && s_FifoIn - s_FifoOut < FIFO_N)
        {
            fseek(in, idx * 2336, SEEK_SET);
            if (fread(raw, 1, 2336, in) != 2336)
            {
                eof = 1;
                break;
            }
            idx++;
            if (raw[0] != file || raw[1] != chan || !(raw[2] & 0x04))
            {
                continue;
            }
            memcpy(s_Fifo[s_FifoIn % FIFO_N].data, raw + 8, XA_DATA);
            s_Fifo[s_FifoIn % FIFO_N].coding = raw[3];
            s_FifoIn++;
            eof = (raw[2] & 0x80) != 0;
        }
        fill(half);
        /* De-block: blocks of BLOCK left then BLOCK right samples -> interleaved frames. */
        {
            int b, k;
            for (b = 0; b < HALF_FRAMES / BLOCK; b++)
            {
                s16* blk = (s16*)(half + b * BLOCK * 4);
                for (k = 0; k < BLOCK; k++)
                {
                    fwrite(&blk[k], 2, 1, out);
                    fwrite(&blk[BLOCK + k], 2, 1, out);
                }
            }
        }
        if (eof && s_FifoOut == s_FifoIn)
        {
            empty++;
        }
    }
    fclose(out);
    return 0;
}
