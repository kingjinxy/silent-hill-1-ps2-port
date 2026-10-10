/** @brief sh1agent.irx: remote control for hardware testing over Ethernet.
 *
 * Listens on UDP port 62968 through Neutrino's ministack (loaded with the game when it boots over
 * UDPFS; this module only loads if ministack is present). The first two payload bytes are a
 * command: "RS" (restart the game), "PI" (ping). Each command is copied into a 16-byte mailbox in EE
 * memory by SIF DMA (address from the "mb=0x..." argument; src/port/ps2/agent_ps2.c polls it every
 * vertical blank), with two 32-bit arguments from the next 8 payload bytes ("MD": address,
 * length), and answered with a short text to the sender (tools/port/ps2_ctl.py).
 *
 * The EE's state: the game sends s_EeStatus (its address goes to the EE in the mailbox's first spare
 * word) twice a second from its vertical blank interrupt, and the ping's answer shows it. That works
 * when everything on the EE that prints is stuck (a hung console path), and tells a stopped EE (no
 * interrupts: the count doesn't move between pings) from a stuck game thread.
 *
 * The EE's console output: the game's log thread (src/port/ps2/log_ps2.c) DMAs a chunk of text into
 * s_Log (address in the mailbox's second spare word), text first, then the header with a new
 * sequence number; a thread here broadcasts it as UDP to port 18194, as ministack's udptty would,
 * and acknowledges it in the mailbox's third spare word. That leaves out udptty, which stalled for
 * good during disc loads on hardware (its lock never came free) and took every printf with it.
 */

#include <irx.h>
#include <loadcore.h>
#include <intrman.h>
#include <sifman.h>
#include <sysclib.h>
#include <stdio.h>
#include <thbase.h>
#include "ministack.h"

IRX_ID("sh1agent", 1, 0);

#define AGENT_PORT 62968

typedef struct
{
    u32 magic; /* 'SH1A' */
    u32 seq;   /* incremented per command */
    u32 cmd;   /* two command bytes */
    u32 arg;
    u32 arg2;
    u32 pad[3];
} mailbox_t;

typedef struct
{
    u32 magic;       /* 'SH1S' once the EE has sent one */
    u32 vblanks;     /* the EE's vertical blank count */
    u32 frames;      /* frames shown */
    u32 epc;         /* where the vertical blank interrupted the EE */
    u32 main_status; /* the game thread: kernel thread status (1 run, 2 ready, 4 wait, ...) */
    u32 main_wait;   /* its wait type (1 sleep, 2 semaphore) */
    u32 main_wait_id;
    u32 log_pending; /* console output not sent yet (bytes) */
    u32 log_dropped;
    u32 pad[3];
} ee_status_t;

#define LOG_MAX 2048

typedef struct
{
    u32  seq; /* the EE's chunk number (written last) */
    u32  len;
    u32  pad[2];
    char text[LOG_MAX];
} log_t;

static mailbox_t     s_Box __attribute__((aligned(16)));
static log_t         s_Log __attribute__((aligned(16)));
static udp_packet_t  s_LogPkt;
static ee_status_t   s_EeStatus __attribute__((aligned(16)));
static u32           s_LastVBlanks;
static u32           s_EeBox;
static udp_socket_t* s_Socket;
static udp_packet_t  s_Reply;
static char          s_ReplyText[384];

static void to_ee(void)
{
    SifDmaTransfer_t dt;
    int              state, id;
    dt.src  = &s_Box;
    dt.dest = (void*)s_EeBox;
    dt.size = sizeof(s_Box);
    dt.attr = 0;
    CpuSuspendIntr(&state);
    id = sceSifSetDma(&dt, 1);
    CpuResumeIntr(state);
    while (sceSifDmaStat(id) >= 0)
    {
    }
}

static int on_packet(udp_socket_t* socket, void* arg, const u8* hdr, u16 hdr_len)
{
    u32 src_ip  = ((u32)hdr[26] << 24) | ((u32)hdr[27] << 16) | ((u32)hdr[28] << 8) | hdr[29];
    u16 src_port = (u16)((hdr[34] << 8) | hdr[35]);
    u32 cmd      = (u32)hdr[42] | ((u32)hdr[43] << 8);
    u32 args[2]  = { 0, 0 };
    int len;
    (void)arg;
    (void)hdr_len;

    smap_fifo_read(0x2C, args, sizeof(args)); /* the payload after the command bytes */
    s_Box.seq++;
    s_Box.cmd  = cmd;
    s_Box.arg  = args[0];
    s_Box.arg2 = args[1];
    to_ee();

    len = sprintf(s_ReplyText, "SH1 agent: command %c%c, seq %u\n", hdr[42], hdr[43], (unsigned)s_Box.seq);
    if (cmd == ('P' | ('I' << 8)))
    {
        if (s_EeStatus.magic != 0x53314853) /* 'SH1S' */
        {
            len += sprintf(s_ReplyText + len, "EE: no status received\n");
        }
        else
        {
            len += sprintf(s_ReplyText + len,
                           "EE: vblank %u (+%u since the last ping), frame %u, pc %08x; game thread status %u wait %u/%u; "
                           "log %u bytes pending, %u dropped\n",
                           (unsigned)s_EeStatus.vblanks, (unsigned)(s_EeStatus.vblanks - s_LastVBlanks),
                           (unsigned)s_EeStatus.frames, (unsigned)s_EeStatus.epc, (unsigned)s_EeStatus.main_status,
                           (unsigned)s_EeStatus.main_wait, (unsigned)s_EeStatus.main_wait_id,
                           (unsigned)s_EeStatus.log_pending, (unsigned)s_EeStatus.log_dropped);
            s_LastVBlanks = s_EeStatus.vblanks;
        }
        /* The IOP's side of the EE's transfers: DMA channel 10 (SIF1, EE to IOP) and 9 (SIF0), the
         * DMA enables (DPCR2) and the SIF control register. */
        len += sprintf(s_ReplyText + len, "IOP: ch10 CHCR %08x MADR %08x BCR %08x TADR %08x; ch9 CHCR %08x; DPCR2 %08x; SIF CTRL %08x\n",
                       (unsigned)*(volatile u32*)0xBF801528, (unsigned)*(volatile u32*)0xBF801520,
                       (unsigned)*(volatile u32*)0xBF801524, (unsigned)*(volatile u32*)0xBF80152C,
                       (unsigned)*(volatile u32*)0xBF801518, (unsigned)*(volatile u32*)0xBF801570,
                       (unsigned)*(volatile u32*)0xBD000040);
    }
    udp_packet_init(&s_Reply, src_ip, src_port);
    s_Reply.align = 0x2020;
    udp_packet_send_ll(socket, &s_Reply, 2, s_ReplyText, (u16)len);
    return 0;
}

static void log_thread(void* arg)
{
    u32 last = 0;
    (void)arg;
    for (;;)
    {
        u32 seq = *(volatile u32*)&s_Log.seq, len, off;
        if (seq == last)
        {
            DelayThread(2000);
            continue;
        }
        last = seq;
        len  = s_Log.len < LOG_MAX ? s_Log.len : LOG_MAX;
        for (off = 0; off < len; off += 1024)
        {
            u32 n = len - off < 1024 ? len - off : 1024;
            udp_packet_send_ll(s_Socket, &s_LogPkt, 2, s_Log.text + off, (u16)n);
        }
        s_Box.pad[2] = last;
        to_ee();
    }
}

int _start(int argc, char* argv[])
{
    int i;
    for (i = 1; i < argc; i++)
    {
        if (!strncmp(argv[i], "mb=", 3))
        {
            s_EeBox = strtoul(&argv[i][3], NULL, 16);
        }
    }
    if (s_EeBox == 0)
    {
        printf("sh1agent: no mailbox address (mb=0x...)\n");
        return MODULE_NO_RESIDENT_END;
    }
    s_Box.magic  = 0x41314853; /* 'SH1A' */
    s_Box.pad[0] = (u32)&s_EeStatus;
    s_Box.pad[1] = (u32)&s_Log;
    s_Socket    = udp_bind(AGENT_PORT, on_packet, NULL);
    if (s_Socket == NULL)
    {
        printf("sh1agent: could not bind UDP port %d\n", AGENT_PORT);
        return MODULE_NO_RESIDENT_END;
    }
    udp_packet_init(&s_LogPkt, 0xFFFFFFFF, 18194); /* broadcast, as udptty */
    s_LogPkt.align = 0x2020;                       /* two spaces: tools/port/ps2_log.py drops them */
    {
        iop_thread_t th;
        th.attr      = TH_C;
        th.option    = 0;
        th.thread    = log_thread;
        th.stacksize = 0x800;
        th.priority  = 60; /* below the sound module's threads (40-55) */
        StartThread(CreateThread(&th), NULL);
    }
    to_ee();
    printf("sh1agent: listening on UDP port %d, mailbox at EE 0x%08x\n", AGENT_PORT, (unsigned)s_EeBox);
    return MODULE_RESIDENT_END;
}
