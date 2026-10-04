/* Port build only: game symbols the PS1 build gets from fixed addresses. */

/* Fixed buffers referenced from map7_s03 data (see sym.map7_s03.txt): inside the RAM arena, like
 * PSX_RAM_ADDR(). */
g_FsBuffer18 = g_PsxRam + 0x180600;
g_FsBuffer20 = g_PsxRam + 0x185600;

/* bodyprog reads the current map's header through this name. Temporary: always map0_s00, until the
 * overlay-load hook (Step 2) switches maps. */
g_MapOverlayHdr = g_MapOverlayHdr_map0_s00;
