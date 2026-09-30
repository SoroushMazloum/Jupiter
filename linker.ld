ENTRY(_start)
SECTIONS {
    . = 0x1000;
    .text   : { *kernel_entry.o(.text) *(.text .text.*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.note*) *(.eh_frame*) *(.comment) }
}