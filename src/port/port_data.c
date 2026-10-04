/** @brief Port-only definitions of game variables the PS1 build only gets through linker addresses.
 *
 * These live in bss gaps the generated PS1 linker scripts reserve without a symbol. The matching
 * build defines them in `configs/USA/relative_syms.ld` relative to a neighbouring symbol; where that
 * neighbour only exists under the PS1 compiler's naming (e.g. numbered function-local statics), the
 * port defines the variable here instead. Only valid for variables that don't overlap another one.
 */

#include "common.h"
#include "bodyprog/bodyprog.h"

/** Anchored on `screenPosY.53` (a GCC 2.8 static-local name) in `relative_syms.ld`. Used only by
 * bodyprog_mapscreen_80066D90.c; occupies its own bss gap. */
q3_12 D_800C4454;
