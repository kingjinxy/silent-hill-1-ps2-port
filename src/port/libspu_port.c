/** @brief Placeholder PS1 libspu for the port until sound is implemented (Step 6): every call
 * succeeds immediately and nothing is played. Return values follow PSY-Q's libspu so the game's
 * waits (e.g. for SPU transfers) complete: transfers are done at once, and voices report the key
 * state last set (SPU_ON after a key on, SPU_OFF after a key off; -1 for a mask with no voice, as
 * libspu does). The sound driver waits for both after keying a voice on or off.
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
static unsigned long s_KeyOn; /* voices keyed on (bit per voice) */

void SpuSetKey(long on_off, unsigned long voice_bit)
{
    if (on_off)
    {
        s_KeyOn |= voice_bit;
    }
    else
    {
        s_KeyOn &= ~voice_bit;
    }
}

void SpuSetKeyOnWithAttr(SpuVoiceAttr* attr)
{
    s_KeyOn |= attr->voice;
}

long SpuGetKeyStatus(unsigned long voice_bit)
{
    voice_bit &= 0xFFFFFF;
    if (voice_bit == 0)
    {
        return -1;
    }
    return (s_KeyOn & voice_bit) ? SPU_ON : SPU_OFF;
}
void SpuSetCommonAttr(SpuCommonAttr* attr) { (void)attr; }
long SpuInitMalloc(long num, char* top) { (void)top; return num; }
