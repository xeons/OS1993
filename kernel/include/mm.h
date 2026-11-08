/*
 * mm.h -- physical frames, page tables, kernel heap.
 *
 * Kernel linear address map
 *
 *   00000000-003FFFFF  identity: conventional memory, BIOS, video, HMA.
 *                      Becomes the current V86 machine's address space.
 *   80000000-83FFFFFF  XMS arena, direct mapped (phys_to_virt). Kernel image,
 *                      boot page tables, then frames handed out by the PMM.
 *   C0000000-CFFFFFFF  kernel heap (kmalloc), grown page by page
 *   D0000000-FFBFFFFF  free for future kernel mappings
 *   FFC00000-FFFFFFFF  recursive window: all page tables; PD at FFFFF000
 */
#ifndef OS1993_MM_H
#define OS1993_MM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "bootinfo.h"

#define PAGE_SIZE   4096u
#define PAGE_SHIFT  12
#define PAGE_MASK   (~(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(x)   (((x) + PAGE_SIZE - 1) & PAGE_MASK)
#define PAGE_ALIGN_DOWN(x) ((x) & PAGE_MASK)

/* Page directory / table entry bits (386: no global pages, no PAT) */
#define PTE_P    0x001u     /* present */
#define PTE_W    0x002u     /* writable */
#define PTE_U    0x004u     /* user (ring 3 / V86) accessible */
#define PTE_PWT  0x008u
#define PTE_PCD  0x010u
#define PTE_A    0x020u     /* accessed */
#define PTE_D    0x040u     /* dirty */
#define PTE_AVAIL_MASK 0xE00u  /* bits 9-11 are ours */

#define KHEAP_BASE   0xC0000000u
#define KHEAP_LIMIT  0xD0000000u
#define PT_WINDOW    0xFFC00000u
#define PD_WINDOW    0xFFFFF000u

/* pmm.c -- physical page frames in the XMS arena */
void     pmm_init(const struct bootinfo *bi);
uint32_t pmm_alloc(void);               /* physical address, 0 if exhausted */
void     pmm_free(uint32_t phys);
uint32_t pmm_total_pages(void);
uint32_t pmm_free_pages(void);
void    *phys_to_virt(uint32_t phys);   /* arena or low 4 MB, else NULL */

/* paging.c -- the current page directory, via the recursive mapping */
int      vmm_map(uint32_t va, uint32_t pa, uint32_t flags);  /* 0, or <0 */
uint32_t vmm_unmap(uint32_t va);        /* returns old frame, 0 if none */
bool     vmm_translate(uint32_t va, uint32_t *pa);
void     tlb_flush(void);

#define VMM_ENOMEM  (-1)
#define VMM_EEXIST  (-2)
#define VMM_EINVAL  (-3)

/* heap.c -- kernel heap */
struct heap_stats {
    uint32_t mapped;        /* bytes of heap backed by frames */
    uint32_t used;          /* bytes in allocated blocks (incl. overhead) */
    uint32_t free;          /* bytes in free blocks */
    uint32_t used_blocks;
    uint32_t free_blocks;
};

void  heap_init(void);
void *kmalloc(size_t size);             /* 8-byte aligned; NULL on failure */
void *kzalloc(size_t size);
void  kfree(void *p);
void  heap_check(void);                 /* panics on corruption */
void  heap_get_stats(struct heap_stats *st);

/* mmtest.c */
void  mm_selftest(void);

#endif
