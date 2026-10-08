/** @brief sh1agent.irx: remote control for hardware testing over Ethernet.
 *
 * Listens on UDP port 62968 through Neutrino's ministack (loaded with the game when it boots over
 * UDPFS; this module only loads if ministack is present). The first two payload bytes are a
 * command: "RS" (restart the game), "PI" (ping). Each command is copied into a 16-byte mailbox in EE
 * memory by SIF DMA (address from the "mb=0x..." argument; src/port/ps2/agent_ps2.c polls it every
 * vertical blank), with two 32-bit arguments from the next 8 payload bytes ("MD": address,
 * length), and answered with a short text to the sender (tools/port/ps2_ctl.py).
 */

#include <irx.h>
#include <loadcore.h>
#include <intrman.h>
#include <sifman.h>
#include <sysclib.h>
#include <stdio.h>
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

static mailbox_t     s_Box __attribute__((aligned(16)));
static u32           s_EeBox;
static udp_socket_t* s_Socket;
static udp_packet_t  s_Reply;
static char          s_ReplyText[64];

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
    udp_packet_init(&s_Reply, src_ip, src_port);
    s_Reply.align = 0x2020;
    udp_packet_send_ll(socket, &s_Reply, 2, s_ReplyText, (u16)len);
    return 0;
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
    s_Box.magic = 0x41314853; /* 'SH1A' */
    s_Socket    = udp_bind(AGENT_PORT, on_packet, NULL);
    if (s_Socket == NULL)
    {
        printf("sh1agent: could not bind UDP port %d\n", AGENT_PORT);
        return MODULE_NO_RESIDENT_END;
    }
    to_ee();
    printf("sh1agent: listening on UDP port %d, mailbox at EE 0x%08x\n", AGENT_PORT, (unsigned)s_EeBox);
    return MODULE_RESIDENT_END;
}
