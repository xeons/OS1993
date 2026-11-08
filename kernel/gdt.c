/*
 * gdt.c -- the kernel's own GDT.
 *
 * Planned growth (see README roadmap):
 *   0x18/0x20  ring-3 code/data for protected-mode applications
 *   0x28       TSS (ring transitions; ESP0 for V86 virtual machines)
 *   0x30+      LDTs for the System VM / Win16 layer
 */
#include "gdt.h"

struct gdt_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;        /* flags (G, D/B) + limit 19:16 */
    uint8_t  base_hi;
} __attribute__((packed));

static struct gdt_entry gdt[3];

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

    static struct dtr gdtr;
    gdtr.limit = sizeof gdt - 1;
    gdtr.base  = (uint32_t)(uintptr_t)gdt;
    gdt_load(&gdtr);
}
