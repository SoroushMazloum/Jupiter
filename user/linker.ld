ENTRY(_start)
SECTIONS
{
    . = 0x00400000;
    .text : AT(0) { *(.text*) }
    .rodata : AT(LOADADDR(.text) + SIZEOF(.text)) { *(.rodata*) }
    .data : AT(LOADADDR(.rodata) + SIZEOF(.rodata)) { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.comment*) *(.eh_frame*) *(.note*) }
}
