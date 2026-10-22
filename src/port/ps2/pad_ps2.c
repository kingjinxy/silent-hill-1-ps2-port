/** @brief The PS2's controller ports through the BIOS's PADMAN (include/port/pad.h).
 *
 * rom0:SIO2MAN and rom0:PADMAN are used (present in every BIOS, v1.00 included) with ps2sdk's libpad,
 * which talks to that PADMAN. Polled once per vertical blank from VSync (libetc_ps2.c). Test input and
 * the reset combo are applied by input_ps2.c (Port_InputFilter); the PS1 libpad calls Konami's libkpad
 * makes are in libpad_psx.c.
 */

#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <stdio.h>
#include <string.h>
#include "port/pad.h"
#include "port/prof.h"

extern void Port_InputFilter(int port, unsigned char* data); /* input_ps2.c */

#define PORTS 2

typedef struct
{
    unsigned char        area[256] __attribute__((aligned(64))); /* padPortOpen's DMA area */
    unsigned char        data[8];       /* the PS1 controller record (pad.h) */
    int                  lastState;     /* for the state log */
    int                  infoState;     /* the state when the mode info was last read, -1 = read again */
    int                  infoAge;       /* polls since then */
    int                  info[4];       /* padInfoMode terms 1-3 */
    int                  modeTable[9];  /* term 4: count, then IDs 0-7 */
    const unsigned char* motorSrc;      /* Port_PadSetMotorSource */
    int                  motorLen;
    unsigned char        motorSent[6];  /* levels last sent */
    int                  motorValid;    /* motorSent was sent to the pad plugged in now */
    int                  notReady;      /* reads that weren't ready, logged every 600 polls */
} Pad;

static Pad s_Pads[PORTS] __attribute__((aligned(64)));
static int s_Open;
static int s_Polls;

void Port_PadModulesLoad(void)
{
    static int loaded;
    if (loaded++)
    {
        return;
    }
    SifInitRpc(0);
    if (SifLoadModule("rom0:SIO2MAN", 0, NULL) < 0 || SifLoadModule("rom0:PADMAN", 0, NULL) < 0)
    {
        printf("pad: can't load rom0:SIO2MAN/PADMAN\n");
    }
    padInit(0);
}

void Port_PadInit(void)
{
    int p;
    Port_PadModulesLoad();
    if (s_Open)
    {
        return;
    }
    for (p = 0; p < PORTS; p++)
    {
        s_Pads[p].data[0]   = 0xFF; /* no pad yet */
        s_Pads[p].data[1]   = 0;
        s_Pads[p].lastState = -1;
        s_Pads[p].infoState = -1;
        padPortOpen(p, 0, s_Pads[p].area);
    }
    s_Open = 1;
}

/** Before a restart (crash_ps2.c Port_Restart): the IOP's PADMAN writes the pad data into the port's
 * DMA area every vertical blank, and keeps doing it until the IOP reboots, which under Neutrino comes
 * after the next ELF is in memory. A new build has the area elsewhere: the writes then landed in its
 * code or data, and the first boot after every few deploys froze at the Konami logo (2026-10-10). */
void Port_PadShutdown(void)
{
    int p;
    if (s_Open)
    {
        for (p = 0; p < PORTS; p++)
        {
            padPortClose(p, 0);
        }
        s_Open = 0;
    }
    padEnd();
}

/** Sends the motor levels when they change: every padSetActDirect is a pad command (state EXECCMD
 * until it completes), and sending one every poll left half the reads not ready. */
static void motors_update(int p, Pad* pad)
{
    unsigned char act[6] = { 0 };
    memcpy(act, pad->motorSrc, pad->motorLen < 6 ? pad->motorLen : 6);
    if (!pad->motorValid || memcmp(act, pad->motorSent, 6) != 0)
    {
        memcpy(pad->motorSent, act, 6);
        padSetActDirect(p, 0, pad->motorSent);
        pad->motorValid = 1;
    }
}

/** Mode info, read when the pad's state changes (a mode change, e.g. the Analog button, goes through
 * other states) and once a second besides: with the BIOS's PADMAN, padInfoMode goes through an IOP
 * RPC, and the 12 calls cost about 210k cycles every frame (4% of a 60 fps frame) on hardware. */
static void mode_info_update(int p, Pad* pad, int state)
{
    int i;
    if (state != pad->infoState || ++pad->infoAge >= 60)
    {
        pad->infoState = state;
        pad->infoAge   = 0;
        PROF_BEGIN("pad: padInfoMode x12")
        for (i = 1; i <= 3; i++)
        {
            pad->info[i] = padInfoMode(p, 0, i, 0);
        }
        pad->modeTable[0] = padInfoMode(p, 0, PAD_MODETABLE, -1);
        for (i = 0; i < 8; i++)
        {
            pad->modeTable[i + 1] = i < pad->modeTable[0] ? padInfoMode(p, 0, PAD_MODETABLE, i) : 0;
        }
        PROF_END("pad: padInfoMode x12")
    }
}

static void poll_port(int p, Pad* pad)
{
    struct padButtonStatus st;
    int                    state, ready, r;

    PROF_BEGIN("pad: padGetState")
    state = padGetState(p, 0);
    PROF_END("pad: padGetState")
    ready = state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1;
    if (state != pad->lastState)
    {
        printf("pad: port %d state %d\n", p + 1, state);
        pad->lastState = state;
    }

    if (pad->motorSrc && state == PAD_STATE_STABLE)
    {
        motors_update(p, pad);
    }
    if (ready)
    {
        mode_info_update(p, pad, state);
    }
    else
    {
        pad->infoState = -1; /* read again when it's back */
    }

    if (state == PAD_STATE_DISCONN || state == PAD_STATE_FINDPAD)
    {
        memset(pad->info, 0, sizeof(pad->info));
        memset(pad->modeTable, 0, sizeof(pad->modeTable));
        pad->data[0]    = 0xFF;
        pad->data[1]    = 0;
        pad->motorValid = 0; /* a pad plugged in again gets the motor levels anew */
        return;
    }
    if (ready)
    {
        PROF_BEGIN("pad: padRead")
        r = padRead(p, 0, &st);
        PROF_END("pad: padRead")
        if (r != 0 && st.ok == 0)
        {
            memcpy(pad->data, &st, 8);
            Port_InputFilter(p, pad->data);
            return;
        }
    }
    /* A read that isn't ready, a mode change in progress: the previous data stays. The pad is polled
     * at any point of a frame, and a one-frame "disconnected" made the game drop held buttons
     * (running stopped every half second). */
    pad->notReady++;
}

void Port_PadPoll(void)
{
    int p;
    if (!s_Open)
    {
        return;
    }
    if (++s_Polls >= 600)
    {
        if (s_Pads[0].notReady || s_Pads[1].notReady)
        {
            printf("pad: reads not ready in the last 600 polls: port 1 %d, port 2 %d (previous data kept)\n",
                   s_Pads[0].notReady, s_Pads[1].notReady);
        }
        s_Polls = s_Pads[0].notReady = s_Pads[1].notReady = 0;
    }
    for (p = 0; p < PORTS; p++)
    {
        poll_port(p, &s_Pads[p]);
    }
}

int Port_PadIsOpen(void)
{
    return s_Open;
}

void Port_PadRead(int port, void* out)
{
    memcpy(out, s_Pads[port].data, 8);
}

unsigned int Port_PadButtons(void)
{
    const unsigned char* d = s_Pads[0].data;
    if (!s_Open || d[0] != 0)
    {
        return 0;
    }
    return ~(d[2] | (d[3] << 8)) & 0xFFFF;
}

int Port_PadState(int port)
{
    return padGetState(port, 0);
}

int Port_PadModeInfo(int port, int term, int offs)
{
    if (term >= 1 && term <= 3)
    {
        return s_Pads[port].info[term];
    }
    if (term == PAD_MODETABLE && offs >= -1 && offs < 8)
    {
        return s_Pads[port].modeTable[offs + 1];
    }
    return 0;
}

int Port_PadActInfo(int port, int actno, int term)
{
    return padInfoAct(port, 0, actno, term);
}

int Port_PadSetMainMode(int port, int mode, int lock)
{
    return padSetMainMode(port, 0, mode, lock);
}

int Port_PadSetActAlign(int port, const unsigned char* align)
{
    char a[6];
    memcpy(a, align, 6);
    return padSetActAlign(port, 0, a);
}

void Port_PadSetMotorSource(int port, const unsigned char* src, int len)
{
    s_Pads[port].motorSrc = src;
    s_Pads[port].motorLen = len;
}
