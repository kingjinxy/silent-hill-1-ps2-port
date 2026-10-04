ENTRY(_start)
SECTIONS
{
    . = 0x80010000;
    .text : { *(.text.startup) crt0.o(.text) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) *(.sdata*) }
    _end_of_file = .;
    .bss : { *(.sbss*) *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) *(.note*) }
}
