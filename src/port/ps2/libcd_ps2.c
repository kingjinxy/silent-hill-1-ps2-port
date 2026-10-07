/** @brief PS1 libcd (the parts the file queue uses) on the PS2's CD/DVD drive.
 *
 * The game addresses the PS1 disc by sector. On that disc SILENT. starts at sector 64 and HILL.
 * right after it; on the port's DVD both are ordinary files (tools/port/make_iso.sh). SILENT. holds
 * 2048-byte sectors, so a PS1 sector maps straight onto a DVD sector. HILL. holds raw 2336-byte PS1
 * sectors (8-byte subheader + data, as dumpsxiso extracted it), so a data read takes the 2048 bytes
 * after each subheader.
 *
 * Reads are asynchronous, as on the PS1: CdRead starts a request and returns; CdReadSync(1) (which
 * the file queue and the sound loader poll) reports the sectors still to come and moves the request
 * along: when the DVD chunk in flight is done, its data is copied out and the next chunk is started.
 * CdReadSync(0) waits for the whole request. A request is read in chunks of up to BOUNCE_SECTORS DVD
 * sectors: for HILL., a run of raw PS1 sectors is one contiguous byte range, so ~27 PS1 sectors come
 * in one DVD read (then each sector's 2048 data bytes are copied out past its subheader).
 *
 * Compiled with ps2sdk headers (not the game's), so the PSY-Q types are declared locally.
 */

#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <libcdvd.h>

/* libcdvd-common.h maps some old names onto its own functions; these are the PSY-Q ones. */
#undef CdInit
#undef CdRead
#undef CdSync
#undef CdPosToInt
#undef CdIntToPos

typedef struct
{
    unsigned char minute; /* BCD */
    unsigned char second; /* BCD */
    unsigned char sector; /* BCD */
    unsigned char track;
} CdlLOC;

/* PSY-Q libcd constants (include/psyq/libcd.h). */
#define CdlSetloc    0x02
#define CdlStop      0x08
#define CdlPause     0x09
#define CdlInit      0x0A
#define CdlComplete  0x02
#define CdlDiskError 0x05

#define PS1_SILENT_SECTOR 64     /* SILENT. on the PS1 disc (filetable sector numbers are absolute). */
#define PS1_RAW_SECTOR    2336   /* HILL. sector size: 8-byte subheader + 2328. */
#define PS1_SUBHEADER     8
#define DVD_SECTOR        2048

static sceCdlFILE s_Silent;
static sceCdlFILE s_Hill;
static unsigned int s_SilentSectors;
static unsigned int s_HillPs1Start;
static int s_Ready;
static int s_Pos; /* PS1 sector set by CdlSetloc. */

/* Bounce buffer for DVD reads: the EE side of libcdvd DMAs into it. */
#define BOUNCE_SECTORS 32
static unsigned char s_Bounce[BOUNCE_SECTORS * DVD_SECTOR] __attribute__((aligned(64)));

/* The read request in progress (CdRead -> CdReadSync). */
static struct
{
    int            active;  /* a request is in progress */
    int            error;   /* it failed: CdReadSync reports -1 until the next CdRead */
    int            pos;     /* next PS1 sector to deliver */
    int            left;    /* PS1 sectors still to deliver */
    unsigned char* dst;
    /* The DVD chunk in flight: PS1 sectors it covers, and where the first one's data starts. */
    int            inFlight;
    int            chunkSectors;
    unsigned int   chunkSkip;
    int            hill;
} s_Req;

/** Starts a non-blocking DVD read; 0 if the drive refused it. */
static int dvd_start(unsigned int lsn, unsigned int count)
{
    sceCdRMode mode;
    mode.trycount    = 0;
    mode.spindlctrl  = SCECdSpinNom;
    mode.datapattern = SCECdSecS2048;
    mode.pad         = 0;
    return sceCdRead(lsn, count, s_Bounce, &mode);
}

/** Starts the next chunk of the request; 0 on failure (the request is then in error). */
static int chunk_start(void)
{
    int pos = s_Req.pos;
    if (pos >= PS1_SILENT_SECTOR && pos < (int)s_HillPs1Start)
    {
        /* SILENT.: 2048-byte sectors map straight onto DVD sectors. */
        unsigned int n = s_Req.left > BOUNCE_SECTORS ? BOUNCE_SECTORS : (unsigned int)s_Req.left;
        if ((unsigned int)(pos - PS1_SILENT_SECTOR) + n > s_SilentSectors)
        {
            n = s_SilentSectors - (unsigned int)(pos - PS1_SILENT_SECTOR);
        }
        if (n == 0 || !dvd_start(s_Silent.lsn + (unsigned int)(pos - PS1_SILENT_SECTOR), n))
        {
            return 0;
        }
        s_Req.hill         = 0;
        s_Req.chunkSectors = (int)n;
        s_Req.chunkSkip    = 0;
    }
    else if (pos >= (int)s_HillPs1Start)
    {
        /* HILL.: raw 2336-byte sectors, as many whole ones as fit the bounce buffer. */
        unsigned long long off  = (unsigned long long)(pos - s_HillPs1Start) * PS1_RAW_SECTOR;
        unsigned int       first = (unsigned int)(off / DVD_SECTOR);
        unsigned int       skip  = (unsigned int)(off % DVD_SECTOR);
        unsigned int       n     = (BOUNCE_SECTORS * DVD_SECTOR - skip) / PS1_RAW_SECTOR;
        unsigned int       count;
        if (n > (unsigned int)s_Req.left)
        {
            n = (unsigned int)s_Req.left;
        }
        count = (skip + n * PS1_RAW_SECTOR + DVD_SECTOR - 1) / DVD_SECTOR;
        if (!dvd_start(s_Hill.lsn + first, count))
        {
            return 0;
        }
        s_Req.hill         = 1;
        s_Req.chunkSectors = (int)n;
        s_Req.chunkSkip    = skip;
    }
    else
    {
        printf("libcd: read of PS1 sector %d (outside SILENT./HILL.)\n", pos);
        return 0;
    }
    s_Req.inFlight = 1;
    return 1;
}

/** Copies the finished chunk out of the bounce buffer. */
static void chunk_finish(void)
{
    int i;
    if (s_Req.hill)
    {
        for (i = 0; i < s_Req.chunkSectors; i++)
        {
            memcpy(s_Req.dst, s_Bounce + s_Req.chunkSkip + i * PS1_RAW_SECTOR + PS1_SUBHEADER, DVD_SECTOR);
            s_Req.dst += DVD_SECTOR;
        }
    }
    else
    {
        memcpy(s_Req.dst, s_Bounce, (unsigned int)s_Req.chunkSectors * DVD_SECTOR);
        s_Req.dst += s_Req.chunkSectors * DVD_SECTOR;
    }
    s_Req.pos += s_Req.chunkSectors;
    s_Req.left -= s_Req.chunkSectors;
    s_Req.inFlight = 0;
}

/** Moves the request along; `wait`: until it is complete. */
static void advance(int wait)
{
    while (s_Req.active)
    {
        if (s_Req.inFlight)
        {
            if (sceCdSync(wait ? 0 : 1))
            {
                return; /* still reading */
            }
            if (sceCdGetError() != SCECdErNO)
            {
                printf("libcd: drive error %d at PS1 sector %d\n", sceCdGetError(), s_Req.pos);
                s_Req.inFlight = 0;
                s_Req.active   = 0;
                s_Req.error    = 1;
                return;
            }
            chunk_finish();
        }
        if (s_Req.left <= 0)
        {
#ifdef SH_PORT_TRACE_CD
            printf("libcd: read done at PS1 sector %d\n", s_Req.pos);
#endif
            s_Req.active = 0;
            s_Pos        = s_Req.pos;
            return;
        }
        if (!chunk_start())
        {
            printf("libcd: read failed to start at PS1 sector %d (%d left)\n", s_Req.pos, s_Req.left);
            s_Req.active = 0;
            s_Req.error  = 1;
            return;
        }
        if (!wait)
        {
            return;
        }
    }
}

int CdInit(void)
{
    if (s_Ready)
    {
        return 1;
    }
    sceCdInit(SCECdINIT);
    sceCdMmode(SCECdMmodeDvd);
    if (!sceCdSearchFile(&s_Silent, "\\SILENT.;1") || !sceCdSearchFile(&s_Hill, "\\HILL.;1"))
    {
        printf("libcd: SILENT./HILL. not found on the disc\n");
        return 0;
    }
    s_SilentSectors = s_Silent.size / DVD_SECTOR;
    s_HillPs1Start  = PS1_SILENT_SECTOR + s_SilentSectors;
    s_Ready         = 1;
    printf("libcd: SILENT. at DVD sector %u (%u sectors), HILL. at %u (PS1 sector %u)\n",
           s_Silent.lsn, s_SilentSectors, s_Hill.lsn, s_HillPs1Start);
    return 1;
}

int CdReset(int mode)
{
    (void)mode;
    return CdInit();
}

static int bcd(int v)
{
    return ((v / 10) << 4) | (v % 10);
}

static int unbcd(int v)
{
    return ((v >> 4) * 10) + (v & 0xF);
}

CdlLOC* CdIntToPos(int i, CdlLOC* p)
{
    i += 150; /* 2-second lead-in */
    p->minute = (unsigned char)bcd(i / (60 * 75));
    p->second = (unsigned char)bcd((i / 75) % 60);
    p->sector = (unsigned char)bcd(i % 75);
    return p;
}

int CdPosToInt(CdlLOC* p)
{
    return ((unbcd(p->minute) * 60 + unbcd(p->second)) * 75 + unbcd(p->sector)) - 150;
}

int CdControl(unsigned char com, unsigned char* param, unsigned char* result)
{
    (void)result;
    if ((com == CdlStop || com == CdlPause || com == CdlInit) && s_Req.active)
    {
        /* These stop a read in progress, which then reports an error (the file queue resets and
         * retries); status polls (CdlNop, which the sound code sends every frame), seeks and mode
         * changes leave it running. */
        if (s_Req.inFlight)
        {
            sceCdSync(0);
        }
        s_Req.active   = 0;
        s_Req.inFlight = 0;
        s_Req.error    = 1;
        printf("libcd: command %02X stopped a read at PS1 sector %d\n", com, s_Req.pos);
    }
    if (com == CdlSetloc && param)
    {
        s_Pos = CdPosToInt((CdlLOC*)param);
    }
    return 1; /* Seeks etc. complete immediately. */
}

int CdControlB(unsigned char com, unsigned char* param, unsigned char* result)
{
    return CdControl(com, param, result);
}

int CdSync(int mode, unsigned char* result)
{
    (void)mode;
    (void)result;
    return CdlComplete;
}

int CdRead(int sectors, unsigned int* buf, int mode)
{
    (void)mode;
    if (!s_Ready && !CdInit())
    {
        return 0;
    }
    if (s_Req.active)
    {
        advance(1); /* a new request replaces one still running: let the drive finish first */
    }
    s_Req.error = 0;
    if (sectors <= 0)
    {
        s_Req.active = 0; /* nothing to read (empty files): done at once */
        return 1;
    }
    s_Req.active   = 1;
    s_Req.pos      = s_Pos;
#ifdef SH_PORT_TRACE_CD
    printf("libcd: read PS1 sector %d, %d sectors\n", s_Pos, sectors);
#endif
    s_Req.left     = sectors;
    s_Req.dst      = (unsigned char*)buf;
    s_Req.inFlight = 0;
    if (!chunk_start())
    {
        s_Req.active = 0;
        return 0;
    }
    return 1;
}

int CdReadSync(int mode, unsigned char* result)
{
    (void)result;
    advance(mode == 0);
    if (s_Req.error)
    {
        return -1;
    }
    return s_Req.active ? s_Req.left : 0;
}
