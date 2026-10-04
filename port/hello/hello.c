#include <stdio.h>
#include <debug.h>
#include <kernel.h>

int main(void)
{
    init_scr();
    scr_printf("Silent Hill PS2 port: toolchain OK\n");
    printf("Silent Hill PS2 port: toolchain OK\n");
    SleepThread();
    return 0;
}
