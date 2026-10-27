/** @brief SH1's settings for the HAL (psx-to-ps2-HAL include/port/hal_game.h). Plain macros only: the
 * HAL's PS2-side files include this against ps2sdk's headers, not the game's. */

#ifndef HAL_GAME_CONFIG_H
#define HAL_GAME_CONFIG_H

#define HAL_GAME_NAME         "SH1"
#define HAL_GAME_PSX_RAM_SIZE 0x200000 /* as PSX_RAM_SIZE (include/decomp/psx_mem.h) */

/* Disc (ps2/libcd_ps2.c): SILENT. (2048-byte sectors) from PS1 sector 64, HILL. (raw 2336-byte XA
 * sectors) right after it. */
#define HAL_GAME_CD_DATA_FILE       "\\SILENT.;1"
#define HAL_GAME_CD_DATA_PS1_SECTOR 64
#define HAL_GAME_CD_RAW_FILE        "\\HILL.;1"

/* Memory card: the PS1 product code of save files (psx_card.c), the PS2 save directory holding the
 * card images (ps2/memcard_ps2.c), and the save icon (src/bodyprog/memcard.c). */
#define HAL_GAME_PS1_PRODUCT    "BASLUS-00707"
#define HAL_GAME_PS2_SAVE_DIR   "BASLUS-00707SH1"
#define HAL_GAME_PS2_ICON_FILE  "SH1.ICO"
#define HAL_GAME_PS2_SAVE_TITLE "SILENT HILLPS1 saves"
#define HAL_GAME_SAVE_ICON      g_MemCard_SaveIconTim

#endif
