/* PS1 side of the GTE test: runs every command on the real GTE and records all registers. */
#include "cmds.h"
#include "inputs.h"

extern void (*gte_test_stubs[])(void);

/* Results: per test, data[32] then ctrl[32] after the command. Read by gdb at gte_test_done. */
unsigned int gte_test_results[GTE_TEST_NUM_CMDS * GTE_TEST_PER_CMD][64];
volatile unsigned int gte_test_count;

#define R32(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) \
               X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

static void write_regs(const unsigned int* data, const unsigned int* ctrl)
{
#define CTC(n) __asm__ volatile("ctc2 %0, $" #n "\n nop\n nop" :: "r"(ctrl[n]));
#define MTC(n) if (GTE_TEST_DATA_WRITTEN(n)) __asm__ volatile("mtc2 %0, $" #n "\n nop\n nop" :: "r"(data[n]));
    R32(CTC)
    R32(MTC)
#undef CTC
#undef MTC
}

static void read_regs(unsigned int* out)
{
    unsigned int v;
#define MFC(n) __asm__ volatile("mfc2 %0, $" #n "\n nop\n nop" : "=r"(v)); out[n] = v;
#define CFC(n) __asm__ volatile("cfc2 %0, $" #n "\n nop\n nop" : "=r"(v)); out[32 + n] = v;
    R32(MFC)
    R32(CFC)
#undef MFC
#undef CFC
}

__attribute__((noinline)) void gte_test_done(void)
{
    for (;;) { }
}

int main(void)
{
    unsigned int data[32], ctrl[32];
    int c, t;
    for (c = 0; c < GTE_TEST_NUM_CMDS; c++)
    {
        for (t = 0; t < GTE_TEST_PER_CMD; t++)
        {
            gte_test_inputs(c, t, data, ctrl);
            write_regs(data, ctrl);
            gte_test_stubs[c]();
            read_regs(gte_test_results[c * GTE_TEST_PER_CMD + t]);
            gte_test_count++;
        }
    }
    gte_test_done();
    return 0;
}
