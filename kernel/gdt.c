/*
 * gdt.c -- the kernel's GDT and its single TSS.
 *
 *   0x08  ring 0 code, flat
 *   0x10  ring 0 data, flat
 *   0x18  TSS: ESP0 for traps out of V86 mode and ring 3, I/O permission bitmap
 *   0x20  ring 3 code, flat  (selector 0x23 with RPL 3)
 *   0x28  ring 3 data, flat  (selector 0x2B)
 *   0x30  ring 3 data at the running thread's TIB (FS, selector 0x33)
 *   0x38  the double-fault TSS: a task of its own, with its own stack, so a
 *         kernel stack overflow (which faults again pushing the #PF frame)
 *         still reaches a handler
 *   0x40  the LDT of the running DPMI client (dpmi.c)
 *
 * Ring-3 segments cover all 4 GB; paging keeps user code out of kernel pages.
 */
#include "gdt.h"
#include "mm.h"
#include "kernel.h"
#include "io.h"

struct gdt_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;        /* flags (G, D/B) + limit 19:16 */
    uint8_t  base_hi;
} __attribute__((packed));

static struct gdt_entry gdt[9];
struct tss kernel_tss;

/* The double-fault task: no I/O bitmap, its own stack. */
static struct {
    uint32_t link, esp0, ss0, esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap_base;
} df_tss;
static uint8_t df_stack[8192] __attribute__((aligned(16)));
extern void double_fault_entry(void);       /* isr.asm */

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

    df_tss.cr3 = read_cr3();
    df_tss.eip = (uint32_t)(uintptr_t)double_fault_entry;
    df_tss.eflags = 0x2;                                /* interrupts off */
    df_tss.esp = df_tss.esp0 = (uint32_t)(uintptr_t)(df_stack + sizeof df_stack);
    df_tss.cs = KCODE_SEL;
    df_tss.ss = df_tss.ss0 = df_tss.ds = df_tss.es = df_tss.fs = df_tss.gs = KDATA_SEL;
    df_tss.iomap_base = sizeof df_tss;                  /* none */
    gdt_set(DFTSS_SEL >> 3, (uint32_t)(uintptr_t)&df_tss, sizeof df_tss - 1, 0x89, 0x00);

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

/* The LDT descriptor (a DPMI client's LDT, or none). */
void gdt_set_ldt(uint32_t base, uint32_t limit)
{
    if (limit) gdt_set(LDT_SEL >> 3, base, limit, 0x82, 0x00);
    else       gdt_set(LDT_SEL >> 3, 0, 0, 0, 0);
}

void tss_trap_port(uint16_t port, bool trap)
{
    if (trap) kernel_tss.iomap[port / 8] |=  (uint8_t)(1u << (port % 8));
    else      kernel_tss.iomap[port / 8] &= (uint8_t)~(1u << (port % 8));
}
