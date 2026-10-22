/** @brief The PS1 libpad calls Konami's libkpad makes (src/bodyprog/libkpad/libkpad.c,
 * bodyprog_80089090.c), on the PS2's pads (pad_ps2.c). Nothing else in the game uses libpad: its input
 * goes through Joy_Init/Joy_ReadP1 (joy.c), which call pad_ps2.c directly.
 *
 * libkpad's handlers (s_KPad_Handler) detect the controller from PadGetState and the IDs from
 * PadInfoMode, then set the motor alignment (PadSetActAlign, `isAlignDone`) and the buffer the motor
 * levels are read from (PadSetAct, `isActSet`); func_80089524 switches to analog mode
 * (PadSetMainMode). PS1 sockets: 0x00 = port 1, 0x10 = port 2 (bits 0-1, the multitap slot, unused).
 */

#include <libpad.h>
#include "port/pad.h"

static int port_of(int socket)
{
    return (socket >> 4) & 1;
}

/** The pad buffers are refreshed once per vertical blank (Port_PadPoll), once the ports are open. */
int PadChkVsync(void)
{
    return Port_PadIsOpen();
}

/** PS1 states: 0 disconnected, 1 searching, 2 executing a command, 6 stable. */
int PadGetState(int socket)
{
    switch (Port_PadState(port_of(socket)))
    {
        case PAD_STATE_DISCONN:  return 0;
        case PAD_STATE_EXECCMD:  return 2;
        case PAD_STATE_STABLE:   return 6;
        case PAD_STATE_FINDCTP1: return 6; /* stable, but no extended (DualShock) modes */
    }
    return 1;
}

/** Terms as ps2sdk's (1 current ID, 2 current extended ID, 3 current offset, 4 ID table). */
int PadInfoMode(int socket, int term, int offs)
{
    return Port_PadModeInfo(port_of(socket), term, offs);
}

int PadInfoAct(int socket, int actno, int term)
{
    return Port_PadActInfo(port_of(socket), actno, term);
}

int PadSetMainMode(int socket, int offs, int lock)
{
    return Port_PadSetMainMode(port_of(socket), offs, lock);
}

int PadSetActAlign(int socket, unsigned char* align)
{
    return Port_PadSetActAlign(port_of(socket), align);
}

/** The PS1 sends the motor levels from this buffer every vertical blank. */
void PadSetAct(int socket, unsigned char* data, int len)
{
    Port_PadSetMotorSource(port_of(socket), data, len);
}
