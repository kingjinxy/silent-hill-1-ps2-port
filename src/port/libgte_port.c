/** @brief Hand-written libgte functions the recompiler can't translate (see tools/port/recomp_all.sh).
 * The rest of libgte is recompiled from Sony's library objects.
 */

#include "port/gte.h"

/** libgte msc00.o. On PS1 this also installs a kernel patch (`_patch_gte`) and enables COP2 in the
 * status register; neither applies to the software GTE. Default register values as set by the
 * original. */
void InitGeom(void)
{
    Gte_CtrlWrite(29, 341);         /* ZSF3 */
    Gte_CtrlWrite(30, 256);         /* ZSF4 */
    Gte_CtrlWrite(26, 1000);        /* H */
    Gte_CtrlWrite(27, (unsigned int)-4194); /* DQA */
    Gte_CtrlWrite(28, 0x1400000);   /* DQB */
    Gte_CtrlWrite(24, 0);           /* OFX */
    Gte_CtrlWrite(25, 0);           /* OFY */
}
