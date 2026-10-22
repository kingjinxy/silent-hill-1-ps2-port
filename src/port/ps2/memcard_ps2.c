/** @brief The PS2 memory cards behind the port's PS1 cards (src/port/psx_card.c).
 *
 * rom0:MCMAN and rom0:MCSERV (the versions that go with the rom0:SIO2MAN/PADMAN that pad_ps2.c
 * loads) through ps2sdk's libmc. PS1 slot 1 is the PS2 card in port 1, slot 2 the one in port 2.
 * Each holds the PS1 card image in one save folder, with icon.sys and an icon so the PS2 browser
 * shows it as Silent Hill's save:
 *     mcN:/BASLUS-00707SH1/  icon.sys  SH1.ICO  CARD.MCD (128 KB, the PS1 card's raw image)
 * The folder is created when the game first writes to the card.
 *
 * Card queries (mcGetInfo) are asynchronous: the game asks about both slots continually, and a
 * query goes to the card. Loads and stores block (they happen on the save and load screens). One
 * libmc call runs at a time; a blocking call first finishes a query in flight. */

#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libmc.h>
#include <stdio.h>
#include <string.h>

#define SAVE_DIR  "/BASLUS-00707SH1"
#define CARD_FILE SAVE_DIR "/CARD.MCD"
#define ICON_FILE "SH1.ICO"
#define CARD_SIZE 0x20000

/* mcOpen modes: the IOP's values (io_common.h's MC_O_*, which newlib builds can't include), not newlib's O_*. */
#define MC_O_RDONLY 0x0001
#define MC_O_WRONLY 0x0002
#define MC_O_CREAT  0x0200

extern void Port_PadModulesLoad(void); /* pad_ps2.c: rom0:SIO2MAN (MCMAN needs it) and PADMAN */

static int s_Ready;            /* 1 libmc ready, -1 unavailable */
static int s_Busy = -1;        /* port of the query in flight, -1 none */
static int s_Want[2];          /* a query waits for the one in flight to end */
static int s_Done[2];          /* a query ended (found while finishing it for a blocking call) */
static int s_DoneState[2];
static int s_Formatted[2];     /* the card in the port is a formatted PS2 card */
static int s_InfoType, s_InfoFree, s_InfoFormat;
static int s_DirMade[2];       /* the save folder exists on this card */

static int                  s_MainThread = -1; /* libmc is used from the game's main thread only */
static const unsigned char* s_Icon;
static int                  s_IconLength;

static int mc_wait(void);
static int create(int port, const unsigned char* img);

static int init(void)
{
    if (s_Ready)
    {
        return s_Ready > 0;
    }
    Port_PadModulesLoad();
    if (SifLoadModule("rom0:MCMAN", 0, NULL) < 0 || SifLoadModule("rom0:MCSERV", 0, NULL) < 0 || mcInit(MC_TYPE_MC) < 0)
    {
        printf("memcard: can't start the memory card modules (rom0:MCMAN/MCSERV): saving unavailable\n");
        s_Ready = -1;
        return 0;
    }
    printf("memcard: ready\n");
    s_Ready = 1;
#ifdef SH_PORT_MC_FORMAT
    {
        /* Test builds (SH1_MC_FORMAT=1): format an unformatted card in port 1 (PCSX2's fresh test cards). */
        int type, freeClusters, formatted, r;
        mcGetInfo(0, 0, &type, &freeClusters, &formatted);
        r = mc_wait();
        if (r == sceMcResNoFormat || (r >= -1 && type == sceMcTypePS2 && !formatted))
        {
            mcFormat(0, 0);
            printf("memcard: test build: formatted the card in port 1 (%d)\n", mc_wait());
        }
    }
#endif
    return 1;
}

static int mc_wait(void)
{
    int cmd, result;
    mcSync(MC_WAIT, &cmd, &result);
    return result;
}

/** The state from a finished mcGetInfo: 0 same card, 1 another (formatted) card, -1 none/unusable. */
static int info_state(int port, int result)
{
    if (result == 0 || result == -1)
    {
        if (result == -1 || s_InfoType != sceMcTypePS2)
        {
            s_DirMade[port] = 0;
        }
        s_Formatted[port] = s_InfoType == sceMcTypePS2 && (result == -1 || s_Formatted[port] || s_InfoFormat);
        if (!s_Formatted[port])
        {
            return -1;
        }
        return result == -1 ? 1 : 0;
    }
    s_Formatted[port] = 0;
    s_DirMade[port]   = 0;
    return -1; /* -2 unformatted card, < -2 no card or an error (a PS1 card answers that way too) */
}

static void start(int port)
{
    s_Want[port] = 0;
    s_Busy       = port;
    if (mcGetInfo(port, 0, &s_InfoType, &s_InfoFree, &s_InfoFormat) < 0)
    {
        s_Busy            = -1;
        s_Done[port]      = 1;
        s_DoneState[port] = -1;
    }
}

/** Finishes the query in flight (before a blocking call), keeping its result for PortMc_InfoPoll. */
static void finish(void)
{
    if (s_Busy >= 0)
    {
        int port          = s_Busy;
        s_DoneState[port] = info_state(port, mc_wait());
        s_Done[port]      = 1;
        s_Busy            = -1;
    }
}

void PortMc_InfoStart(int port)
{
    if (!init())
    {
        s_Done[port]      = 1;
        s_DoneState[port] = -1;
        return;
    }
    if (s_Busy < 0)
    {
        start(port);
    }
    else
    {
        s_Want[port] = 1;
    }
}

int PortMc_InfoPoll(int port, int* state)
{
    if (GetThreadId() != s_MainThread)
    {
        return 0; /* TestEvent from another thread: the main thread's next call delivers it */
    }
    if (s_Done[port])
    {
        s_Done[port] = 0;
        *state       = s_DoneState[port];
        return 1;
    }
    if (s_Busy == port)
    {
        int cmd, result;
        if (mcSync(MC_NOWAIT, &cmd, &result) == 0)
        {
            return 0; /* still running */
        }
        s_Busy = -1;
        *state = info_state(port, result);
        if (s_Want[port ^ 1])
        {
            start(port ^ 1);
        }
        return 1;
    }
    if (s_Busy < 0 && s_Want[port])
    {
        start(port);
    }
    return 0;
}

int PortMc_LoadImage(int port, unsigned char* img)
{
    int fd, n;
    if (!init())
    {
        return -1;
    }
    finish();
    mcOpen(port, 0, CARD_FILE, MC_O_RDONLY);
    fd = mc_wait();
    if (fd < 0)
    {
        printf("memcard: port %d: no save folder yet (%d)\n", port + 1, fd);
#ifdef SH_PORT_MC_IMPORT
        if (fd == sceMcResNoEntry)
        {
            /* Import builds (SH1_MC_IMPORT=1): a PS1 card image at host:importN.mcd becomes the new save. */
            char  name[32];
            FILE* f;
            sprintf(name, "host:import%d.mcd", port + 1);
            f = fopen(name, "rb");
            if (f)
            {
                n = (int)fread(img, 1, CARD_SIZE, f);
                fclose(f);
                if (n == CARD_SIZE && img[0] == 'M' && img[1] == 'C' && create(port, img) == 0)
                {
                    printf("memcard: port %d: imported %s\n", port + 1, name);
                    return 1;
                }
                printf("memcard: port %d: importing %s failed\n", port + 1, name);
            }
        }
#endif
        return fd == sceMcResNoEntry ? 0 : -1;
    }
    mcRead(fd, img, CARD_SIZE);
    n = mc_wait();
    mcClose(fd);
    mc_wait();
    if (n != CARD_SIZE)
    {
        printf("memcard: port %d: reading the PS1 card image failed (%d)\n", port + 1, n);
        return -1;
    }
    s_DirMade[port] = 1;
    printf("memcard: port %d: PS1 card image loaded\n", port + 1);
    return 1;
}

/* Kanji-mode (full-width) Shift-JIS for the browser's title: letters, digits and space. */
static void sjis(unsigned short* out, const char* s)
{
    for (; *s; s++, out++)
    {
        unsigned short c = (unsigned char)*s;
        unsigned short v = c == ' ' ? 0x8140 : (c >= '0' && c <= '9') ? 0x824F + (c - '0') :
                           (c >= 'A' && c <= 'Z') ? 0x8260 + (c - 'A') : (c >= 'a' && c <= 'z') ? 0x8281 + (c - 'a') :
                           c == '-' ? 0x817C : 0x8140;
        *out = (unsigned short)((v >> 8) | (v << 8)); /* stored big-endian */
    }
}

static int write_file(int port, const char* name, const void* data, int length)
{
    int fd, n;
    mcOpen(port, 0, name, MC_O_WRONLY | MC_O_CREAT);
    fd = mc_wait();
    if (fd < 0)
    {
        return fd;
    }
    mcWrite(fd, data, length);
    n = mc_wait();
    mcClose(fd);
    mc_wait();
    return n == length ? 0 : -1;
}

/** Creates the save folder with its icon files and the whole card image. */
static int create(int port, const unsigned char* img)
{
    static mcIcon            iconSys __attribute__((aligned(64)));
    static const iconIVECTOR BG[4]       = { { 40, 40, 48, 0 }, { 40, 40, 48, 0 }, { 8, 8, 10, 0 }, { 8, 8, 10, 0 } };
    static const iconFVECTOR LIGHT_DIR[3] = { { 0.5, 0.5, 0.5, 0.0 }, { 0.0, -0.4, -0.1, 0.0 }, { -0.5, -0.5, 0.5, 0.0 } };
    static const iconFVECTOR LIGHT_COL[3] = { { 0.3, 0.3, 0.3, 0.0 }, { 0.4, 0.4, 0.4, 0.0 }, { 0.5, 0.5, 0.5, 0.0 } };
    static const iconFVECTOR AMBIENT      = { 0.5, 0.5, 0.5, 0.0 };
    int                      r;

    mcMkDir(port, 0, SAVE_DIR);
    r = mc_wait();
    if (r < 0 && r != sceMcResNoEntry) /* -4 here: the folder exists already */
    {
        printf("memcard: port %d: can't create %s (%d)\n", port + 1, SAVE_DIR, r);
    }
    memset(&iconSys, 0, sizeof(iconSys));
    memcpy(iconSys.head, "PS2D", 4);
    sjis(iconSys.title, "SILENT HILLPS1 saves");
    iconSys.nlOffset = 11 * 2; /* second line at "PS1 saves" */
    iconSys.trans    = 0x60;
    memcpy(iconSys.bgCol, BG, sizeof(BG));
    memcpy(iconSys.lightDir, LIGHT_DIR, sizeof(LIGHT_DIR));
    memcpy(iconSys.lightCol, LIGHT_COL, sizeof(LIGHT_COL));
    memcpy(iconSys.lightAmbient, AMBIENT, sizeof(AMBIENT));
    strcpy((char*)iconSys.view, ICON_FILE);
    strcpy((char*)iconSys.copy, ICON_FILE);
    strcpy((char*)iconSys.del, ICON_FILE);
    if (write_file(port, SAVE_DIR "/icon.sys", &iconSys, sizeof(iconSys)) < 0 ||
        (s_Icon && write_file(port, SAVE_DIR "/" ICON_FILE, s_Icon, s_IconLength) < 0) ||
        write_file(port, CARD_FILE, img, CARD_SIZE) < 0)
    {
        printf("memcard: port %d: creating the save failed\n", port + 1);
        return -1;
    }
    s_DirMade[port] = 1;
    printf("memcard: port %d: save folder created\n", port + 1);
    return 0;
}

#ifdef SH_PORT_MC_FORMAT
/** Test builds: a copy of the PS1 card image next to the disc image (host:cardN.mcd, an emulator
 * memory card file), to load the save in the original game in DuckStation. */
static void host_mirror(int port, const unsigned char* img)
{
    char  name[32];
    FILE* f;
    sprintf(name, "host:card%d.mcd", port + 1);
    f = fopen(name, "wb");
    if (f)
    {
        fwrite(img, 1, CARD_SIZE, f);
        fclose(f);
    }
}
#endif

static int store(int port, const unsigned char* img, int offset, int length);

int PortMc_StoreImage(int port, const unsigned char* img, int offset, int length)
{
    int r = store(port, img, offset, length);
#ifdef SH_PORT_MC_FORMAT
    if (r == 0)
    {
        host_mirror(port, img);
    }
#endif
    return r;
}

/** Writes the image's bytes [offset, offset + length) to the card (the whole image when it creates the save). */
static int store(int port, const unsigned char* img, int offset, int length)
{
    int fd, n;
    if (!init())
    {
        return -1;
    }
    finish();
    if (!s_DirMade[port])
    {
        mcOpen(port, 0, CARD_FILE, MC_O_RDONLY);
        fd = mc_wait();
        if (fd >= 0)
        {
            mcClose(fd);
            mc_wait();
            s_DirMade[port] = 1;
        }
        else
        {
            return create(port, img); /* writes the whole image, this change included */
        }
    }
    mcOpen(port, 0, CARD_FILE, MC_O_WRONLY);
    fd = mc_wait();
    if (fd < 0)
    {
        printf("memcard: port %d: can't open the PS1 card image (%d)\n", port + 1, fd);
        return -1;
    }
    mcSeek(fd, offset, SEEK_SET);
    mc_wait();
    mcWrite(fd, img + offset, length);
    n = mc_wait();
    mcClose(fd);
    mc_wait();
    if (n != length)
    {
        printf("memcard: port %d: write of %d bytes at %d failed (%d)\n", port + 1, length, offset, n);
        return -1;
    }
    return 0;
}

/** Called once from InitCARD (the game's main thread). */
void PortMc_SetIcon(const unsigned char* ico, int length)
{
    s_MainThread = GetThreadId();
    s_Icon       = ico;
    s_IconLength = length;
}

#ifdef SH_PORT_MC_FORMAT
/** Test builds: re-reads the card image from the card into `img` (psx_card.c's self-test). */
int PortMc_Reload(int port, unsigned char* img)
{
    return PortMc_LoadImage(port, img);
}
#endif
