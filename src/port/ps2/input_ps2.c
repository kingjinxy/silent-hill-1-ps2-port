/** @brief Scripted input for testing: `host:input.txt` (next to the disc image, with PCSX2's host file
 * system on) is a timeline of button presses the port feeds to the game through the pad (libpad_ps2.c),
 * one per line, in emulated seconds since boot:
 *
 *     12.5 start 6      hold Start for 6 frames (vertical blanks) at 12.5 s
 *     14   cross down   press Cross and Down together (default 6 frames)
 *     15   dump         write the next displayed frame to host:frame_<n>.ppm (display_ps2.c)
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
} Event;

static Event s_Events[MAX_EVENTS];
static int   s_Count;
static int   s_Loaded = -1;
static int   s_Next;      /* next event to start */
static int   s_ActiveEnd; /* vertical blank where the current press ends */
static unsigned int s_Active;

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

/** Buttons to press now (called from the pad poll, once per vertical blank). */
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
        if (e->buttons)
        {
            s_Active    = e->buttons;
            s_ActiveEnd = now + e->frames;
            printf("input: %.2f s press %x for %d frames\n", now / 60.0, e->buttons, e->frames);
        }
    }
    return s_Active;
}
