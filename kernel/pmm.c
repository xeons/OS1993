/*
 * pmm.c -- physical page frame allocator.
 *
 * One bit per 4 KB frame of the XMS arena (1 = in use). The arena is at most
 * 65535 KB (the XMS 2.0 allocation limit), so the bitmap is a fixed 2 KB.
 * Frames below bootinfo.free_offset (kernel image, boot page tables) are
 * permanently reserved.
 */
#include "kernel.h"
#include "io.h"
#include "mm.h"

#define MAX_PAGES   (65536u * 1024u / PAGE_SIZE)
#define WORDS       (MAX_PAGES / 32)

static uint32_t bitmap[WORDS];
static uint32_t base_phys, base_virt;
static uint32_t npages, nfree, reserved;
static uint32_t hint;                   /* word to start searching from */

void pmm_init(const struct bootinfo *bi)
{
    base_phys = bi->arena_phys;
    base_virt = bi->arena_virt;
    npages    = bi->arena_size >> PAGE_SHIFT;
    reserved  = bi->free_offset >> PAGE_SHIFT;
    if (npages > MAX_PAGES)
        npages = MAX_PAGES;

    memset(bitmap, 0xFF, sizeof bitmap);    /* also covers bits past npages */
    for (uint32_t i = reserved; i < npages; i++)
        bitmap[i / 32] &= ~(1u << (i % 32));
    nfree = npages - reserved;
    hint  = reserved / 32;
}

uint32_t pmm_alloc(void)
{
    uint32_t flags = irq_save();
    uint32_t words = (npages + 31) / 32;

    for (uint32_t k = 0; k < words; k++) {
        uint32_t w = (hint + k) % words;
        if (bitmap[w] == 0xFFFFFFFFu)
            continue;
        uint32_t b = 0;
        while (bitmap[w] & (1u << b))       /* no BSF tricks: keep it simple */
            b++;
        bitmap[w] |= 1u << b;
        nfree--;
        hint = w;
        irq_restore(flags);
        return base_phys + (w * 32 + b) * PAGE_SIZE;
    }
    irq_restore(flags);
    return 0;
}

void pmm_free(uint32_t phys)
{
    uint32_t i = (phys - base_phys) >> PAGE_SHIFT;

    if ((phys & ~PAGE_MASK) || phys < base_phys || i >= npages)
        panic("pmm_free: %08X is not an arena frame.", phys);
    if (i < reserved)
        panic("pmm_free: frame %08X belongs to the kernel image or boot tables.", phys);

    uint32_t flags = irq_save();
    if (!(bitmap[i / 32] & (1u << (i % 32))))
        panic("pmm_free: frame %08X freed twice.", phys);
    bitmap[i / 32] &= ~(1u << (i % 32));
    nfree++;
    irq_restore(flags);
}

uint32_t pmm_total_pages(void) { return npages; }
uint32_t pmm_free_pages(void)  { return nfree; }

void *phys_to_virt(uint32_t phys)
{
    if (phys - base_phys < npages * PAGE_SIZE)
        return (void *)(uintptr_t)(base_virt + (phys - base_phys));
    if (phys < 0x00400000u)
        return (void *)(uintptr_t)phys;     /* identity-mapped low 4 MB */
    return NULL;
}
