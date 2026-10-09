/** @brief PS1 kernel events (libapi OpenEvent/TestEvent/...) and libcard for the port.
 *
 * Events are kept in a small table; a handle is 0xF1000000 | index, as on the PS1. Delivering an
 * event sets its "ready" flag (polled with TestEvent, which clears it) and, for an enabled
 * EvMdINTR event, calls its handler.
 *
 * Memory cards aren't implemented yet: libcard behaves as on a PS1 with both slots empty, where
 * every card operation is accepted and then ends with a time-out (SwCARD and HwCARD EvSpTIMOUT).
 * Root counter 2's event (the sound driver's tick) is delivered by src/port/ps2/spu_ps2.c.
 */

#define EVENT_COUNT 32
#define EVENT_BASE  0xF1000000UL

#define HwCARD     0xF0000011UL
#define SwCARD     0xF4000001UL
#define EvSpTIMOUT 0x0100
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

long TestEvent(unsigned long ev)
{
    Event* e = event_get(ev);
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

/* libcard: no cards inserted. */

static long card_timeout(void)
{
    event_deliver(HwCARD, EvSpTIMOUT);
    event_deliver(SwCARD, EvSpTIMOUT);
    return 1;
}

void InitCARD(long val)
{
    (void)val;
}

long StartCARD(void)
{
    return 1;
}

long StopCARD(void)
{
    return 1;
}

void _bu_init(void)
{
}

void _new_card(void)
{
}

long _card_info(long chan)
{
    (void)chan;
    return card_timeout();
}

long _card_clear(long chan)
{
    (void)chan;
    return card_timeout();
}

long _card_load(long chan)
{
    (void)chan;
    return card_timeout();
}

long _card_read(long chan, long block, unsigned char* buf)
{
    (void)chan;
    (void)block;
    (void)buf;
    return card_timeout();
}

long _card_write(long chan, long block, unsigned char* buf)
{
    (void)chan;
    (void)block;
    (void)buf;
    return card_timeout();
}
