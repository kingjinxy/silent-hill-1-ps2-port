/* libgte/libkmath equivalence test. Built twice from this file:
 *   PS1 (-DLIB_TEST_PS1): links Sony's original objects, runs in DuckStation, records results.
 *   EE: links the recompiled versions, embeds the PS1 results, compares, prints a report. */
#include "../gte_test/inputs.h"
#include "lib_tests.h"

#define PER_FUNC 8
#define ARENA    2048
#define NUM_TESTS (LIB_TEST_NUM_FUNCS * PER_FUNC)

typedef struct
{
    unsigned int  ret;
    unsigned int  gte[64];
    unsigned char arena[ARENA];
} Result;

unsigned char arena[ARENA] __attribute__((aligned(16)));

static void fill(int f, int t, unsigned int* args, unsigned int data[32], unsigned int ctrl[32])
{
    int i;
    gte_test_inputs(1000 + f, t & ~1, data, ctrl); /* realistic GTE state */
    gte_rng_state ^= (unsigned int)(f * 131 + t * 17 + 3);
    for (i = 0; i < ARENA / 2; i++)
    {
        int v = (t & 1) ? (int)(gte_rng() & 0xFFFF) : gte_rand_range(0x1000);
        ((unsigned short*)arena)[i] = (unsigned short)v;
    }
    for (i = 0; i < 9; i++)
    {
        if (lib_test_ptrs[f] & (1 << i))
        {
            args[i] = (unsigned int)(unsigned long)&arena[i * 224];
        }
        else
        {
            args[i] = (t & 1) ? gte_rng() : (unsigned int)gte_rand_range(0x1000);
        }
    }
}

#ifdef LIB_TEST_PS1

#define R32(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) \
               X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

static void write_regs(const unsigned int* data, const unsigned int* ctrl)
{
#define CTC(n) __asm__ volatile("ctc2 %0, $" #n "\n nop\n nop" :: "r"(ctrl[n]));
#define MTC(n) if (GTE_TEST_DATA_WRITTEN(n)) __asm__ volatile("mtc2 %0, $" #n "\n nop\n nop" :: "r"(data[n]));
    R32(CTC)
    R32(MTC)
}

static void read_regs(unsigned int* out)
{
    unsigned int v;
#define MFC(n) __asm__ volatile("mfc2 %0, $" #n "\n nop\n nop" : "=r"(v)); out[n] = v;
#define CFC(n) __asm__ volatile("cfc2 %0, $" #n "\n nop\n nop" : "=r"(v)); out[32 + n] = v;
    R32(MFC)
    R32(CFC)
}

Result lib_test_results[NUM_TESTS];

int printf(const char* fmt, ...) { (void)fmt; return 0; } /* libgte's error messages */

__attribute__((noinline)) void lib_test_done(void) { for (;;) { } }

int main(void)
{
    int f, t, i;
    for (f = 0; f < LIB_TEST_NUM_FUNCS; f++)
    {
        for (t = 0; t < PER_FUNC; t++)
        {
            unsigned int args[9], data[32], ctrl[32];
            Result* res = &lib_test_results[f * PER_FUNC + t];
            fill(f, t, args, data, ctrl);
            write_regs(data, ctrl);
            res->ret = lib_test_call(f, args);
            read_regs(res->gte);
            if (lib_test_ret[f] & 2)
            {
                res->ret -= (unsigned int)(unsigned long)arena; /* pointer: stored as arena offset */
            }
            for (i = 0; i < ARENA; i++) res->arena[i] = arena[i];
        }
    }
    lib_test_done();
    return 0;
}

#else /* EE */

#include <stdio.h>
#include "port/gte.h"

extern const Result ps1_results[NUM_TESTS];

int main(void)
{
    int f, t, i, bad = 0, shown = 0;
    for (f = 0; f < LIB_TEST_NUM_FUNCS; f++)
    {
        int bad_f = 0;
        for (t = 0; t < PER_FUNC; t++)
        {
            unsigned int args[9], data[32], ctrl[32], gte[64], ret;
            const Result* ref = &ps1_results[f * PER_FUNC + t];
            const char* what = 0;
            int where = 0;
            fill(f, t, args, data, ctrl);
            Gte_Reset();
            for (i = 0; i < 32; i++) Gte_CtrlWrite(i, ctrl[i]);
            for (i = 0; i < 32; i++) if (GTE_TEST_DATA_WRITTEN(i)) Gte_DataWrite(i, data[i]);
            ret = lib_test_call(f, args);
            for (i = 0; i < 32; i++) { gte[i] = Gte_DataRead(i); gte[32 + i] = Gte_CtrlRead(i); }

            if (lib_test_ret[f] & 2)
            {
                ret -= (unsigned int)(unsigned long)arena; /* pointer: compare as arena offset */
            }
            if ((lib_test_ret[f] & 1) && ret != ref->ret)
            {
                what = "return";
            }
            for (i = 0; !what && i < 64; i++) if (gte[i] != ref->gte[i]) { what = "GTE reg"; where = i; }
            for (i = 0; !what && i < ARENA; i++) if (arena[i] != ref->arena[i]) { what = "arena byte"; where = i; }
            if (what)
            {
                bad++;
                bad_f++;
                if (shown++ < 30)
                {
                    printf("MISMATCH %s test %d: %s %d (ps2=%08X ps1=%08X)\n", lib_test_names[f], t, what, where,
                           what[0] == 'r' ? ret : what[0] == 'G' ? gte[where] : arena[where],
                           what[0] == 'r' ? ref->ret : what[0] == 'G' ? ref->gte[where] : ref->arena[where]);
                }
            }
        }
        printf("%-24s %s\n", lib_test_names[f], bad_f ? "DIFFERS" : "ok");
    }
    printf("lib_test done: %d of %d tests differ\n", bad, NUM_TESTS);
    return 0;
}

#endif
