/* GUI.BIN: a flat ring-3 image linked at USER_BASE, header first. */
OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

SECTIONS
{
    . = 0x00400000;
    __image_start = .;
    .text : { KEEP(*(.header)) *(.text .text.*) }
    .rodata : ALIGN(16) { *(.rodata .rodata.*) }
    .data   : ALIGN(16) { *(.data .data.*) }
    __image_end = .;
    .bss (NOLOAD) : ALIGN(16) { *(.bss .bss.*) *(COMMON) }
    __mem_end = .;

    __image_size = __image_end - __image_start;
    __mem_size   = __mem_end - __image_start;

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
