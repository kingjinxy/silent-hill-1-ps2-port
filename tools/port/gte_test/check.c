/* Host side of the GTE test: replays every test through src/port/gte.c and compares with results.bin
 * (registers recorded on the PS1 GTE, dumped from DuckStation). Usage: ./check results.bin [-v] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port/gte.h"
#include "cmds.h"
static unsigned int g_seed;
#define GTE_TEST_SEED g_seed
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

#ifdef _EE
static unsigned int ee_count(void)
{
    unsigned int c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}
#endif

static unsigned int hw[GTE_TEST_NUM_CMDS * GTE_TEST_PER_CMD][64];
#ifdef _EE
static unsigned long long s_Cycles[64];
static unsigned int       s_Runs[64];
#endif

/** Replays one seed's tests and compares with its recorded results; returns the differing tests. */
static int check_file(const char* path, int verbose)
{
    FILE* f;
    int c, t, r, bad_tests = 0, shown = 0;
    int bad_per_cmd[GTE_TEST_NUM_CMDS] = { 0 };
    int bad_per_reg[64] = { 0 };

    if (!(f = fopen(path, "rb")))
    {
        printf("can't open %s\n", path);
        return -1;
    }
    if (fread(hw, sizeof(hw), 1, f) != 1)
    {
        printf("short results file %s\n", path);
        fclose(f);
        return -1;
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
#ifdef _EE
            {
                unsigned int t0 = ee_count();
                Gte_Command(gte_test_cmds[c]);
                s_Cycles[gte_test_cmds[c] & 63] += ee_count() - t0;
                s_Runs[gte_test_cmds[c] & 63]++;
            }
#else
            Gte_Command(gte_test_cmds[c]);
#endif
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
    printf("%s: %d of %d tests differ\n", path, bad_tests, GTE_TEST_NUM_CMDS * GTE_TEST_PER_CMD);
    if (bad_tests)
    {
        printf("by register:");
        for (r = 0; r < 64; r++) if (bad_per_reg[r]) printf(" %s=%d", r < 32 ? NAMES_D[r] : NAMES_C[r - 32], bad_per_reg[r]);
        printf("\nby command:");
        for (c = 0; c < GTE_TEST_NUM_CMDS; c++) if (bad_per_cmd[c]) printf(" %s/%07X=%d", cmd_name(gte_test_cmds[c]), gte_test_cmds[c], bad_per_cmd[c]);
        printf("\n");
    }
    return bad_tests;
}

/* Host: check results.bin [-v] [seed]. EE (tools/port/gte_test/run_ee.py): every seed's
 * host:results_seedN.bin, then average EE cycles per command. */
int main(int argc, char** argv)
{
    int bad = 0;
#ifdef _EE
    int s, op;
    (void)argc;
    (void)argv;
    for (s = 0; s <= 6; s++)
    {
        char path[64];
        int  n;
        sprintf(path, "host:results_seed%d.bin", s);
        g_seed = (unsigned int)s;
        n      = check_file(path, 0);
        bad += n < 0 ? 1 : n;
    }
    /* Microbenchmark: each command 256 times in a row on one game-like input set (test 2: nothing
     * saturates), as the game runs them; registers are reloaded each time. */
    printf("game-like cycles (EE):");
    for (op = 0; op < GTE_TEST_NUM_CMDS; op++)
    {
        unsigned int data[32], ctrl[32], k, r, t0, total = 0, cmd = gte_test_cmds[op];
        static int done[64];
        if (((cmd >> 19) & 1) == 0 || done[cmd & 63]++)
        {
            continue; /* one variant per opcode, sf=1 */
        }
        g_seed = 0;
        gte_test_inputs(op, 2, data, ctrl);
        for (k = 0; k < 256; k++)
        {
            for (r = 0; r < 32; r++) Gte_CtrlWrite(r, ctrl[r]);
            for (r = 0; r < 32; r++) if (GTE_TEST_DATA_WRITTEN(r)) Gte_DataWrite(r, data[r]);
            t0 = ee_count();
            Gte_Command(cmd);
            total += ee_count() - t0;
        }
        printf(" %s=%u", cmd_name(cmd), total / 256);
    }
    {
        unsigned int k, t0, empty = 0, nothing = 0;
        for (k = 0; k < 256; k++)
        {
            t0 = ee_count();
            Gte_Command(0);
            empty += ee_count() - t0;
            t0 = ee_count();
            nothing += ee_count() - t0;
        }
        printf(" (baselines: no-op command=%u, timer only=%u)", empty / 256, nothing / 256);
    }
    printf("\n");
    printf("cycles per command (EE):");
    for (op = 0; op < 64; op++)
    {
        if (s_Runs[op])
        {
            printf(" %s=%llu", cmd_name((unsigned int)op), s_Cycles[op] / s_Runs[op]);
        }
    }
    printf("\ngte_test_ee: %s\n", bad ? "FAILED" : "all tests match");
#else
    if (argc < 2)
    {
        fprintf(stderr, "usage: check results.bin [-v] [seed]\n");
        return 2;
    }
    g_seed = argc > 3 ? (unsigned int)atoi(argv[3]) : (unsigned int)GTE_TEST_DEFAULT_SEED;
    bad    = check_file(argv[1], argc > 2 && !strcmp(argv[2], "-v"));
#endif
    return bad != 0;
}
