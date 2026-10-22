/** @brief Scripted input for testing: `host:input.txt` (next to the disc image, with PCSX2's host file
 * system on) is a timeline of button presses the port feeds to the game through the pad
 * (Port_InputFilter, below), one per line, in emulated seconds since boot:
 *
 *     12.5 start 6      hold Start for 6 frames (vertical blanks) at 12.5 s
 *     14   cross down   press Cross and Down together (default 6 frames)
 *     15   dump         write the next displayed frame to host:frame_<n>.ppm (display_ps2.c)
 *     20   crash        read address 8 in the main loop (tests the crash reporter on hardware)
 *     # comment
 *
 * Buttons: select l3 r3 start up right down left l2 r2 l1 r1 triangle circle cross square. Each event
 * is logged ("input: ..."). Without the file (or on hardware), nothing happens.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int  Port_VBlanks(unsigned int* cycles); /* libetc_ps2.c */
extern void Display_RequestDump(void);          /* display_ps2.c */

#define MAX_EVENTS 128

typedef struct
{
    int          at;      /* vertical blank */
    unsigned int buttons; /* PS1 bit order, set = pressed; 0 with dump */
    int          frames;
    int          dump;
    int          crash; /* test: raise an address error (crash reporter, crash_ps2.c) */
} Event;

static Event s_Events[MAX_EVENTS];
static int   s_Count;
static int   s_Loaded = -1;
static int   s_Next;      /* next event to start */
static int   s_ActiveEnd; /* vertical blank where the current press ends */
static unsigned int s_Active;
static int          s_Crash; /* raised from the main thread (Port_InputCrashCheck), not the pad poll */

static const char* const NAMES[16] = { "select", "l3", "r3", "start", "up", "right", "down", "left",
                                        "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square" };

static void load(void)
{
    static char buf[4096];
    FILE*       f = fopen("host:input.txt", "r");
    char*       line;
    int         n;
    s_Loaded = 0;
    if (!f)
    {
        return;
    }
    n = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n > 0 ? n : 0] = 0;
    for (line = strtok(buf, "\n"); line && s_Count < MAX_EVENTS; line = strtok(NULL, "\n"))
    {
        Event e;
        char* tok;
        char* save;
        memset(&e, 0, sizeof(e));
        e.frames = 6;
        if ((tok = strchr(line, '#')))
        {
            *tok = 0;
        }
        tok = strtok_r(line, " \t\r", &save);
        if (!tok)
        {
            continue;
        }
        e.at = (int)(atof(tok) * 60.0 + 0.5);
        while ((tok = strtok_r(NULL, " \t\r", &save)))
        {
            int i;
            if (!strcmp(tok, "dump"))
            {
                e.dump = 1;
                continue;
            }
            if (!strcmp(tok, "crash"))
            {
                e.crash = 1;
                continue;
            }
            if (tok[0] >= '0' && tok[0] <= '9')
            {
                e.frames = atoi(tok);
                continue;
            }
            for (i = 0; i < 16; i++)
            {
                if (!strcmp(tok, NAMES[i]))
                {
                    e.buttons |= 1u << i;
                }
            }
        }
        s_Events[s_Count++] = e;
    }
    s_Loaded = s_Count > 0;
    if (s_Loaded)
    {
        printf("input: %d events\n", s_Count);
    }
}

/** Buttons to press now (called from Port_InputFilter, once per vertical blank). */
unsigned int Port_InputButtons(void)
{
    unsigned int cycles;
    int          now;
    if (s_Loaded < 0)
    {
        load();
    }
    if (s_Loaded <= 0)
    {
        return 0;
    }
    now = Port_VBlanks(&cycles);
    if (s_Active && now >= s_ActiveEnd)
    {
        s_Active = 0;
    }
    while (s_Next < s_Count && now >= s_Events[s_Next].at)
    {
        const Event* e = &s_Events[s_Next++];
        if (e->dump)
        {
            Display_RequestDump();
            printf("input: %.2f s dump\n", now / 60.0);
        }
        if (e->crash)
        {
            printf("input: %.2f s crash (test)\n", now / 60.0);
            s_Crash = 1;
        }
        if (e->buttons)
        {
            s_Active    = e->buttons;
            s_ActiveEnd = now + e->frames;
            printf("input: %.2f s press %x for %d frames\n", now / 60.0, e->buttons, e->frames);
        }
    }
    return s_Active;
}

/** Test crash requested by host:input.txt ("crash"): a read of address 8 in the caller's thread (the
 * main loop calls this once per frame): a TLB fault on a real PS2. PCSX2 lets it pass, so there the
 * crash reporter can't be tested this way. */
void Port_InputCrashCheck(void)
{
#ifdef SH_PORT_TEST_CRASH
    /* Test build (SH1_TEST_CRASH=1): the same fault as a real crash, 30 s after boot, for testing the
     * crash reporter and its restart on hardware (where host:input.txt isn't available). */
    unsigned int cycles;
    static int   done;
    if (!done && Port_VBlanks(&cycles) >= 30 * 60)
    {
        done    = 1;
        s_Crash = 1;
    }
#endif
    if (s_Crash)
    {
        s_Crash = 0;
        (void)*(volatile unsigned int*)8; /* TLB load fault (a real PS2; PCSX2 lets it pass) */
    }
}

/** Applied to every pad read (pad_ps2.c, once per vertical blank) before the game sees it. `data` is
 * the PS1 controller record (include/port/pad.h). */
void Port_InputFilter(int port, unsigned char* data)
{
    extern void         Port_AgentRequest(const char* cmd); /* agent_ps2.c */
    extern unsigned int Port_WarpButtons(void);             /* warp_ps2.c */
    static int          resetHeld;
    unsigned int        held, press;
    int                 k;

    /* Sticks resting near the centre read as exactly 80h, as PS1 pads (and DuckStation) report: the
     * game reads the live sticks even during the attract demo, and a PS2 pad at rest (7Fh) changed
     * Harry's run animation speed there. The game's own dead zone is larger. */
    for (k = 4; k < 8; k++)
    {
        int d = (int)data[k] - 0x80;
        if (d >= -8 && d <= 8)
        {
            data[k] = 0x80;
        }
    }
    if (port != 0)
    {
        return;
    }

    /* In-game reset: L1+L2+R1+R2+Select+Start held for a second exits to the PS2 browser (the RESET
     * button doesn't under Neutrino). PS1 bit order, active low. */
    held = ~(data[2] | (data[3] << 8)) & 0xFFFF;
    if ((held & 0x0F09) == 0x0F09)
    {
        if (++resetHeld == 60)
        {
            printf("input: reset combo held: exiting to the browser\n");
            Port_AgentRequest("OS");
        }
    }
    else
    {
        resetHeld = 0;
    }

    /* Test presses from the map warp (warp_ps2.c) and scripted input (above); active low. */
    press = Port_WarpButtons() | Port_InputButtons();
    data[2] &= (unsigned char)~(press & 0xFF);
    data[3] &= (unsigned char)~(press >> 8);
}
