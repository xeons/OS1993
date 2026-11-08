/*
 * paging.c -- edit the current address space through the recursive mapping.
 *
 * PDE 1023 points at the page directory itself, so:
 *   the PTE for linear address va is at  PT_WINDOW + (va >> 12) * 4
 *   the PDE for va is at                 PD_WINDOW + (va >> 22) * 4
 *
 * The 386 has no INVLPG, so any change to a present entry flushes the whole
 * TLB by reloading CR3. Not-present entries are never cached, so creating a
 * mapping needs no flush.
 *
 * TODO(V86): once each VM has its own page directory, kernel PDEs (>= 2 GB)
 * must stay identical in all of them. Either preallocate the kernel page
 * tables or propagate new kernel PDEs to every VM's directory.
 */
#include "kernel.h"
#include "io.h"
#include "mm.h"

#define PDE(va) (((volatile uint32_t *)PD_WINDOW)[(uint32_t)(va) >> 22])
#define PTE(va) (((volatile uint32_t *)PT_WINDOW)[(uint32_t)(va) >> 12])
#define PT_OF(va) ((void *)(uintptr_t)(PT_WINDOW + (((uint32_t)(va) >> 22) << PAGE_SHIFT)))

void tlb_flush(void)
{
    __asm__ volatile("mov %%cr3, %%eax\n\tmov %%eax, %%cr3" ::: "eax", "memory");
}

int vmm_map(uint32_t va, uint32_t pa, uint32_t flags)
{
    if ((va & ~PAGE_MASK) || (pa & ~PAGE_MASK) || va >= PT_WINDOW)
        return VMM_EINVAL;

    int rc = 0;
    uint32_t irq = irq_save();

    if (!(PDE(va) & PTE_P)) {
        uint32_t pt = pmm_alloc();
        if (!pt) { rc = VMM_ENOMEM; goto out; }
        PDE(va) = pt | PTE_P | PTE_W | (flags & PTE_U);
        memset(PT_OF(va), 0, PAGE_SIZE);
    } else if (flags & PTE_U) {
        PDE(va) |= PTE_U;    /* x86 ANDs PDE and PTE permissions */
    }

    if (PTE(va) & PTE_P) { rc = VMM_EEXIST; goto out; }
    PTE(va) = pa | (flags & (PTE_W | PTE_U | PTE_PWT | PTE_PCD | PTE_AVAIL_MASK)) | PTE_P;
out:
    irq_restore(irq);
    return rc;
}

uint32_t vmm_unmap(uint32_t va)
{
    va = PAGE_ALIGN_DOWN(va);
    if (va >= PT_WINDOW)
        return 0;

    uint32_t irq = irq_save();
    uint32_t old = 0;
    if ((PDE(va) & PTE_P) && (PTE(va) & PTE_P)) {
        old = PTE(va) & PAGE_MASK;
        PTE(va) = 0;
        tlb_flush();
    }
    irq_restore(irq);
    return old;
}

bool vmm_translate(uint32_t va, uint32_t *pa)
{
    if (!(PDE(va) & PTE_P) || !(PTE(va) & PTE_P))
        return false;
    if (pa)
        *pa = (PTE(va) & PAGE_MASK) | (va & ~PAGE_MASK);
    return true;
}

void klow_init(void)
{
    for (uint32_t pa = 0; pa < KLOW_SIZE; pa += PAGE_SIZE)
        if (vmm_map(KLOW_BASE + pa, pa, PTE_W) != 0)
            panic("klow_init: cannot map %08X.", pa);
}
