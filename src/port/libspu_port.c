/** @brief Placeholder PS1 libspu for the port until sound is implemented (Step 6): every call
 * succeeds immediately and nothing is played. Return values follow PSY-Q's libspu so the game's
 * waits (e.g. for SPU transfers) complete: transfers are done at once, voices report off.
 */

#include "common.h"
#include "libspu.h"

void SpuInit(void) {}
void SpuQuit(void) {}
long SpuSetReverb(long on_off) { return on_off; }
long SpuSetReverbModeParam(SpuReverbAttr* attr) { (void)attr; return 0; }
long SpuReserveReverbWorkArea(long on_off) { return on_off; }
unsigned long SpuSetReverbVoice(long on_off, unsigned long voice_bit) { (void)on_off; return voice_bit; }
unsigned long SpuGetReverbVoice(void) { return 0; }
long SpuClearReverbWorkArea(long mode) { (void)mode; return 0; }
unsigned long SpuWrite(unsigned char* addr, unsigned long size) { (void)addr; return size; }
long SpuSetTransferMode(long mode) { return mode; }
unsigned long SpuSetTransferStartAddr(unsigned long addr) { return addr; }
long SpuIsTransferCompleted(long flag) { (void)flag; return 1; }
void SpuSetVoiceAttr(SpuVoiceAttr* arg) { (void)arg; }
void SpuGetVoiceAttr(SpuVoiceAttr* arg) { (void)arg; }
void SpuSetKey(long on_off, unsigned long voice_bit) { (void)on_off; (void)voice_bit; }
void SpuSetKeyOnWithAttr(SpuVoiceAttr* attr) { (void)attr; }
long SpuGetKeyStatus(unsigned long voice_bit) { (void)voice_bit; return 0; } /* SPU_OFF */
void SpuSetCommonAttr(SpuCommonAttr* attr) { (void)attr; }
long SpuInitMalloc(long num, char* top) { (void)top; return num; }
