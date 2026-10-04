#ifndef _PSX_MEM_H
#define _PSX_MEM_H

/** @brief PS1 fixed memory map abstraction.
 *
 * The game places many buffers at hardcoded RAM addresses (file load buffers, ordering tables,
 * VAB/KDT buffers, etc.) and uses the 1 KiB scratchpad at `0x1F800000` directly. Every such address
 * must go through the macros below rather than being written as a raw literal.
 *
 * - Matching (PS1) build: the macros expand to the original literal, so codegen is unchanged.
 * - Port build (`SH_PORT`): addresses are redirected into static arenas that mirror the PS1 layout,
 *   so the game's memory map is preserved while the program itself can live anywhere.
 */

#define PSX_RAM_SIZE     0x200000 /** 2 MiB main RAM. */
#define PSX_SCRATCH_SIZE 0x400    /** 1 KiB scratchpad (D-cache). */

#ifdef SH_PORT

extern u8 g_PsxRam[PSX_RAM_SIZE];
extern u8 g_PsxScratch[PSX_SCRATCH_SIZE];

/** @brief Converts a PS1 RAM address (any mirror: KUSEG, KSEG0, KSEG1) to a host pointer into the RAM arena. */
#define PSX_RAM_ADDR(addr) \
    ((void*)&g_PsxRam[(u32)(addr) & (PSX_RAM_SIZE - 1)])

#define PSX_SCRATCH ((void*)g_PsxScratch)

/** @brief Canonical integer form of a pointer, used when comparing buffer ranges.
 * There are no address mirrors on the host, so the full pointer is used.
 */
#define PSX_ADDR_CANON(ptr) \
    ((u32)(ptr))

#else

#define PSX_RAM_ADDR(addr) \
    ((void*)(addr))

#define PSX_SCRATCH ((void*)0x1F800000)

/** @brief Canonical integer form of a pointer, used when comparing buffer ranges.
 * Strips the segment bits so that KUSEG/KSEG0/KSEG1 mirrors of the same RAM compare equal.
 */
#define PSX_ADDR_CANON(ptr) \
    ((u32)(ptr) & 0xFFFFFF)

#endif

#define PSX_SCRATCH_ADDR(offset) ((void*)(((u8*)PSX_SCRATCH) + (offset)))

#endif
