/*
 * gdt.c -- the kernel's GDT and its single TSS.
 *
 *   0x08  ring 0 code, flat
 *   0x10  ring 0 data, flat
 *   0x18  TSS: ESP0 for traps out of V86 mode and ring 3, I/O permission bitmap
 *   0x20  ring 3 code, flat  (selector 0x23 with RPL 3)
 *   0x28  ring 3 data, flat  (selector 0x2B)
 *
 * Ring-3 segments cover all 4 GB; paging keeps user code out of kernel pages.
 */
#include "gdt.h"
#include "mm.h"
#include "kernel.h"

struct gdt_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;        /* flags (G, D/B) + limit 19:16 */
    uint8_t  base_hi;
} __attribute__((packed));

static struct gdt_entry gdt[7];
struct tss kernel_tss;

static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    gdt[i].limit_lo = limit & 0xFFFF;
    gdt[i].base_lo  = base & 0xFFFF;
    gdt[i].base_mid = (base >> 16) & 0xFF;
    gdt[i].access   = access;
    gdt[i].gran     = (uint8_t)((flags & 0xF0) | ((limit >> 16) & 0x0F));
    gdt[i].base_hi  = (base >> 24) & 0xFF;
}

void gdt_init(void)
{
    gdt_set(0, 0, 0, 0, 0);
    gdt_set(KCODE_SEL >> 3, 0, 0xFFFFF, 0x9A, 0xC0);   /* ring 0 code, flat 4G */
    gdt_set(KDATA_SEL >> 3, 0, 0xFFFFF, 0x92, 0xC0);   /* ring 0 data, flat 4G */
    gdt_set(UCODE_SEL >> 3, 0, 0xFFFFF, 0xFA, 0xC0);   /* ring 3 code, flat 4G */
    gdt_set(UDATA_SEL >> 3, 0, 0xFFFFF, 0xF2, 0xC0);   /* ring 3 data, flat 4G */
    gdt_set(UTLS_SEL >> 3, 0, PAGE_SIZE - 1, 0xF2, 0x40);  /* ring 3 data, one page: the TIB */

    /* All ports allowed by default (bitmap zeroed in .bss); the terminating
     * byte past the bitmap must have all bits set. */
    kernel_tss.ss0 = KDATA_SEL;
    kernel_tss.iomap_base = (uint16_t)__builtin_offsetof(struct tss, iomap);
    kernel_tss.iomap_end = 0xFF;
    gdt_set(TSS_SEL >> 3, (uint32_t)(uintptr_t)&kernel_tss, sizeof kernel_tss - 1,
            0x89, 0x00);                                /* available 386 TSS */

    static struct dtr gdtr;
    gdtr.limit = sizeof gdt - 1;
    gdtr.base  = (uint32_t)(uintptr_t)gdt;
    gdt_load(&gdtr);
    __asm__ volatile("ltr %w0" : : "r"(TSS_SEL));
}

/* The scheduler, switching to a user thread: FS is reloaded from here on
 * the way back to ring 3 (isr_common pops it), so it finds that thread's
 * TIB. */
void gdt_set_tls(uint32_t base)
{
    gdt_set(UTLS_SEL >> 3, base, PAGE_SIZE - 1, 0xF2, 0x40);
}

void tss_trap_port(uint16_t port, bool trap)
{
    if (trap) kernel_tss.iomap[port / 8] |=  (uint8_t)(1u << (port % 8));
    else      kernel_tss.iomap[port / 8] &= (uint8_t)~(1u << (port % 8));
}
