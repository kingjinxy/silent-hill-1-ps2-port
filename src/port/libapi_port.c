/** @brief PS1 kernel events (libapi OpenEvent/TestEvent/...) for the port.
 *
 * Events are kept in a small table; a handle is 0xF1000000 | index, as on the PS1. Delivering an
 * event sets its "ready" flag (polled with TestEvent, which clears it) and, for an enabled
 * EvMdINTR event, calls its handler.
 *
 * libcard (memory cards) is in psx_card.c; TestEvent first lets it deliver the events of card
 * queries that have finished (Port_CardPoll).
 * Root counter 2's event (the sound driver's tick) is delivered by src/port/ps2/spu_ps2.c.
 */

#define EVENT_COUNT 32
#define EVENT_BASE  0xF1000000UL

#define EvMdINTR   0x1000

typedef struct
{
    unsigned long cls;
    unsigned long spec;
    long          mode;
    long        (*func)(void);
    int           used;
    int           enabled;
    int           ready;
} Event;

static Event s_Events[EVENT_COUNT];

static Event* event_get(unsigned long ev)
{
    unsigned long i = ev - EVENT_BASE;
    return (i < EVENT_COUNT && s_Events[i].used) ? &s_Events[i] : 0;
}

static void event_deliver(unsigned long cls, unsigned long spec)
{
    int i;
    for (i = 0; i < EVENT_COUNT; i++)
    {
        Event* e = &s_Events[i];
        if (e->used && e->cls == cls && e->spec == spec)
        {
            e->ready = 1;
            if (e->enabled && e->mode == EvMdINTR && e->func)
            {
                e->func();
            }
        }
    }
}

/** Delivers an event from the port's platform code (root counter interrupts). */
void Port_EventDeliver(unsigned long cls, unsigned long spec)
{
    event_deliver(cls, spec);
}

long OpenEvent(unsigned long cls, long spec, long mode, long (*func)(void))
{
    int i;
    for (i = 0; i < EVENT_COUNT; i++)
    {
        if (!s_Events[i].used)
        {
            Event* e   = &s_Events[i];
            e->cls     = cls;
            e->spec    = (unsigned long)spec;
            e->mode    = mode;
            e->func    = func;
            e->used    = 1;
            e->enabled = 0;
            e->ready   = 0;
            return (long)(EVENT_BASE | i);
        }
    }
    return -1;
}

long CloseEvent(unsigned long ev)
{
    Event* e = event_get(ev);
    if (!e)
    {
        return 0;
    }
    e->used = 0;
    return 1;
}

long EnableEvent(unsigned long ev)
{
    Event* e = event_get(ev);
    if (!e)
    {
        return 0;
    }
    e->enabled = 1;
    return 1;
}

long DisableEvent(unsigned long ev)
{
    Event* e = event_get(ev);
    if (!e)
    {
        return 0;
    }
    e->enabled = 0;
    return 1;
}

extern void Port_CardPoll(void); /* psx_card.c */

long TestEvent(unsigned long ev)
{
    Event* e;
    Port_CardPoll();
    e = event_get(ev);
    if (!e || !e->ready)
    {
        return 0;
    }
    e->ready = 0;
    return 1;
}

long WaitEvent(unsigned long ev)
{
    return TestEvent(ev);
}

void DeliverEvent(unsigned long cls, unsigned long spec)
{
    event_deliver(cls, spec);
}
