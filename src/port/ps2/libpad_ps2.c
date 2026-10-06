/** @brief PS1 libpad (PadInitDirect & co.) on the PS2's controller ports, through the BIOS's PADMAN.
 *
 * On the PS1, PadInitDirect hands the BIOS two receive buffers that it refreshes every vertical
 * blank: status (0 = ok, 0xFF = no controller), controller ID (0x41 digital, 0x73 analog), buttons
 * (16 bits, active low), then right X/Y and left X/Y sticks. ps2sdk's padButtonStatus starts with the
 * same 8 bytes, so they're copied as is. The buffers are refreshed by Pad_Poll(), called from VSync
 * (the game waits for a vertical blank once per frame).
 *
 * rom0:SIO2MAN and rom0:PADMAN are used (present in every BIOS, v1.00 included) with ps2sdk's libpad,
 * which talks to that PADMAN. Ports: PS1 socket 0x00 = port 1, 0x10 = port 2. Vibration goes through
 * padSetActAlign/padSetActDirect.
 */

#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <stdio.h>
#include <string.h>

#define PORTS 2

static unsigned char  s_PadArea[PORTS][256] __attribute__((aligned(64)));
static unsigned char* s_Recv[PORTS];
static int            s_Started;
static unsigned char  s_Act[PORTS][6];
static unsigned char* s_ActSrc[PORTS];
static int            s_ActLen[PORTS];
static int            s_LastState[PORTS] = { -1, -1 };
static int            s_Info[PORTS][4];      /* PadInfoMode terms 1-3 (current ID, extended ID, offset) */
static int            s_IdTable[PORTS][9];   /* term 4: offs -1 (count), then IDs 0-7 */

static int port_of(int socket)
{
    return (socket >> 4) & 1;
}

static void load_modules(void)
{
    static int loaded;
    if (loaded++)
    {
        return;
    }
    SifInitRpc(0);
    if (SifLoadModule("rom0:SIO2MAN", 0, NULL) < 0 || SifLoadModule("rom0:PADMAN", 0, NULL) < 0)
    {
        printf("libpad: can't load rom0:SIO2MAN/PADMAN\n");
    }
    padInit(0);
}

void PadInitDirect(unsigned char* pad1, unsigned char* pad2)
{
    int p;
    load_modules();
    s_Recv[0] = pad1;
    s_Recv[1] = pad2;
    for (p = 0; p < PORTS; p++)
    {
        if (s_Recv[p])
        {
            s_Recv[p][0] = 0xFF; /* no controller yet */
            s_Recv[p][1] = 0;
        }
    }
}

void PadStartCom(void)
{
    int p;
    load_modules();
    if (s_Started)
    {
        return;
    }
    for (p = 0; p < PORTS; p++)
    {
        padPortOpen(p, 0, s_PadArea[p]);
    }
    s_Started = 1;
}

void PadStopCom(void)
{
    int p;
    if (!s_Started)
    {
        return;
    }
    for (p = 0; p < PORTS; p++)
    {
        padPortClose(p, 0);
    }
    s_Started = 0;
}

/** Refreshes the receive buffers (the PS1 BIOS does this every vertical blank). */
void Pad_Poll(void)
{
    int p;
    if (!s_Started)
    {
        return;
    }
    for (p = 0; p < PORTS; p++)
    {
        struct padButtonStatus st;
        int                    state = padGetState(p, 0);

        if (s_ActSrc[p] && state == PAD_STATE_STABLE)
        {
            int n = s_ActLen[p] < 6 ? s_ActLen[p] : 6;
            memset(s_Act[p], 0, sizeof(s_Act[p]));
            memcpy(s_Act[p], s_ActSrc[p], n);
            padSetActDirect(p, 0, s_Act[p]);
        }
        if (state != s_LastState[p])
        {
            printf("libpad: port %d state %d\n", p + 1, state);
            s_LastState[p] = state;
        }
        /* Mode info, read once per frame: with the BIOS's PADMAN, padInfoMode can go through an IOP
         * RPC, which the game would otherwise call several times per frame. */
        if (state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1)
        {
            int i;
            for (i = 1; i <= 3; i++)
            {
                s_Info[p][i] = padInfoMode(p, 0, i, 0);
            }
            s_IdTable[p][0] = padInfoMode(p, 0, PAD_MODETABLE, -1);
            for (i = 0; i < 8; i++)
            {
                s_IdTable[p][i + 1] = i < s_IdTable[p][0] ? padInfoMode(p, 0, PAD_MODETABLE, i) : 0;
            }
        }
        else
        {
            memset(s_Info[p], 0, sizeof(s_Info[p]));
            memset(s_IdTable[p], 0, sizeof(s_IdTable[p]));
        }
        if (!s_Recv[p])
        {
            continue;
        }
        if ((state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1) && padRead(p, 0, &st) != 0 && st.ok == 0)
        {
            memcpy(s_Recv[p], &st, 8);
            /* Sticks resting near the centre read as exactly 80h, as PS1 pads (and DuckStation) report:
             * the game reads the live sticks even during the attract demo, and a PS2 pad at rest
             * (7Fh) changed Harry's run animation speed there. The game's own dead zone is larger. */
            {
                int k;
                for (k = 4; k < 8; k++)
                {
                    int d = (int)s_Recv[p][k] - 0x80;
                    if (d >= -8 && d <= 8)
                    {
                        s_Recv[p][k] = 0x80;
                    }
                }
            }
            if (p == 0)
            {
                /* Test presses from the map warp (warp_ps2.c); buttons are active low. */
                extern unsigned int Port_WarpButtons(void);
                unsigned int        press = Port_WarpButtons();
                s_Recv[p][2] &= (unsigned char)~(press & 0xFF);
                s_Recv[p][3] &= (unsigned char)~(press >> 8);
            }
        }
        else
        {
            s_Recv[p][0] = 0xFF;
            s_Recv[p][1] = 0;
        }
    }
}

int PadChkVsync(void)
{
    return s_Started; /* the buffers are refreshed once per vertical blank */
}

/** PS1 states: 0 disconnected, 1 searching, 2 executing a command, 6 stable. */
int PadGetState(int socket)
{
    int state = padGetState(port_of(socket), 0);
    switch (state)
    {
        case PAD_STATE_DISCONN: return 0;
        case PAD_STATE_EXECCMD: return 2;
        case PAD_STATE_STABLE:  return 6;
        case PAD_STATE_FINDCTP1: return 6; /* stable, but no extended (DualShock) modes */
    }
    return 1;
}

/** Terms match ps2sdk's: 1 current ID, 2 current extended ID, 3 current offset, 4 ID table (offs -1:
 * count). Answered from what Pad_Poll read this frame. */
int PadInfoMode(int socket, int term, int offs)
{
    int p = port_of(socket);
    if (term >= 1 && term <= 3)
    {
        return s_Info[p][term];
    }
    if (term == 4 && offs >= -1 && offs < 8)
    {
        return s_IdTable[p][offs + 1];
    }
    return 0;
}

int PadInfoAct(int socket, int actno, int term)
{
    return padInfoAct(port_of(socket), 0, actno, term);
}

int PadSetMainMode(int socket, int offs, int lock)
{
    return padSetMainMode(port_of(socket), 0, offs, lock);
}

int PadSetActAlign(int socket, unsigned char* align)
{
    unsigned char a[6];
    memcpy(a, align, 6);
    return padSetActAlign(port_of(socket), 0, a);
}

/** The PS1 sends the act data (motor levels) from this buffer every vertical blank. */
void PadSetAct(int socket, unsigned char* data, int len)
{
    int p        = port_of(socket);
    s_ActSrc[p] = data;
    s_ActLen[p] = len;
}
