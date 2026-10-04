/** @brief PS1 libcd (the parts the file queue uses) on the PS2's CD/DVD drive.
 *
 * The game addresses the PS1 disc by sector. On that disc SILENT. starts at sector 64 and HILL.
 * right after it; on the port's DVD both are ordinary files (tools/port/make_iso.sh). SILENT. holds
 * 2048-byte sectors, so a PS1 sector maps straight onto a DVD sector. HILL. holds raw 2336-byte PS1
 * sectors (8-byte subheader + data, as dumpsxiso extracted it), so a data read takes the 2048 bytes
 * after each subheader.
 *
 * Reads are synchronous for now: CdRead completes before returning, so CdReadSync reports done.
 * The file queue's state machine is unchanged.
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
static unsigned char s_Bounce[16 * DVD_SECTOR] __attribute__((aligned(64)));

static int dvd_read(unsigned int lsn, unsigned int count, void* dst)
{
    sceCdRMode mode;
    mode.trycount    = 0;
    mode.spindlctrl  = SCECdSpinNom;
    mode.datapattern = SCECdSecS2048;
    mode.pad         = 0;
    if (!sceCdRead(lsn, count, dst, &mode))
    {
        return 0;
    }
    sceCdSync(0);
    return sceCdGetError() == SCECdErNO;
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
    unsigned char* dst = (unsigned char*)buf;
    int pos = s_Pos;
    (void)mode;

    if (!s_Ready && !CdInit())
    {
        return 0;
    }
    while (sectors > 0)
    {
        if (pos >= PS1_SILENT_SECTOR && pos < (int)s_HillPs1Start)
        {
            /* SILENT.: 2048-byte sectors, straight through the bounce buffer. */
            unsigned int n = sectors > 16 ? 16 : (unsigned int)sectors;
            if ((unsigned int)(pos - PS1_SILENT_SECTOR) + n > s_SilentSectors)
            {
                n = s_SilentSectors - (unsigned int)(pos - PS1_SILENT_SECTOR);
            }
            if (!dvd_read(s_Silent.lsn + (unsigned int)(pos - PS1_SILENT_SECTOR), n, s_Bounce))
            {
                return 0;
            }
            memcpy(dst, s_Bounce, n * DVD_SECTOR);
            dst += n * DVD_SECTOR;
            pos += n;
            sectors -= n;
        }
        else if (pos >= (int)s_HillPs1Start)
        {
            /* HILL.: raw 2336-byte sectors; the data starts after the subheader. */
            unsigned long long off = (unsigned long long)(pos - s_HillPs1Start) * PS1_RAW_SECTOR + PS1_SUBHEADER;
            unsigned int first = (unsigned int)(off / DVD_SECTOR);
            unsigned int skip  = (unsigned int)(off % DVD_SECTOR);
            unsigned int count = (skip + DVD_SECTOR + DVD_SECTOR - 1) / DVD_SECTOR;
            if (!dvd_read(s_Hill.lsn + first, count, s_Bounce))
            {
                return 0;
            }
            memcpy(dst, s_Bounce + skip, DVD_SECTOR);
            dst += DVD_SECTOR;
            pos++;
            sectors--;
        }
        else
        {
            printf("libcd: read of PS1 sector %d (outside SILENT./HILL.)\n", pos);
            return 0;
        }
    }
    s_Pos = pos;
    return 1;
}

int CdReadSync(int mode, unsigned char* result)
{
    (void)mode;
    (void)result;
    return 0; /* No sectors left: reads are synchronous. */
}
