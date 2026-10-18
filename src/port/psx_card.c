/** @brief PS1 memory cards for the port: libcard (_card_info & co.) and the BIOS file functions on
 * "buXY:" paths, over one PS1 memory card image per PS1 card slot.
 *
 * Each PS1 slot is a 128 KB image in the standard PS1 format (block 0: header, 15 directory frames,
 * broken-sector list; blocks 1-15: file data), the same as an emulator's .mcd file. It's kept in EE
 * memory and stored on the PS2 memory card in the same port, inside one save folder
 * (src/port/ps2/memcard_ps2.c: PS1 slot 1 = PS2 card 1, slot 2 = card 2). The game sees a PS1 card
 * with 15 blocks; its save file (BASLUS-00707SILENT00, one block of 11 save slots) lives in the image.
 * A PS2 card without the save folder appears as an empty, formatted PS1 card; the folder is created
 * when the game first writes. Multitap slots (bu01-03, bu11-13) report no card.
 *
 * Results come back as on the PS1: libcard calls return 1 and deliver SwCARD/HwCARD events
 * (EvSpIOE done, EvSpNEW new card, EvSpTIMOUT no card, EvSpERROR); _card_info's PS2 card query runs
 * asynchronously (the game polls every card slot each frame) and its event is delivered when it ends
 * (Port_CardPoll, called from TestEvent). File reads and writes complete at once (asynchronous ones,
 * O_NOWAIT, also deliver SwCARD EvSpIOE); writes go straight to the PS2 card.
 *
 * src/bodyprog/memcard.c maps the game's open/read/write/lseek/close/firstfile/nextfile/erase/rename/
 * format to the Port_Bu* functions here (the plain names belong to the PS2's C library). */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

extern int   printf(const char* fmt, ...);
extern void* memcpy(void* d, const void* s, unsigned int n);
extern void* memset(void* d, int c, unsigned int n);
extern int   VSync(int mode);
extern void  Port_EventDeliver(unsigned long cls, unsigned long spec); /* libapi_port.c */

/* PS2 side (src/port/ps2/memcard_ps2.c). */
extern void PortMc_InfoStart(int slot);
extern int  PortMc_InfoPoll(int slot, int* state); /* 1 when done: state 0 same card, 1 new card, -1 none */
extern int  PortMc_LoadImage(int slot, u8* img);    /* 1 loaded, 0 no save folder, -1 error */
extern int  PortMc_StoreImage(int slot, const u8* img, int offset, int length); /* 0 ok, -1 error */
extern void PortMc_SetIcon(const u8* ico, int icoLength);

#define HwCARD      0xF0000011UL
#define SwCARD      0xF4000001UL
#define EvSpIOE     0x0004
#define EvSpTIMOUT  0x0100
#define EvSpNEW     0x2000
#define EvSpERROR   0x8000

/* PS1 BIOS open() flags (include/psyq/sys/fcntl.h). */
#define FREAD  0x0001
#define FWRITE 0x0002
#define FCREAT 0x0200
#define FASYNC 0x8000

#define CARD_SIZE  0x20000
#define BLOCK_SIZE 0x2000
#define FRAME_SIZE 0x80
#define DIR_COUNT  15

/* Directory frame states. */
#define DIR_FREE   0xA0 /* deleted: 0xA1 first, 0xA2 middle, 0xA3 last block */
#define DIR_FIRST  0x51
#define DIR_MIDDLE 0x52
#define DIR_LAST   0x53

/* DIRENTRY as the game declares it (include/psyq/kernel.h). */
struct DIRENTRY
{
    char             name[20];
    long             attr;
    long             size;
    struct DIRENTRY* next;
    long             head;
    char             system[4];
};

typedef struct
{
    u8  img[CARD_SIZE] __attribute__((aligned(64))); /* the PS2 card driver transfers it by DMA */
    int present;     /* the last query found a formatted PS2 card */
    int known;       /* a query has finished since boot */
    int isNew;       /* "new card": set when a different card is seen, cleared by _card_clear */
    int loaded;      /* img holds this card's data */
    int infoPending; /* a _card_info is waiting for the PS2 card query */
    int infoAt;      /* vertical blank when the last query finished */
} Card;

static Card s_Card[2];

#define INFO_CACHE_VBLANKS 30 /* _card_info answers from the last query for half a second */

/* --- PS1 card format ---------------------------------------------------------------------------*/

static u8* frame(int slot, int i)
{
    return &s_Card[slot].img[i * FRAME_SIZE];
}

static u32 rd32(const u8* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static void wr32(u8* p, u32 v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static u16 rd16(const u8* p)
{
    return p[0] | (p[1] << 8);
}

static void wr16(u8* p, u16 v)
{
    p[0] = v;
    p[1] = v >> 8;
}

/** Byte 0x7F of a frame: XOR of bytes 0-0x7E. */
static void checksum(u8* f)
{
    u8  x = 0;
    int i;
    for (i = 0; i < FRAME_SIZE - 1; i++)
    {
        x ^= f[i];
    }
    f[FRAME_SIZE - 1] = x;
}

/** A freshly formatted PS1 card. */
static void card_format(int slot)
{
    u8* img = s_Card[slot].img;
    int i;
    memset(img, 0, CARD_SIZE);
    img[0] = 'M';
    img[1] = 'C';
    checksum(frame(slot, 0));
    for (i = 1; i <= DIR_COUNT; i++)
    {
        u8* f = frame(slot, i);
        wr32(f, DIR_FREE);
        wr16(f + 8, 0xFFFF);
        checksum(f);
    }
    for (i = 16; i < 36; i++) /* broken sector list: none */
    {
        u8* f = frame(slot, i);
        wr32(f, 0xFFFFFFFF);
        wr16(f + 8, 0xFFFF);
        checksum(f);
    }
    memcpy(frame(slot, 63), frame(slot, 0), FRAME_SIZE); /* write test frame */
}

static int name_eq(const u8* entryName, const char* name)
{
    int i;
    for (i = 0; i < 21; i++)
    {
        if (entryName[i] != (u8)name[i])
        {
            return 0;
        }
        if (name[i] == 0)
        {
            return 1;
        }
    }
    return name[21 - 1] == 0;
}

/** Directory index (1-15) of the file's first block, or 0. */
static int file_find(int slot, const char* name)
{
    int i;
    for (i = 1; i <= DIR_COUNT; i++)
    {
        u8* f = frame(slot, i);
        if ((rd32(f) & 0xFF) == DIR_FIRST && name_eq(f + 0x0A, name))
        {
            return i;
        }
    }
    return 0;
}

static int file_size(int slot, int first)
{
    return (int)rd32(frame(slot, first) + 4);
}

/** Directory index of the file's block k (0 = first), or 0 past its end. */
static int file_block(int slot, int first, int k)
{
    int i = first;
    while (k-- > 0)
    {
        u16 next = rd16(frame(slot, i) + 8);
        if (next == 0xFFFF || next >= DIR_COUNT)
        {
            return 0;
        }
        i = next + 1;
    }
    return i;
}

/** Stores block 0 (the directory) on the PS2 card. */
static int store_dir(int slot)
{
    return PortMc_StoreImage(slot, s_Card[slot].img, 0, BLOCK_SIZE);
}

/* --- Paths and handles -------------------------------------------------------------------------*/

/** "buXY:NAME": the PS1 slot (0/1) and the name; -1 for anything else (and multitap slots). */
static int path_parse(const char* path, const char** name)
{
    if (path[0] != 'b' || path[1] != 'u' || (path[2] != '0' && path[2] != '1') || path[4] != ':')
    {
        return -1;
    }
    if (path[3] != '0')
    {
        return -1; /* multitap */
    }
    *name = path + 5;
    return path[2] - '0';
}

/** The card's data, loading it if needed (the game normally reads the directory first). */
#ifdef SH_PORT_MC_SELFTEST
static void self_test(int slot);
#endif

static int ensure_loaded(int slot)
{
    Card* c = &s_Card[slot];
    int   r;
    if (c->loaded)
    {
        return 1;
    }
    r = PortMc_LoadImage(slot, c->img);
    if (r < 0)
    {
        return 0;
    }
    if (r == 0 || c->img[0] != 'M' || c->img[1] != 'C')
    {
        if (r != 0)
        {
            printf("psx card: slot %d: the saved image isn't a PS1 card; using an empty one\n", slot + 1);
        }
        card_format(slot);
    }
    c->loaded = 1;
#ifdef SH_PORT_MC_SELFTEST
    self_test(slot);
#endif
    return 1;
}

#define FD_COUNT 4
#define FD_BASE  2

typedef struct
{
    int used, slot, first, pos, flags;
} Handle;

static Handle s_Fd[FD_COUNT];

static Handle* fd_get(int fd)
{
    int i = fd - FD_BASE;
    return (i >= 0 && i < FD_COUNT && s_Fd[i].used) ? &s_Fd[i] : 0;
}

/* --- libcard -----------------------------------------------------------------------------------*/

static void sw(unsigned long spec)
{
    Port_EventDeliver(SwCARD, spec);
}

static void hw(unsigned long spec)
{
    Port_EventDeliver(HwCARD, spec);
}

/** _card_info's answer from the last query. */
static void info_answer(int slot)
{
    Card* c = &s_Card[slot];
    sw(!c->present ? EvSpTIMOUT : c->isNew ? EvSpNEW : EvSpIOE);
}

/** A finished query: the card's state. */
static void info_done(int slot, int state)
{
    Card* c = &s_Card[slot];
    if (state < 0)
    {
        if (c->present)
        {
            printf("psx card: slot %d: no card\n", slot + 1);
        }
        c->present = 0;
        c->loaded  = 0;
    }
    else if (state == 1 || !c->present)
    {
        printf("psx card: slot %d: card %s\n", slot + 1, c->known ? "inserted" : "found");
        c->present = 1;
        c->isNew   = 1;
        c->loaded  = 0;
    }
    c->known  = 1;
    c->infoAt = VSync(-1);
}

/** Delivers the events of finished card queries (called from TestEvent). */
void Port_CardPoll(void)
{
    int slot, state;
    for (slot = 0; slot < 2; slot++)
    {
        if (s_Card[slot].infoPending && PortMc_InfoPoll(slot, &state))
        {
            s_Card[slot].infoPending = 0;
            info_done(slot, state);
            info_answer(slot);
        }
    }
}

extern const unsigned int g_MemCard_SaveIconTim[48]; /* src/bodyprog/memcard.c */
static void icon_build(void);

void InitCARD(long val)
{
    (void)val;
    icon_build();
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
    int   slot = (chan >> 4) & 1;
    Card* c    = &s_Card[slot];
    if (chan & 3)
    {
        sw(EvSpTIMOUT); /* multitap slot */
        return 1;
    }
    if (c->infoPending)
    {
        return 1; /* answered when the query in flight ends */
    }
    if (c->known && VSync(-1) - c->infoAt < INFO_CACHE_VBLANKS)
    {
        info_answer(slot);
        return 1;
    }
    c->infoPending = 1;
    PortMc_InfoStart(slot);
    return 1;
}

long _card_clear(long chan)
{
    Card* c = &s_Card[(chan >> 4) & 1];
    if ((chan & 3) || !c->present)
    {
        hw(EvSpTIMOUT);
        return 1;
    }
    c->isNew = 0;
    hw(EvSpIOE);
    return 1;
}

long _card_load(long chan)
{
    int slot = (chan >> 4) & 1;
    if ((chan & 3) || !s_Card[slot].present || !ensure_loaded(slot))
    {
        sw(EvSpTIMOUT);
        return 1;
    }
    sw(EvSpIOE);
    return 1;
}

long _card_read(long chan, long block, unsigned char* buf)
{
    int slot = (chan >> 4) & 1;
    if ((chan & 3) || !s_Card[slot].present || block < 0 || block >= CARD_SIZE / FRAME_SIZE || !ensure_loaded(slot))
    {
        hw(EvSpTIMOUT);
        return 1;
    }
    memcpy(buf, frame(slot, block), FRAME_SIZE);
    hw(EvSpIOE);
    return 1;
}

/** Raw sector write. The game only uses it (after _new_card, on sector 0) to test that a card is
 * there (MemCard_DeviceTest, part of its unused format feature): that sector isn't written. */
long _card_write(long chan, long block, unsigned char* buf)
{
    int slot = (chan >> 4) & 1;
    if ((chan & 3) || !s_Card[slot].present || block < 0 || block >= CARD_SIZE / FRAME_SIZE || !ensure_loaded(slot))
    {
        hw(EvSpTIMOUT);
        return 1;
    }
    if (block != 0)
    {
        memcpy(frame(slot, block), buf, FRAME_SIZE);
        if (PortMc_StoreImage(slot, s_Card[slot].img, (block * FRAME_SIZE) & ~(BLOCK_SIZE - 1), BLOCK_SIZE) < 0)
        {
            hw(EvSpTIMOUT);
            return 1;
        }
    }
    hw(EvSpIOE);
    return 1;
}

/* --- BIOS file functions ------------------------------------------------------------------------*/

long Port_BuOpen(const char* path, unsigned long flags)
{
    const char* name;
    int         slot = path_parse(path, &name), first, i;
    Handle*     h    = 0;

    if (slot < 0 || !s_Card[slot].present || !ensure_loaded(slot) || name[0] == 0)
    {
        return -1;
    }
    first = file_find(slot, name);
    if (flags & FCREAT)
    {
        int need = (int)(flags >> 16), freeIdx[DIR_COUNT], n = 0, k;
        if (first)
        {
            return -1; /* exists */
        }
        if (need < 1)
        {
            need = 1;
        }
        for (i = 1; i <= DIR_COUNT && n < need; i++)
        {
            if ((rd32(frame(slot, i)) & 0xF0) == DIR_FREE)
            {
                freeIdx[n++] = i;
            }
        }
        if (n < need)
        {
            return -1; /* card full */
        }
        for (k = 0; k < need; k++)
        {
            u8* f = frame(slot, freeIdx[k]);
            memset(f, 0, FRAME_SIZE);
            wr32(f, k == 0 ? DIR_FIRST : k == need - 1 ? DIR_LAST : DIR_MIDDLE);
            wr32(f + 4, k == 0 ? (u32)need * BLOCK_SIZE : 0);
            wr16(f + 8, k == need - 1 ? 0xFFFF : (u16)(freeIdx[k + 1] - 1));
            if (k == 0)
            {
                for (i = 0; i < 20 && name[i]; i++)
                {
                    f[0x0A + i] = name[i];
                }
            }
            if (need == 1)
            {
                wr32(f, DIR_FIRST); /* a one-block file: first and last */
            }
            checksum(f);
        }
        if (store_dir(slot) < 0)
        {
            return -1;
        }
        first = freeIdx[0];
    }
    if (!first)
    {
        return -1;
    }
    for (i = 0; i < FD_COUNT; i++)
    {
        if (!s_Fd[i].used)
        {
            h = &s_Fd[i];
            break;
        }
    }
    if (!h)
    {
        return -1;
    }
    h->used  = 1;
    h->slot  = slot;
    h->first = first;
    h->pos   = 0;
    h->flags = (int)flags;
    return FD_BASE + (h - s_Fd);
}

long Port_BuClose(long fd)
{
    Handle* h = fd_get((int)fd);
    if (!h)
    {
        return -1;
    }
    h->used = 0;
    return fd;
}

long Port_BuLseek(long fd, long offset, long whence)
{
    Handle* h = fd_get((int)fd);
    long    pos;
    if (!h)
    {
        return -1;
    }
    pos = whence == 0 ? offset : whence == 1 ? h->pos + offset : file_size(h->slot, h->first) + offset;
    if (pos < 0 || pos > file_size(h->slot, h->first))
    {
        return -1;
    }
    h->pos = (int)pos;
    return pos;
}

/** Reads or writes `n` bytes at the handle's position, block by block along the file's chain. */
static long rw(long fd, void* buf, long n, int write)
{
    Handle* h = fd_get((int)fd);
    u8*     p = (u8*)buf;
    long    done = 0;
    if (!h || n < 0 || !(h->flags & (write ? FWRITE : FREAD)))
    {
        return -1;
    }
    if (n > file_size(h->slot, h->first) - h->pos)
    {
        n = file_size(h->slot, h->first) - h->pos;
    }
    while (done < n)
    {
        int blk = file_block(h->slot, h->first, h->pos / BLOCK_SIZE), in = h->pos % BLOCK_SIZE;
        int len = BLOCK_SIZE - in;
        u8* data;
        if (!blk)
        {
            break;
        }
        if (len > n - done)
        {
            len = (int)(n - done);
        }
        data = &s_Card[h->slot].img[blk * BLOCK_SIZE + in];
        if (write)
        {
            memcpy(data, p + done, len);
            if (PortMc_StoreImage(h->slot, s_Card[h->slot].img, blk * BLOCK_SIZE + (in & ~(FRAME_SIZE - 1)),
                                  ((in + len + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1)) - (in & ~(FRAME_SIZE - 1))) < 0)
            {
                if (h->flags & FASYNC)
                {
                    sw(EvSpTIMOUT);
                    return n; /* accepted; the event reports the failure */
                }
                return -1;
            }
        }
        else
        {
            memcpy(p + done, data, len);
        }
        h->pos += len;
        done += len;
    }
    if (h->flags & FASYNC)
    {
        sw(EvSpIOE);
    }
    return done;
}

long Port_BuRead(long fd, void* buf, long n)
{
    return rw(fd, buf, n, 0);
}

long Port_BuWrite(long fd, void* buf, long n)
{
    return rw(fd, buf, n, 1);
}

/* firstfile/nextfile: the pattern ('?' one character, '*' the rest) and where the listing stopped. */
static int  s_FindSlot = -1, s_FindNext;
static char s_FindPattern[21];

static int pattern_match(const char* pat, const u8* name)
{
    int i;
    for (i = 0; i < 21; i++)
    {
        if (pat[i] == '*')
        {
            return 1;
        }
        if (pat[i] == 0)
        {
            return name[i] == 0;
        }
        if (pat[i] != '?' && (u8)pat[i] != name[i])
        {
            return 0;
        }
    }
    return 1;
}

struct DIRENTRY* Port_BuNextFile(struct DIRENTRY* de)
{
    int slot = s_FindSlot, i;
    if (slot < 0)
    {
        return 0;
    }
    for (i = s_FindNext; i <= DIR_COUNT; i++)
    {
        u8* f = frame(slot, i);
        if ((rd32(f) & 0xFF) == DIR_FIRST && pattern_match(s_FindPattern, f + 0x0A))
        {
            /* name[20] then attr: a 20-character name ends with attr's low byte, which must be 0
             * (the game copies the name with strcpy). */
            memcpy(de->name, f + 0x0A, 20);
            de->attr   = 0;
            de->size   = file_size(slot, i);
            de->next   = 0;
            de->head   = i;
            s_FindNext = i + 1;
            return de;
        }
    }
    s_FindSlot = -1;
    return 0;
}

struct DIRENTRY* Port_BuFirstFile(const char* pattern, struct DIRENTRY* de)
{
    const char* name;
    int         slot = path_parse(pattern, &name), i;
    s_FindSlot       = -1;
    if (slot < 0 || !s_Card[slot].present || !ensure_loaded(slot))
    {
        return 0;
    }
    for (i = 0; i < 20 && name[i]; i++)
    {
        s_FindPattern[i] = name[i];
    }
    s_FindPattern[i] = 0;
    s_FindSlot       = slot;
    s_FindNext       = 1;
    return Port_BuNextFile(de);
}

long Port_BuErase(const char* path)
{
    const char* name;
    int         slot = path_parse(path, &name), first, i, k = 0;
    if (slot < 0 || !s_Card[slot].present || !ensure_loaded(slot) || !(first = file_find(slot, name)))
    {
        return 0;
    }
    for (i = first; i; k++)
    {
        u8* f    = frame(slot, i);
        u16 next = rd16(f + 8);
        wr32(f, (rd32(f) & ~0xFFu) | (0xA0 | (rd32(f) & 0x0F)));
        checksum(f);
        i = (next == 0xFFFF || next >= DIR_COUNT || k >= DIR_COUNT) ? 0 : next + 1;
    }
    return store_dir(slot) < 0 ? 0 : 1;
}

long Port_BuRename(const char* oldPath, const char* newPath)
{
    const char *oldName, *newName;
    int         slot = path_parse(oldPath, &oldName), first, i;
    if (slot < 0 || path_parse(newPath, &newName) != slot || !s_Card[slot].present || !ensure_loaded(slot) ||
        !(first = file_find(slot, oldName)) || file_find(slot, newName))
    {
        return 0;
    }
    {
        u8* f = frame(slot, first);
        memset(f + 0x0A, 0, 21);
        for (i = 0; i < 20 && newName[i]; i++)
        {
            f[0x0A + i] = newName[i];
        }
        checksum(f);
    }
    return store_dir(slot) < 0 ? 0 : 1;
}

long Port_BuFormat(const char* path)
{
    const char* name;
    int         slot = path_parse(path, &name);
    if (slot < 0 || !s_Card[slot].present)
    {
        return 0;
    }
    card_format(slot);
    s_Card[slot].loaded = 1;
    return PortMc_StoreImage(slot, s_Card[slot].img, 0, CARD_SIZE) < 0 ? 0 : 1;
}

/* --- The save's icon in the PS2 browser ----------------------------------------------------------*/

/* A PS2 icon file (.ico): one flat square (two triangles) facing the viewer, one shape, one frame,
 * textured with the game's PS1 save icon (16x16, 4-bit) scaled to the icon's 128x128 texture. */
static u8 s_Icon[20 + 6 * 24 + 20 + 24 + 128 * 128 * 2] __attribute__((aligned(64)));

static u8* put32(u8* p, u32 v)
{
    wr32(p, v);
    return p + 4;
}

static u8* put16(u8* p, u16 v)
{
    wr16(p, v);
    return p + 2;
}

static void icon_build(void)
{
    static const short X[6] = { -1, 1, -1, 1, 1, -1 }, Y[6] = { -1, -1, 1, -1, 1, 1 };
    const u8* tim  = (const u8*)g_MemCard_SaveIconTim;
    const u8* clut = tim + 8 + 12;                      /* TIM header, CLUT block header */
    const u8* pix  = tim + 8 + rd32(tim + 8) + 12;       /* image block, after its header */
    u8*       p    = s_Icon;
    int       i, x, y;

    p = put32(p, 0x00010000); /* version */
    p = put32(p, 1);          /* animation shapes */
    p = put32(p, 0x07);       /* texture: uncompressed */
    p = put32(p, 0x3F800000); /* 1.0f */
    p = put32(p, 6);          /* vertices */
    for (i = 0; i < 6; i++)
    {
        p = put16(p, (u16)(X[i] * 0x0C00));        /* x */
        p = put16(p, (u16)((Y[i] - 1) * 0x0C00));  /* y: from -0x1800 (top) to 0 (bottom) */
        p = put16(p, 0);                           /* z */
        p = put16(p, 0);
        p = put16(p, 0);                           /* normal: towards the viewer */
        p = put16(p, 0);
        p = put16(p, (u16)-0x1000);
        p = put16(p, 0);
        p = put16(p, X[i] < 0 ? 0 : 0x1000);       /* u */
        p = put16(p, Y[i] < 0 ? 0 : 0x1000);       /* v */
        *p++ = 0x80;                               /* vertex colour */
        *p++ = 0x80;
        *p++ = 0x80;
        *p++ = 0x80;
    }
    p = put32(p, 1);          /* animation header */
    p = put32(p, 1);          /* frame length */
    p = put32(p, 0x3F800000); /* speed 1.0f */
    p = put32(p, 0);          /* play offset */
    p = put32(p, 1);          /* frames */
    p = put32(p, 0);          /* frame: shape 0 */
    p = put32(p, 1);          /* keys */
    p = put32(p, 1);
    p = put32(p, 0);
    p = put32(p, 0);          /* key: time 0.0f */
    p = put32(p, 0x3F800000); /* value 1.0f */
    for (y = 0; y < 128; y++) /* texture: 16-bit, R in the low bits as in the PS1's format */
    {
        for (x = 0; x < 128; x++)
        {
            int sx = x / 8, sy = y / 8, idx = (pix[sy * 8 + sx / 2] >> ((sx & 1) * 4)) & 15;
            p = put16(p, (u16)(rd16(clut + idx * 2) | 0x8000));
        }
    }
    PortMc_SetIcon(s_Icon, (int)(p - s_Icon));
}

#ifdef SH_PORT_MC_SELFTEST
/* Test builds (SH1_MC_SELFTEST=1): when slot 1 is first loaded, the BIOS file functions are run as the
 * game uses them (create a one-block file, write it asynchronously, list the directory, read it back,
 * erase it), then the image is read back from the PS2 card and compared with the one in memory. */
extern int PortMc_Reload(int slot, u8* img);
extern int strcmp(const char* a, const char* b);
extern int memcmp(const void* a, const void* b, unsigned int n);

static void self_test(int slot)
{
    static u8        pattern[BLOCK_SIZE], back[BLOCK_SIZE], reread[CARD_SIZE] __attribute__((aligned(64)));
    static const char NAME[] = "bu00:BASLUS-00707TEST00";
    struct DIRENTRY  de;
    long             fd, n;
    int              i, listed = 0, ok = 1;
    if (slot != 0)
    {
        return;
    }
    for (i = 0; i < BLOCK_SIZE; i++)
    {
        pattern[i] = (u8)(i * 7 + 3);
    }
    fd = Port_BuOpen(NAME, (1 << 16) | FCREAT);
    printf("card self-test: create -> %ld\n", fd);
    ok &= fd >= 0;
    Port_BuClose(fd);
    fd = Port_BuOpen(NAME, FWRITE | FASYNC);
    n  = Port_BuLseek(fd, 0, 0) == 0 ? Port_BuWrite(fd, pattern, BLOCK_SIZE) : -1;
    printf("card self-test: write -> %ld\n", n);
    ok &= n == BLOCK_SIZE;
    Port_BuClose(fd);
    for (i = 0; i < 16; i++)
    {
        struct DIRENTRY* d = i == 0 ? Port_BuFirstFile("bu00:*", &de) : Port_BuNextFile(&de);
        if (!d)
        {
            break;
        }
        printf("card self-test: listed %s, %ld bytes\n", de.name, de.size);
        listed += strcmp(de.name, NAME + 5) == 0 && de.size == BLOCK_SIZE;
    }
    ok &= listed == 1;
    fd = Port_BuOpen(NAME, FREAD | FASYNC);
    n  = Port_BuRead(fd, back, BLOCK_SIZE);
    Port_BuClose(fd);
    ok &= n == BLOCK_SIZE && memcmp(back, pattern, BLOCK_SIZE) == 0;
    printf("card self-test: read back -> %ld, %s\n", n, memcmp(back, pattern, BLOCK_SIZE) ? "differs" : "same");
    i = PortMc_Reload(slot, reread);
    ok &= i == 1 && memcmp(reread, s_Card[slot].img, CARD_SIZE) == 0;
    printf("card self-test: card image re-read -> %d, %s\n", i,
           memcmp(reread, s_Card[slot].img, CARD_SIZE) ? "differs" : "same as in memory");
    n = Port_BuErase(NAME);
    ok &= n == 1 && Port_BuFirstFile("bu00:*", &de) == 0;
    printf("card self-test: erase -> %ld; %s\n", n, ok ? "PASSED" : "FAILED");
}
#endif
