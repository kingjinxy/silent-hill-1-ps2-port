/** @brief PS2 entry point: brings up the PS2 side, then runs the game's PS1 `main` (renamed
 * `Game_PsxMain` by tools/port/port_link.py). */

extern int printf(const char* fmt, ...);
extern int Game_PsxMain(void);

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    printf("SH1 port: starting\n");
    return Game_PsxMain();
}
