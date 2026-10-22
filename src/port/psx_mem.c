#include "common.h"
#include "port/spr.h"

// Port-only: backing storage for the PS1 fixed memory map (see `include/decomp/psx_mem.h`).
// Not part of the matching PS1 build; compiled only by the port build (`SH_PORT`).

#ifdef SH_PORT

u8 g_PsxRam[PSX_RAM_SIZE] __attribute__((aligned(64)));
/* The PS1 scratchpad lives in the EE's (include/port/spr.h). Nothing DMAs to or from it: the PS1 couldn't either. */
SPR u8 g_PsxScratch[PSX_SCRATCH_SIZE] __attribute__((aligned(64)));

#endif
