#ifndef _PORT_WIDESCREEN_H
#define _PORT_WIDESCREEN_H

/** @brief Port: anamorphic 16:9 widescreen (Debug Options "Widescreen", `config.unused_26` bit 3;
 * src/port/hal_game.c). The 3D view is squeezed horizontally by 3/4 and widened to match (hor+,
 * vw_calc.c vbSetWorldScreenMatrix); 2D is squeezed by the GS renderer per ordering-table layer
 * (HalGame_OnOtTag); a TV in 16:9 mode stretches the picture back. Cutscene borders become 14:9 side
 * bars, with the view zoomed by up to 7/6 so the full height shows what the 192 lines between the
 * original bars did (cutscene_border.c, vc_main.c). */

/** Non-zero while the 3D view is widened (widescreen on, in gameplay; updated at every DrawOTag). Other
 * screens are squeezed whole instead, 3D included. */
extern int g_PortWidescreen;

/** Second word of the empty marker packets around a full-screen picture (background_draw.c): the GS
 * renderer squeezes what is between them in gameplay (HalGame_OnOtTag). */
#define PORT_PICTURE_START 0x57D1C001
#define PORT_PICTURE_END   0x57D1C000

/** The cutscene borders' shade, Q12 0 (none) to 1 (shown) (cutscene_border.c). */
int Port_CutsceneBorderShade(void);

#endif
