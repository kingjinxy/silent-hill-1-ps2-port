#ifndef _PORT_H
#define _PORT_H

/** @brief Portability helpers for the PS2 port build (`SH_PORT`).
 * Every macro expands to exactly the original code in the matching (PS1) build.
 */

/** @brief `static` in the matching build, external linkage in the port.
 * For definitions that are `static` but declared `extern` in a header (old GCC accepted that; modern
 * GCC rejects it) and for block-scope `static` function declarations. On PS1 other binaries reach
 * these through absolute addresses; once everything is linked together they need real symbols.
 */
#ifdef SH_PORT
#define MATCH_STATIC
#else
#define MATCH_STATIC static
#endif

/** @brief `const` in the matching build, mutable in the port.
 * For data the game declares `const` but writes at runtime (PS1 RAM doesn't enforce read-only data),
 * e.g. the map overlay header. Modern GCC both rejects the writes and may fold reads of a `const`
 * object to its initial value, so the port drops the qualifier.
 */
#ifdef SH_PORT
#define MATCH_CONST
#else
#define MATCH_CONST const
#endif

#ifdef SH_PORT
/** @brief Called for every file read the game starts (`Fs_QueueStartRead`). For overlay files
 * (maps, screens, BODYPROG, B_KONAMI), makes the linked-in overlay current. src/port/overlay.c. */
void Port_OverlayActivate(s32 fileIdx);
#endif

#endif
