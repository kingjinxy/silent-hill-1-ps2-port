/** @brief PS2 entry point: brings up the PS2 side, then runs the game's PS1 `main` (renamed
 * `Game_PsxMain` by tools/port/port_link.py). */

extern int printf(const char* fmt, ...);
extern int Game_PsxMain(void);

extern void Crash_Install(void);        /* src/port/ps2/crash_ps2.c */
extern void Port_MainThreadInit(void);  /* src/port/ps2/libetc_ps2.c */

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    printf("SH1 port: starting\n");
    Port_MainThreadInit();
    Crash_Install();
    return Game_PsxMain();
}
