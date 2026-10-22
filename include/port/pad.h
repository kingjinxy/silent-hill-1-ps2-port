#ifndef _PORT_PAD_H
#define _PORT_PAD_H

/** @brief The PS2's controller ports (src/port/ps2/pad_ps2.c), through ps2sdk's libpad and the BIOS's
 * rom0:SIO2MAN/PADMAN. Ports 0 and 1 are the PS2's ports 1 and 2.
 *
 * A pad's data is the PS1 controller record (8 bytes): status (0 = ok, 0xFF = no pad), controller ID
 * (0x41 digital, 0x73 analog), buttons (16 bits, active low), then right X/Y and left X/Y. ps2sdk's
 * padButtonStatus starts with the same bytes. Port_PadPoll() refreshes it once per vertical blank
 * (from VSync, as the PS1 BIOS does); while a read isn't ready the previous data stays.
 */

/** Loads rom0:SIO2MAN and PADMAN (once); the memory card modules need SIO2MAN first. */
void Port_PadModulesLoad(void);

/** Opens both ports (the game's Joy_Init). */
void Port_PadInit(void);

/** Closes the ports before a restart: PADMAN would keep writing pad data into this build's memory. */
void Port_PadShutdown(void);

/** The ports are open (Port_PadInit was called). */
int Port_PadIsOpen(void);

/** Once per vertical blank: reads both pads, refreshes their mode info, sends changed motor levels. */
void Port_PadPoll(void);

/** Copies port `port`'s last data (8 bytes) to `out`. */
void Port_PadRead(int port, void* out);

/** Port 1's buttons from the last poll (PS1 bit order, set = pressed; 0 without a pad). */
unsigned int Port_PadButtons(void);

/** ps2sdk pad state (PAD_STATE_*). */
int Port_PadState(int port);

/** Mode info as padInfoMode: term 1 current ID, 2 current extended ID, 3 current mode index, 4 mode ID
 * table (offs -1: count). Answered from the cache Port_PadPoll() keeps (each padInfoMode call is an
 * IOP round trip). */
int Port_PadModeInfo(int port, int term, int offs);

int Port_PadActInfo(int port, int actno, int term);
int Port_PadSetMainMode(int port, int mode, int lock);
int Port_PadSetActAlign(int port, const unsigned char* align);

/** Motor levels: `len` bytes (at most 6) read from `src` at every poll, as the PS1 BIOS reads the act
 * buffer every vertical blank; sent to the pad only when they change. NULL stops. */
void Port_PadSetMotorSource(int port, const unsigned char* src, int len);

#endif
