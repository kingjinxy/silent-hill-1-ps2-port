/* Port build only (static linking). Addresses in bodyprog that other binaries use but bodyprog
 * never names: bss it only reserves (`__pad_bss_*`) or bytes inside a labelled object. On PS1 the
 * other binaries reach them through splat's absolute assignments; once everything is linked
 * together they have to be defined relative to bodyprog's own symbols. Offsets from the matching
 * layout. See also relative_syms.ld (symbols referenced from inside their own binary). */
PROVIDE("D_800A9938" = "g_CharaModelAnimsData" + 0xC);
PROVIDE("D_800A9945" = "g_CharaModelAnimsData" + 0x19);
PROVIDE("D_800A99B5" = "g_PaperMapFileIdxs" + 0x1);
PROVIDE("D_800C39A0" = "__pad_bss_800C39A0" + 0x0);
PROVIDE("g_Player_AnimResetRequest" = "__pad_bss_800C4606" + 0x0);
PROVIDE("g_SaveScreen_IsLoadError" = "__pad_bss_800BCD39" + 0x0);
