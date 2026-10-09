/** @brief EE side of the port's sound: SPU2 core 0 through sh1spu.irx (src/port/iop/sh1spu), and the
 * sound driver's timer tick.
 *
 * src/port/libspu_port.c (the PS1 libspu calls) converts everything to SPU2 core 0 register writes,
 * sample uploads and clears. This file sends them into the IOP module's ring buffer by SIF DMA.
 * Register writes are collected and sent together, before any other command, when the driver waits
 * on the SPU, and once per tick or frame. The module sends back a status block: how many bytes of
 * commands it has run, and each voice's envelope.
 *
 * The tick: the PS1 sound driver runs from root counter 2's interrupt (about 578 Hz,
 * src/bodyprog/libsd/smf_main.c smf_timer). Here a kernel alarm (in horizontal blanks) wakes a sound
 * thread above the game's priority, and that thread delivers the counter event
 * (src/port/libapi_port.c), which calls the driver as the PS1's interrupt did.
 */

#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>

extern unsigned char freesd_irx[];
extern unsigned int  size_freesd_irx;
extern unsigned char sh1spu_irx[];
extern unsigned int  size_sh1spu_irx;
extern void*         _gp;

extern void Port_ModuleLoadInit(void); /* agent_ps2.c */
extern void Port_EventDeliver(unsigned long cls, unsigned long spec); /* libapi_port.c */

#define OP_REGS  1
#define OP_WRITE 2
#define OP_CLEAR 3
#define OP_WRAP  4
#define OP_XA    5
#define OP_XASTOP 6

#define CHUNK    16384 /* sample upload per command */
#define MAX_REGS 512

typedef struct
{
    u32 magic;
    u32 ring;
    u32 wpos;
    u32 size;
    u32 done;
    u32 count;
    u32 pad[2];
    u16 envx[24];
    u32 pad2[4];
} status_t;

static status_t s_Status __attribute__((aligned(64)));
static u8       s_StatusPad[64] __attribute__((aligned(64))); /* keeps other data off its cache lines */
static u32      s_Packet[(64 + CHUNK) / 4] __attribute__((aligned(64)));
static u32      s_WPosOut[4] __attribute__((aligned(64)));
static u32      s_Regs[MAX_REGS];
static int      s_RegCount;
static u32      s_Pos;  /* ring offset of the next command */
static u32      s_Sent; /* bytes of commands sent (running count, as status.done) */
static int      s_On;
static int      s_DmaId = -1;
static u32      s_StRegs, s_StUpload; /* statistics (Port_SpuStats) */

static volatile status_t* status(void)
{
    return (volatile status_t*)((u32)&s_Status | 0x20000000); /* uncached: the IOP writes it */
}

static void dma_wait(void)
{
    if (s_DmaId >= 0)
    {
        while (SifDmaStat(s_DmaId) >= 0)
        {
        }
        s_DmaId = -1;
    }
}

static void dma(void* src, u32 dest, u32 size)
{
    SifDmaTransfer_t dt;
    dt.src  = src;
    dt.dest = (void*)dest;
    dt.size = size;
    dt.attr = 0;
    SifWriteBackDCache(src, size);
    while ((s_DmaId = SifSetDma(&dt, 1)) == 0)
    {
    }
}

/** Sends s_Packet (n bytes, a multiple of 16) as the next command. Interrupts are off. */
static void send(u32 n)
{
    volatile status_t* st   = status();
    u32                size = st->size;
    u32                tail = s_Pos + n > size ? size - s_Pos : 0;
    static u32         wrap[4] __attribute__((aligned(64))) = { OP_WRAP, 0, 0, 0 };
    while (size - (s_Sent - st->done) <= tail + n) /* wait for room (never fill the ring completely) */
    {
    }
    dma_wait();
    if (tail)
    {
        dma(wrap, st->ring + s_Pos, 16);
        s_Sent += tail;
        s_Pos = 0;
    }
    dma(s_Packet, st->ring + s_Pos, n);
    s_Pos = (s_Pos + n) % size;
    s_Sent += n;
    s_WPosOut[0] = s_Pos;
    dma(s_WPosOut, st->wpos, 16);
    dma_wait(); /* s_Packet and s_WPosOut are reused */
}

static void flush_regs(void)
{
    u32 n;
    if (s_RegCount == 0)
    {
        return;
    }
    s_Packet[0] = OP_REGS;
    s_Packet[1] = (u32)s_RegCount;
    s_Packet[2] = s_Packet[3] = 0;
    memcpy(&s_Packet[4], s_Regs, (u32)s_RegCount * 4);
    n           = 16 + (((u32)s_RegCount * 4 + 15) & ~15u);
    s_RegCount  = 0;
    send(n);
}

/** Queues a write of `value` to SPU2 register `off` (from 0xBF900000). */
void Port_SpuReg(unsigned int off, unsigned int value)
{
    if (!s_On)
    {
        return;
    }
    DI();
    if (s_RegCount == MAX_REGS)
    {
        flush_regs();
    }
    s_Regs[s_RegCount++] = (off << 16) | (value & 0xFFFF);
    s_StRegs++;
    EI();
}

/** Sends the queued register writes. */
void Port_SpuFlush(void)
{
    if (!s_On)
    {
        return;
    }
    DI();
    flush_regs();
    EI();
}

/** Uploads `size` bytes to SPU2 memory at byte address `addr` (in 64-byte blocks, as the PS1 did). */
void Port_SpuUpload(unsigned int addr, const void* data, unsigned int size)
{
    unsigned int done;
    if (!s_On)
    {
        return;
    }
    s_StUpload += size;
    for (done = 0; done < size; done += CHUNK)
    {
        unsigned int n = size - done < CHUNK ? size - done : CHUNK;
        DI();
        flush_regs();
        s_Packet[0] = OP_WRITE;
        s_Packet[1] = addr + done;
        s_Packet[2] = n;
        memcpy(&s_Packet[16], (const u8*)data + done, n);
        memset((u8*)&s_Packet[16] + n, 0, ((n + 63) & ~63u) - n);
        send(64 + ((n + 63) & ~63u));
        EI();
    }
}

/** Zeroes `size` bytes of SPU2 memory at byte address `addr`. */
void Port_SpuClear(unsigned int addr, unsigned int size)
{
    if (!s_On)
    {
        return;
    }
    DI();
    flush_regs();
    s_Packet[0] = OP_CLEAR;
    s_Packet[1] = addr;
    s_Packet[2] = size;
    s_Packet[3] = 0;
    send(16);
    EI();
}

/** Starts XA playback (src/port/iop/sh1spu/xa.c): `file`/`chan` of the raw PS1 sectors from `index`
 * on in HILL. (at DVD sector `hillLsn`). */
void Port_XaStart(unsigned int hillLsn, unsigned int index, unsigned int file, unsigned int chan)
{
    if (!s_On)
    {
        return;
    }
    DI();
    flush_regs();
    s_Packet[0] = OP_XA;
    s_Packet[1] = hillLsn;
    s_Packet[2] = index;
    s_Packet[3] = file | (chan << 8);
    send(16);
    EI();
}

void Port_XaStop(void)
{
    if (!s_On)
    {
        return;
    }
    DI();
    flush_regs();
    s_Packet[0] = OP_XASTOP;
    s_Packet[1] = s_Packet[2] = s_Packet[3] = 0;
    send(16);
    EI();
}

/** Running count of command bytes sent (compare with Port_SpuDone). */
unsigned int Port_SpuSent(void)
{
    return s_Sent;
}

/** Running count of command bytes the IOP has run. */
unsigned int Port_SpuDone(void)
{
    return s_On ? status()->done : s_Sent;
}

/** Voice v's envelope as last reported by the IOP. */
unsigned int Port_SpuEnvx(int v)
{
    return s_On ? status()->envx[v] : 0;
}

/** Prints and clears the statistics: register writes, uploads, voices sounding, IOP status updates. */
void Port_SpuStats(void)
{
    int v, n = 0;
    if (!s_On)
    {
        return;
    }
    for (v = 0; v < 24; v++)
    {
        n += status()->envx[v] != 0;
    }
    printf("spu: %u register writes, %u KB uploaded, %d voices sounding, IOP status %u\n", (unsigned)s_StRegs,
           (unsigned)(s_StUpload / 1024), n, (unsigned)status()->count);
    s_StRegs = s_StUpload = 0;
}

/* --- Sound timer (PS1 root counter 2 interrupts) --------------------------------------------- */

static int           s_TickThread = -1;
static int           s_TickOn;
static unsigned int  s_TickRate1000; /* horizontal blanks per tick x 1000 */
static unsigned int  s_TickAcc;
static unsigned char s_TickStack[0x4000] __attribute__((aligned(16)));

static void alarm(s32 id, u16 time, void* arg)
{
    unsigned int hs;
    (void)id;
    (void)time;
    (void)arg;
    if (!s_TickOn)
    {
        return;
    }
    s_TickAcc += s_TickRate1000;
    hs = s_TickAcc / 1000;
    s_TickAcc -= hs * 1000;
    iSetAlarm((u16)(hs ? hs : 1), alarm, NULL);
    iWakeupThread(s_TickThread);
    ExitHandler();
}

static void tick_thread(void* arg)
{
    (void)arg;
    for (;;)
    {
        SleepThread();
        if (s_TickOn)
        {
            Port_EventDeliver(0xF2000002UL, 0x0002); /* RCntCNT2, EvSpINT */
            Port_SpuFlush();
        }
    }
}

/** Starts (rate_hz > 0) or stops the sound driver's tick. */
void Port_SoundTimer(unsigned int rate_hz)
{
    if (s_TickThread < 0)
    {
        ee_thread_t th;
        th.func             = tick_thread;
        th.stack            = s_TickStack;
        th.stack_size       = sizeof(s_TickStack);
        th.gp_reg           = &_gp;
        th.initial_priority = 20; /* above the game (64), below the remote control agent (2) */
        s_TickThread        = CreateThread(&th);
        StartThread(s_TickThread, NULL);
    }
    if (rate_hz == 0)
    {
        s_TickOn = 0;
        return;
    }

    s_TickRate1000 = 15734U * 1000U / rate_hz;
    if (!s_TickOn)
    {
        s_TickOn  = 1;
        s_TickAcc = 0;
        SetAlarm((u16)(s_TickRate1000 / 1000), alarm, NULL);
    }
}

/* --- Start-up ------------------------------------------------------------------------------- */

/** Loads the sound modules once (SpuInit). */
void Port_SpuStart(void)
{
    static int tried;
    char       args[32];
    int        id, result = 0, len, i;
    if (tried)
    {
        return;
    }
    tried = 1;
    (void)s_StatusPad;
    memset(&s_Status, 0, sizeof(s_Status));
    FlushCache(0);
    Port_ModuleLoadInit();
    id = SifExecModuleBuffer(freesd_irx, size_freesd_irx, 0, NULL, &result);
    if (id < 0)
    {
        printf("port: no sound (freesd.irx: %d)\n", id);
        return;
    }
    len = sprintf(args, "st=0x%08x", (unsigned int)&s_Status & 0x1FFFFFFF) + 1;
    id  = SifExecModuleBuffer(sh1spu_irx, size_sh1spu_irx, len, args, &result);
    if (id < 0 || result == 1)
    {
        printf("port: no sound (sh1spu.irx: %d, %d)\n", id, result);
        return;
    }
    for (i = 0; i < 1000000 && status()->magic != 0x32555053; i++)
    {
    }
    if (status()->magic != 0x32555053)
    {
        printf("port: no sound (sh1spu.irx didn't answer)\n");
        return;
    }
    s_On = 1;
    printf("port: sound on SPU2 core 0 (ring of %u bytes)\n", (unsigned)status()->size);
}
