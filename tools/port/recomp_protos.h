/* Prototypes for functions recompiled by tools/port/recomp.py that no game header declares (the game
 * calls them implicitly). Only read by the recompiler to size the wrappers; argument counts checked
 * against the PS1 code (register use). */
void Math_RotMatrixZxy(SVECTOR* rot, MATRIX* out);
void Math_RotMatrixXyzGte(SVECTOR* rot, MATRIX* out);
