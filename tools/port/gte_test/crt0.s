.set noreorder
.text
.globl _start
_start:
    lui   $sp, 0x801F
    ori   $sp, $sp, 0xFF00
    mfc0  $t0, $12          /* SR: enable COP2 (CU2) */
    lui   $t1, 0x4000
    or    $t0, $t0, $t1
    mtc0  $t0, $12
    nop
    nop
    jal   main
    nop
1:  b 1b
    nop
