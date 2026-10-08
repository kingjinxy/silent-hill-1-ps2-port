/* The parts of Neutrino's ministack (github.com/ps2max32/neutrino, iop/ministack) that sh1agent uses:
 * its UDP socket API, imported from the "mstack" export table. Packet layout: Ethernet (14 bytes),
 * IPv4 (20), UDP (8), 2 padding bytes; hdr[] in the receive callback is the frame's first 44 bytes. */
#ifndef SH1AGENT_MINISTACK_H
#define SH1AGENT_MINISTACK_H

#include <tamtypes.h>
#include <irx.h>

typedef struct
{
    u8  headers[42]; /* Ethernet + IPv4 + UDP */
    u16 align;       /* 2 bytes of padding before the payload */
} __attribute__((packed, aligned(4))) udp_packet_t;

struct udp_socket;
typedef int (*udp_port_handler)(struct udp_socket* socket, void* arg, const u8* hdr, u16 hdr_len);
typedef struct udp_socket
{
    u16              port_src;
    udp_port_handler handler;
    void*            handler_arg;
} udp_socket_t;

udp_socket_t* udp_bind(u16 port_src, udp_port_handler handler, void* handler_arg);
void          udp_packet_init(udp_packet_t* pkt, u32 ip_dst, u16 port_dst);
int           udp_packet_send_ll(udp_socket_t* socket, udp_packet_t* pkt, u16 pktdatasize, const void* data, u16 datasize);

/* Neutrino's SMAP driver: reads more of the received frame (offset from the frame start), from the
 * receive callback. */
void smap_fifo_read(u16 offset, void* dst, u32 bytes);
#define smap_IMPORTS_start DECLARE_IMPORT_TABLE(smap, 1, 0)
#define smap_IMPORTS_end   END_IMPORT_TABLE
#define I_smap_fifo_read   DECLARE_IMPORT(7, smap_fifo_read)

#define mstack_IMPORTS_start DECLARE_IMPORT_TABLE(mstack, 1, 0)
#define mstack_IMPORTS_end   END_IMPORT_TABLE
#define I_udp_bind           DECLARE_IMPORT(4, udp_bind)
#define I_udp_packet_init    DECLARE_IMPORT(5, udp_packet_init)
#define I_udp_packet_send_ll DECLARE_IMPORT(6, udp_packet_send_ll)

#endif
