/*
 * bootinfo.h -- the contract between OS1993.COM (real-mode loader) and
 * VMM.BIN (32-bit kernel).
 *
 * The loader leaves this structure in conventional memory (identity mapped)
 * and passes its linear address to the kernel in EBX.
 *
 * KEEP IN SYNC with the `bootinfo:` block in loader/os1993.asm. The static
 * asserts below catch offset drift on the C side.
 *
 * Machine state on kernel entry:
 *   - 32-bit protected mode, paging enabled, interrupts disabled
 *   - CS = flat code (loader GDT 0x18), DS/ES/FS/GS/SS = flat data (0x20)
 *   - linear 0x00000000-0x003FFFFF identity mapped (conventional memory,
 *     BIOS, video RAM, HMA)
 *   - the whole XMS arena mapped contiguously at arena_virt (0x80000000);
 *     the kernel image sits at the start of it
 *   - page directory entry 1023 points at the page directory itself
 *     (recursive mapping: page tables visible at 0xFFC00000, PD at 0xFFFFF000)
 *   - A20 enabled (via XMS)
 *   - ESP is NOT valid; the kernel sets up its own stack
 */
#ifndef OS1993_BOOTINFO_H
#define OS1993_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_MAGIC   0x3339534Fu   /* "OS93" */
#define KHDR_MAGIC       0x334D4D56u   /* "VMM3" */

#define BI_FLAG_TEST     0x00000001u   /* /T: automated test run, auto-exit */

struct bootinfo {
    uint32_t magic;        /*  0 BOOTINFO_MAGIC                              */
    uint32_t flags;        /*  4 BI_FLAG_*                                   */
    uint32_t arena_phys;   /*  8 physical base of XMS arena (4K aligned)     */
    uint32_t arena_size;   /* 12 arena size in bytes (multiple of 4K)        */
    uint32_t arena_virt;   /* 16 where the arena is mapped (kernel vbase)    */
    uint32_t kernel_size;  /* 20 image + bss, rounded up to 4K               */
    uint32_t pd_phys;      /* 24 physical address of the page directory      */
    uint32_t free_offset;  /* 28 first free byte in the arena (offset)       */
    uint32_t loader_base;  /* 32 linear base of the loader's segment         */
    uint32_t return_off;   /* 36 far pointer to the loader's return stub ... */
    uint16_t return_sel;   /* 40 ... (selector in the loader's GDT)          */
    uint16_t reserved0;    /* 42                                             */
    uint16_t reserved1;    /* 44                                             */
    uint16_t gdtr_limit;   /* 46 loader GDTR, restore before returning       */
    uint32_t gdtr_base;    /* 48                                             */
    uint32_t xms_kb;       /* 52 size of the XMS block actually allocated    */
    /* Offsets below are within the loader's segment (loader_base >> 4).   */
    uint16_t v86_stub_off; /* 56 two ARPL breakpoints: end of kernel V86
                                 calls, and (+2) returns from DOS            */
    uint16_t v86_stack_top;/* 58 initial SP for kernel V86 calls             */
    uint16_t kpath_off;    /* 60 ASCIIZ path the kernel was loaded from      */
    uint16_t buf_off;      /* 62 conventional-memory transfer buffer ...     */
    uint16_t buf_size;     /* 64 ... and its size in bytes                   */
    uint8_t  pic1_mask;    /* 66 IRQ masks DOS had when we started           */
    uint8_t  pic2_mask;    /* 67                                             */
} __attribute__((packed));

_Static_assert(__builtin_offsetof(struct bootinfo, return_off) == 36, "bootinfo layout");
_Static_assert(__builtin_offsetof(struct bootinfo, gdtr_limit) == 46, "bootinfo layout");
_Static_assert(__builtin_offsetof(struct bootinfo, xms_kb)     == 52, "bootinfo layout");
_Static_assert(__builtin_offsetof(struct bootinfo, pic1_mask)  == 66, "bootinfo layout");
_Static_assert(sizeof(struct bootinfo) == 68,                          "bootinfo layout");

/* Return to DOS through the loader. Never returns. (kernel/entry.asm) */
__attribute__((noreturn))
void exit_to_dos(const struct bootinfo *bi, uint8_t exit_code);

#endif
