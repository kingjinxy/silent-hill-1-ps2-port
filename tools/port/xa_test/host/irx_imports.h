/* Host stand-ins for the IOP headers, so tools/port/xa_test/xa_host.c can build sh1spu's xa.c natively. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef int16_t s16; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;
typedef struct { int attr, option, initial, max; } iop_sema_t;
typedef struct { int attr, option; void (*thread)(void*); int priority, stacksize; } iop_thread_t;
typedef struct { u8 trycount, spindlctrl, datapattern, pad; } sceCdRMode;
#define TH_C 0
#define SCECdSpinNom 1
#define SCECdSecS2048 0
#define SD_TRANS_WRITE 0
#define SD_TRANS_LOOP 0x10
#define SD_TRANS_STOP 2
static int  CreateSema(iop_sema_t* s) { (void)s; return 1; }
static int  SignalSema(int s) { (void)s; return 0; }
static int  iSignalSema(int s) { (void)s; return 0; }
static int  WaitSema(int s) { (void)s; return 0; }
static int  CreateThread(iop_thread_t* t) { (void)t; return 1; }
static int  StartThread(int t, void* a) { (void)t; (void)a; return 0; }
static int  DelayThread(int us) { (void)us; return 0; }
static int  sceCdRead(u32 l, u32 n, void* b, sceCdRMode* m) { (void)l; (void)n; (void)b; (void)m; return 0; }
static int  sceCdSync(int m) { (void)m; return 0; }
static void* sceSdSetTransIntrHandler(int c, int (*f)(int, void*), void* a) { (void)c; (void)f; (void)a; return 0; }
static int  sceSdBlockTrans(int c, int m, void* b, u32 s) { (void)c; (void)m; (void)b; (void)s; return 0; }
typedef struct { u32 lo, hi; } iop_sys_clock_t;
static void GetSystemTime(iop_sys_clock_t* c) { c->lo = c->hi = 0; }
static void SysClock2USec(iop_sys_clock_t* c, u32* s, u32* u) { (void)c; *s = *u = 0; }
