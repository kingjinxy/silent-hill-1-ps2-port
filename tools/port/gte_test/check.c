/* Host side of the GTE test: replays every test through src/port/gte.c and compares with results.bin
 * (registers recorded on the PS1 GTE, dumped from DuckStation). Usage: ./check results.bin [-v] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port/gte.h"
#include "cmds.h"
#include "inputs.h"

static const char* NAMES_D[32] = { "VXY0", "VZ0", "VXY1", "VZ1", "VXY2", "VZ2", "RGBC", "OTZ", "IR0", "IR1", "IR2",
    "IR3", "SXY0", "SXY1", "SXY2", "SXYP", "SZ0", "SZ1", "SZ2", "SZ3", "RGB0", "RGB1", "RGB2", "RES1", "MAC0",
    "MAC1", "MAC2", "MAC3", "IRGB", "ORGB", "LZCS", "LZCR" };
static const char* NAMES_C[32] = { "RT0", "RT1", "RT2", "RT3", "RT4", "TRX", "TRY", "TRZ", "LLM0", "LLM1", "LLM2",
    "LLM3", "LLM4", "RBK", "GBK", "BBK", "LCM0", "LCM1", "LCM2", "LCM3", "LCM4", "RFC", "GFC", "BFC", "OFX", "OFY",
    "H", "DQA", "DQB", "ZSF3", "ZSF4", "FLAG" };

static const char* cmd_name(unsigned int c)
{
    switch (c & 0x3F)
    {
        case 0x01: return "RTPS"; case 0x30: return "RTPT"; case 0x06: return "NCLIP"; case 0x0C: return "OP";
        case 0x10: return "DPCS"; case 0x11: return "INTPL"; case 0x12: return "MVMVA"; case 0x13: return "NCDS";
        case 0x14: return "CDP"; case 0x16: return "NCDT"; case 0x1B: return "NCCS"; case 0x1C: return "CC";
        case 0x1E: return "NCS"; case 0x20: return "NCT"; case 0x28: return "SQR"; case 0x29: return "DCPL";
        case 0x2A: return "DPCT"; case 0x2D: return "AVSZ3"; case 0x2E: return "AVSZ4"; case 0x3D: return "GPF";
        case 0x3E: return "GPL"; case 0x3F: return "NCCT";
    }
    return "?";
}

int main(int argc, char** argv)
{
    FILE* f;
    static unsigned int hw[GTE_TEST_NUM_CMDS * GTE_TEST_PER_CMD][64];
    int verbose = argc > 2 && !strcmp(argv[2], "-v");
    int c, t, r, bad_tests = 0, shown = 0;
    int bad_per_cmd[GTE_TEST_NUM_CMDS] = { 0 };
    int bad_per_reg[64] = { 0 };

    if (argc < 2 || !(f = fopen(argv[1], "rb")))
    {
        fprintf(stderr, "usage: check results.bin [-v]\n");
        return 2;
    }
    if (fread(hw, sizeof(hw), 1, f) != 1)
    {
        fprintf(stderr, "short results file\n");
        return 2;
    }
    fclose(f);

    for (c = 0; c < GTE_TEST_NUM_CMDS; c++)
    {
        for (t = 0; t < GTE_TEST_PER_CMD; t++)
        {
            unsigned int data[32], ctrl[32], sw[64];
            const unsigned int* ref = hw[c * GTE_TEST_PER_CMD + t];
            int mism = 0;
            gte_test_inputs(c, t, data, ctrl);
            Gte_Reset();
            for (r = 0; r < 32; r++) Gte_CtrlWrite(r, ctrl[r]);
            for (r = 0; r < 32; r++) if (GTE_TEST_DATA_WRITTEN(r)) Gte_DataWrite(r, data[r]);
            Gte_Command(gte_test_cmds[c]);
            for (r = 0; r < 32; r++) { sw[r] = Gte_DataRead(r); sw[32 + r] = Gte_CtrlRead(r); }
            for (r = 0; r < 64; r++)
            {
                if (sw[r] != ref[r])
                {
                    mism++;
                    bad_per_reg[r]++;
                    if (verbose || shown < 25)
                    {
                        printf("cmd %3d %-5s %07X test %d: %-4s hw=%08X sw=%08X\n", c, cmd_name(gte_test_cmds[c]),
                               gte_test_cmds[c], t, r < 32 ? NAMES_D[r] : NAMES_C[r - 32], ref[r], sw[r]);
                        shown++;
                    }
                }
            }
            if (mism) { bad_tests++; bad_per_cmd[c]++; }
        }
    }
    printf("\n%d of %d tests differ\n", bad_tests, GTE_TEST_NUM_CMDS * GTE_TEST_PER_CMD);
    if (bad_tests)
    {
        printf("by register:");
        for (r = 0; r < 64; r++) if (bad_per_reg[r]) printf(" %s=%d", r < 32 ? NAMES_D[r] : NAMES_C[r - 32], bad_per_reg[r]);
        printf("\nby command:");
        for (c = 0; c < GTE_TEST_NUM_CMDS; c++) if (bad_per_cmd[c]) printf(" %s/%07X=%d", cmd_name(gte_test_cmds[c]), gte_test_cmds[c], bad_per_cmd[c]);
        printf("\n");
    }
    return bad_tests != 0;
}
