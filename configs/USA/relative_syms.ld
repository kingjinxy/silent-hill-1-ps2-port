/* Symbols the game references that sit inside another object or in a gap the generated linker
 * scripts only reserve (`. += N`). Defined relative to the preceding real symbol so they move with
 * it instead of staying at their PS1 address. PROVIDE: only applies where the name is used.
 * Regenerate: tools/port/prune_undefined_syms.py relativize (see its docstring). */
PROVIDE("D_800C15B4" = "D_800C15B0" + 0x4);
PROVIDE("D_800C391E" = "__pad_bss_800C391E" + 0x0);
PROVIDE("D_800C4454" = "screenPosY.53" + 0x4);
PROVIDE("D_800CD768_tbl" = "D_800CD768" + 0x0);
