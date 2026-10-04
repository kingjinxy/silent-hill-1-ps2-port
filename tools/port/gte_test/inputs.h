/* Shared by the PS1 test program and the host checker: deterministic GTE register inputs. */
#ifndef GTE_TEST_INPUTS_H
#define GTE_TEST_INPUTS_H

#define GTE_TEST_PER_CMD 8

#ifndef GTE_TEST_SEED
#define GTE_TEST_SEED 0
#endif

static unsigned int gte_rng_state;

static unsigned int gte_rng(void)
{
    unsigned int x = gte_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return gte_rng_state = x;
}

/* Signed value in [-range, range). */
static int gte_rand_range(int range)
{
    return (int)(gte_rng() % (unsigned int)(range * 2)) - range;
}

/* Two random signed 16-bit halves. The RNG calls are sequenced explicitly: argument evaluation
 * order differs between compilers (MIPS vs host), which would desynchronise the two sides. */
static unsigned int gte_rand_pair(int range)
{
    int lo = gte_rand_range(range);
    int hi = gte_rand_range(range);
    return ((unsigned int)lo & 0xFFFF) | ((unsigned int)hi << 16);
}

/* Fills data[32] and ctrl[32] for test `t` of command `c`. Odd tests are fully random (saturation,
 * flags, overflow paths); even tests use realistic ranges. */
static void gte_test_inputs(int c, int t, unsigned int data[32], unsigned int ctrl[32])
{
    int i;
    gte_rng_state = 0x9E3779B9u ^ (unsigned int)(c * 977 + t * 7919 + 1) ^ ((unsigned int)GTE_TEST_SEED * 0x85EBCA6Bu);
    for (i = 0; i < 16; i++)
    {
        gte_rng();
    }
    if (t & 1)
    {
        for (i = 0; i < 32; i++)
        {
            data[i] = gte_rng();
            ctrl[i] = gte_rng();
        }
        ctrl[26] &= 0xFFFF; /* H */
        return;
    }
    for (i = 0; i < 32; i++)
    {
        data[i] = 0;
        ctrl[i] = 0;
    }
    for (i = 0; i < 3; i++)
    {
        data[i * 2]     = gte_rand_pair(0x1000);
        data[i * 2 + 1] = (unsigned int)gte_rand_range(0x1000);
    }
    data[6] = gte_rng();                                 /* RGBC */
    data[8] = (unsigned int)(gte_rng() % 0x1001);        /* IR0 */
    for (i = 9; i < 12; i++)
    {
        data[i] = (unsigned int)gte_rand_range(0x1000); /* IR1-3 */
    }
    for (i = 12; i < 15; i++)
    {
        data[i] = gte_rand_pair(0x400); /* SXY0-2 */
    }
    for (i = 16; i < 20; i++)
    {
        data[i] = gte_rng() % 0x10000;                   /* SZ0-3 */
    }
    for (i = 20; i < 23; i++)
    {
        data[i] = gte_rng();                             /* RGB0-2 */
    }
    data[24] = gte_rng();
    for (i = 25; i < 28; i++)
    {
        data[i] = (unsigned int)gte_rand_range(0x8000);  /* MAC1-3 */
    }
    for (i = 0; i < 5; i++)                              /* RT, LLM, LCM: unit-scale */
    {
        ctrl[i]      = gte_rand_pair(0x1000);
        ctrl[8 + i]  = gte_rand_pair(0x1000);
        ctrl[16 + i] = gte_rand_pair(0x1000);
    }
    for (i = 5; i < 8; i++)
    {
        ctrl[i] = (unsigned int)gte_rand_range(0x4000); /* TR */
    }
    ctrl[7] = (unsigned int)(gte_rng() % 0x8000);        /* TRZ in front of the camera */
    for (i = 13; i < 16; i++)
    {
        ctrl[i] = (unsigned int)gte_rand_range(0x100000); /* BK */
    }
    for (i = 21; i < 24; i++)
    {
        ctrl[i] = (unsigned int)gte_rand_range(0x1000);   /* FC */
    }
    ctrl[24] = (unsigned int)gte_rand_range(0x1000000);   /* OFX */
    ctrl[25] = (unsigned int)gte_rand_range(0x1000000);   /* OFY */
    ctrl[26] = 0x100 + gte_rng() % 0x200;                 /* H */
    ctrl[27] = (unsigned int)gte_rand_range(0x8000);      /* DQA */
    ctrl[28] = (unsigned int)gte_rand_range(0x2000000);   /* DQB */
    ctrl[29] = (unsigned int)gte_rand_range(0x1000);      /* ZSF3 */
    ctrl[30] = (unsigned int)gte_rand_range(0x1000);      /* ZSF4 */
}

/* Data registers written as inputs (skips SXYP, IRGB, ORGB, LZCR, which alias others). */
#define GTE_TEST_DATA_WRITTEN(r) ((r) != 15 && (r) != 28 && (r) != 29 && (r) != 31)

#endif
