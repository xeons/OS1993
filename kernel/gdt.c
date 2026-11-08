/*
 * gdt.c -- the kernel's GDT and its single TSS.
 *
 *   0x08  ring 0 code, flat
 *   0x10  ring 0 data, flat
 *   0x18  TSS: ESP0 for traps out of V86 mode, plus the I/O permission bitmap
 *
 * Planned: ring-3 code/data for protected-mode apps, LDTs for the Win16 layer.
 */
#include "gdt.h"
#include "kernel.h"

struct gdt_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;        /* flags (G, D/B) + limit 19:16 */
    uint8_t  base_hi;
} __attribute__((packed));

static struct gdt_entry gdt[4];
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

void tss_trap_port(uint16_t port, bool trap)
{
    if (trap) kernel_tss.iomap[port / 8] |=  (uint8_t)(1u << (port % 8));
    else      kernel_tss.iomap[port / 8] &= (uint8_t)~(1u << (port % 8));
}
