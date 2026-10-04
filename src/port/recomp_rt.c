/** @brief Runtime support for recompiled code (see include/port/recomp.h). */

#include "port/recomp.h"

extern int printf(const char* fmt, ...);

void Rc_BadJump(unsigned int target)
{
    printf("recomp: jump to unknown code address %08X\n", target);
    for (;;)
    {
    }
}
