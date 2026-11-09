/* A MOD3 module (program or DLL) linked at MODBASE (-Wl,--defsym). The
 * header comes first; .imports is always present and 0-terminated. */
OUTPUT_FORMAT(elf32-i386)
ENTRY(__image_start)

SECTIONS
{
    . = MODBASE;
    __image_start = .;
    .text : { KEEP(*(.header)) *(.text .text.*) }
    .rodata  : ALIGN(16) { *(.rodata .rodata.*) }
    .exports : ALIGN(4)  { KEEP(*(.exports)) }
    .data    : ALIGN(16) { *(.data .data.*) }
    .imports : ALIGN(4)  { __imports = .; KEEP(*(.imports)) LONG(0) LONG(0) LONG(0) }
    __image_end = .;
    .bss (NOLOAD) : ALIGN(16) { *(.bss .bss.*) *(COMMON) }
    __mem_end = .;

    __image_size = __image_end - __image_start;
    __mem_size   = __mem_end - __image_start;
    PROVIDE(__exports = 0);

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
