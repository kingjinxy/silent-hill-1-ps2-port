/** @brief sh1spu.irx: the PS1 SPU on SPU2 core 0, for the port's libspu (src/port/libspu_port.c,
 * src/port/ps2/spu_ps2.c).
 *
 * The EE sends commands into a ring buffer in IOP memory by SIF DMA and then sends its new write
 * position. A thread here polls the ring and runs each command: register writes (SPU2 core 0, the
 * offsets already converted on the EE), sample uploads (SPU2 DMA through freesd's sceSdVoiceTrans)
 * and clears of sound memory. After running commands, and at least every millisecond, it sends a
 * status block into EE memory: how far the ring has been run (a running byte count) and every
 * voice's envelope (ENVX), which the EE needs for SpuGetKeyStatus.
 *
 * Only core 0 plays the game's sound. Core 1 just passes core 0's output through to the outputs.
 * EE address of the status block: argument "st=0x...". When it starts, the module writes the
 * ring's address into that block.
 */

#include "irx_imports.h"
#include "log.h"

IRX_ID("sh1spu", 1, 0);

#define RING_SIZE (96 * 1024)
#define SPU2      0xBF900000
#define REG(off)  (*(volatile u16*)(SPU2 + (off)))

#define OP_REGS  1 /* a: count; then count words (offset << 16 | value), 16-byte padded */
#define OP_WRITE 2 /* a: SPU2 byte address, b: size; then the data, padded to 64 bytes */
#define OP_CLEAR 3 /* a: SPU2 byte address, b: size */
#define OP_WRAP  4 /* continue at the start of the ring */
#define OP_XA    5 /* a: DVD sector of HILL., b: PS1 sector in it, c: file | channel << 8 (xa.c) */
#define OP_XASTOP 6
#define OP_SHUTDOWN 7 /* the game is about to restart: stop every transfer and voice */

void xa_init(void);
void xa_start(u32 hillLsn, u32 index, u32 file, u32 chan);
void xa_stop(void);
void xa_check(void);

typedef struct
{
    u32 magic; /* 'SPU2' once the module runs */
    u32 ring;  /* IOP address of the ring */
    u32 wpos;  /* IOP address of the EE's write position (one word, 16-byte aligned) */
    u32 size;
    u32 done;  /* bytes of commands run so far (running count) */
    u32 count; /* status updates sent */
    u32 pad[2];
    u16 envx[24];
    u32 pad2[4];
} status_t;

/* The module loader aligns data to 16 bytes only; SPU2 DMA needs 64-byte aligned memory on hardware
 * (PCSX2 doesn't), so the ring and the zero block are aligned at run time. */
static u8       s_RingSpace[RING_SIZE + 64];
static u8*      s_Ring;
static u32      s_WPos[4] __attribute__((aligned(16)));
static status_t s_State;
static status_t s_Out __attribute__((aligned(16)));
static u8       s_ZeroSpace[2048 + 64];
static u8*      s_Zero;
#define ZERO_SIZE 2048
static u32      s_EeStatus;
static int      s_DmaId = -1;

static void send_status(void)
{
    SifDmaTransfer_t dt;
    int              v, state;
    if (s_DmaId >= 0)
    {
        while (sceSifDmaStat(s_DmaId) >= 0) /* the previous status still being sent from s_Out */
        {
        }
    }
    s_State.count++;
    for (v = 0; v < 24; v++)
    {
        s_State.envx[v] = REG(v * 0x10 + 0xA);
    }
    s_Out   = s_State;
    dt.src  = &s_Out;
    dt.dest = (void*)s_EeStatus;
    dt.size = sizeof(s_Out);
    dt.attr = 0;
    CpuSuspendIntr(&state);
    s_DmaId = sceSifSetDma(&dt, 1);
    CpuResumeIntr(state);
}

static void transfer(u8* src, u32 addr, u32 size)
{
    /* Channel 1 (core 1's DMA): channel 0's carries core 0's XA input (xa.c). Sound memory is shared. */
    int r = sceSdVoiceTrans(1, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, src, (u32*)addr, size);
    if (r < 0)
    {
        printf("sh1spu: upload of %u bytes to 0x%x failed (%d)\n", (unsigned)size, (unsigned)addr, r);
        return;
    }
    sceSdVoiceTransStatus(1, 1); /* wait */
}

/** Waits a little over two SPU2 sample periods (2 / 48000 s) before a key on or key off. The PS1
 * driver writes a voice's key off and then its next key on with real time between them. Here
 * register writes come in batches, so they arrive microseconds apart, and the SPU2 then misses the
 * key off: a looping instrument note keeps sounding. The wait reads an SPU2 register, so it takes
 * bus time (several hundred ns per read) rather than a CPU-speed loop. */
static void key_gap(void)
{
    int i;
    for (i = 0; i < 400; i++)
    {
        (void)REG(0x344); /* STATX */
    }
}

static int s_Down; /* OP_SHUTDOWN done: nothing more is sent to the EE */

/** Runs one command at ring offset pos; returns its size in the ring. */
static u32 run(u32 pos)
{
    u32* p = (u32*)&s_Ring[pos];
    u32  n, i, size;
    switch (p[0])
    {
        case OP_REGS:
            n = p[1];
            for (i = 0; i < n; i++)
            {
                u32 off = p[4 + i] >> 16;
                if (off == 0x1A0 || off == 0x1A4) /* KON / KOFF (voices 0-15; 16-23 follow at +2) */
                {
                    key_gap();
                }
                if (off == 0x19A) /* ATTR: keep the DMA mode bits of the XA input */
                {
                    REG(off) = (u16)((p[4 + i] & ~0x30u) | (REG(off) & 0x30));
                    continue;
                }
                REG(off) = (u16)p[4 + i];
            }
            return 16 + ((n * 4 + 15) & ~15u);
        case OP_WRITE:
            size = (p[2] + 63) & ~63u;
            transfer(&s_Ring[pos + 64], p[1], size);
            return 64 + size;
        case OP_CLEAR:
            for (i = 0; i < p[2]; i += ZERO_SIZE)
            {
                transfer(s_Zero, p[1] + i, p[2] - i < ZERO_SIZE ? p[2] - i : ZERO_SIZE);
            }
            return 16;
        case OP_XA:
            xa_start(p[1], p[2], p[3] & 0xFF, (p[3] >> 8) & 0xFF);
            return 16;
        case OP_XASTOP:
            xa_stop();
            return 16;
        case OP_SHUTDOWN:
        {
            extern void xa_shutdown(void); /* xa.c */
            xa_shutdown();
            REG(0x1A4) = 0xFFFF; /* core 0 KOFF: every voice */
            REG(0x1A6) = 0x00FF;
            REG(0x188) = REG(0x18A) = REG(0x190) = REG(0x192) = 0; /* no voice mixed in */
            sceSdVoiceTransStatus(1, 1); /* no sample upload left running (they're waited for anyway) */
            printf("sh1spu: shut down for a restart\n");
            s_Down = 1;
            return 16;
        }
        case OP_WRAP:
            return RING_SIZE - pos;
    }
    printf("sh1spu: bad command %u at %u\n", (unsigned)p[0], (unsigned)pos);
    return RING_SIZE - pos;
}

static void loop(void* arg)
{
    u32 pos = 0, idle = 0, ticks = 0;
    (void)arg;
    for (;;)
    {
        u32 w = *(volatile u32*)s_WPos;
        if (w != pos)
        {
            while (pos != w)
            {
                u32 n = run(pos);
                s_State.done += n;
                pos = (pos + n) % RING_SIZE;
            }
            send_status();
            idle = 0;
            while (s_Down) /* the last status went out: no more DMA into EE memory until the reboot */
            {
                DelayThread(100000);
            }
            continue;
        }
        if (++idle >= 4)
        {
            send_status();
            idle = 0;
        }
        if (++ticks >= 4000) /* about a second */
        {
            xa_check();
            ticks = 0;
        }
        DelayThread(250);
    }
}

/** Core 1 passes core 0's output through; core 0 mixes every voice, dry and through reverb. */
static void setup(void)
{
    sceSdInit(0);
    REG(0x400 + 0x198) = 0x00C;            /* core 1 MMIX: core 0's output (dry) */
    REG(0x788 + 0x8)   = 0x7FFF;           /* core 1 AVOLL/R and BVOLL/R: input volumes (which of the */
    REG(0x788 + 0xA)   = 0x7FFF;           /* two takes core 0's output differs between documents) */
    REG(0x788 + 0xC)   = 0x7FFF;
    REG(0x788 + 0xE)   = 0x7FFF;
    REG(0x400 + 0x19A) = 0xC000;           /* core 1 ATTR: on, unmuted */
    REG(0x19A)         = 0xC000;           /* core 0 ATTR */
    REG(0x788 + 0x0)   = 0x3FFF;           /* core 1 MVOLL/R */
    REG(0x788 + 0x2)   = 0x3FFF;
    REG(0x198)         = 0xF00;            /* core 0 MMIX: voices, dry and wet (the XA input: libspu) */
    REG(0x188)         = 0xFFFF;           /* core 0 VMIXL/R: every voice dry */
    REG(0x18A)         = 0x00FF;
    REG(0x190)         = 0xFFFF;
    REG(0x192)         = 0x00FF;
    REG(0x18C) = REG(0x18E) = REG(0x194) = REG(0x196) = 0; /* no reverb voices yet */
}

int _start(int argc, char* argv[])
{
    iop_thread_t th;
    int          i;
    spu_log_init();
    for (i = 1; i < argc; i++)
    {
        if (!strncmp(argv[i], "st=", 3))
        {
            s_EeStatus = strtoul(&argv[i][3], NULL, 16);
        }
    }
    if (s_EeStatus == 0)
    {
        printf("sh1spu: no status address (st=0x...)\n");
        return MODULE_NO_RESIDENT_END;
    }
    s_Ring = (u8*)(((u32)s_RingSpace + 63) & ~63u);
    s_Zero = (u8*)(((u32)s_ZeroSpace + 63) & ~63u);
    for (i = 0; i < ZERO_SIZE; i++)
    {
        s_Zero[i] = 0;
    }
    setup();
    xa_init();
    s_State.magic = 0x32555053; /* 'SPU2' */
    s_State.ring  = (u32)s_Ring;
    s_State.wpos  = (u32)s_WPos;
    s_State.size  = RING_SIZE;
    send_status();
    th.attr      = TH_C;
    th.thread    = loop;
    th.priority  = 40;
    th.stacksize = 0x800;
    th.option    = 0;
    StartThread(CreateThread(&th), NULL);
    printf("sh1spu: SPU2 core 0 ready, ring at IOP 0x%08x\n", (unsigned)s_Ring);
    return MODULE_RESIDENT_END;
}
