/*
 * VMM.BIN layout. The loader maps the XMS arena at KERNEL_VBASE and copies
 * the flat binary to its start, so the first byte of the file must be the
 * kernel header (section .header, kernel/entry.asm).
 */
OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

KERNEL_VBASE = 0x80000000;

SECTIONS
{
    . = KERNEL_VBASE;
    __image_start = .;

    .text : {
        KEEP(*(.header))
        *(.text .text.*)
    }
    .rodata : ALIGN(16) { *(.rodata .rodata.*) }
    .data   : ALIGN(16) { *(.data .data.*) }
    __image_end = .;

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    __kernel_end = .;

    __image_size  = __image_end  - __image_start;   /* bytes in VMM.BIN  */
    __kernel_size = __kernel_end - __image_start;   /* image + bss       */

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
