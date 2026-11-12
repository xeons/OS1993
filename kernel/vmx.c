/*
 * vmx.c -- a VM's extended memory: the linear window XMS blocks and DPMI
 * memory live in.
 *
 * Each VM has its own 64 MB at VMX_BASE (the page directory entries for it
 * are swapped with the VM, as PDE 0 is), user-accessible so a DPMI client
 * in ring 3 can use it, but out of V86 code's reach. Blocks are carved out
 * of it first fit and backed by frames when allocated. XMS (xms.c) hands
 * out EMB handles over these blocks and locks one by giving its linear
 * address; DPMI (dpmi.c) gives clients their memory blocks from it.
 * Threads that aren't a VM's get an empty window.
 */
#include "kernel.h"
#include "io.h"
#include "mm.h"
#include "vm.h"

#define PTE_VMX (PTE_P | PTE_W | PTE_U)
#define PD ((volatile uint32_t *)PD_WINDOW)

static struct vm *installed;

/* Put vm's window into the page directory (NULL: none). */
void vmx_install(struct vm *vm)
{
    if (vm == installed)
        return;
    bool any = installed && installed->vmx_pts;
    for (unsigned i = 0; i < VMX_PDES; i++) {
        uint32_t pt = vm && vm->vmx_pts ? vm->vmx_pt[i] : 0;
        if (pt) any = true;
        PD[(VMX_BASE >> 22) + i] = pt ? pt | PTE_VMX : 0;
    }
    installed = vm;
    if (any)
        tlb_flush();
}

static volatile uint32_t *pte(struct vm *vm, uint32_t lin)
{
    uint32_t i = (lin - VMX_BASE) >> 22;
    if (!vm->vmx_pt[i]) {
        uint32_t f = pmm_alloc();
        if (!f)
            return NULL;
        memset(phys_to_virt(f), 0, PAGE_SIZE);
        vm->vmx_pt[i] = f;
        vm->vmx_pts++;
        if (vm == installed)
            PD[(VMX_BASE >> 22) + i] = f | PTE_VMX;
    }
    return (volatile uint32_t *)phys_to_virt(vm->vmx_pt[i]) + ((lin >> PAGE_SHIFT) & 1023);
}

static void unmap_range(struct vm *vm, uint32_t lin, uint32_t len)
{
    for (uint32_t p = lin; p < lin + len; p += PAGE_SIZE) {
        uint32_t i = (p - VMX_BASE) >> 22;
        if (!vm->vmx_pt[i])
            continue;
        volatile uint32_t *e = (volatile uint32_t *)phys_to_virt(vm->vmx_pt[i]) + ((p >> PAGE_SHIFT) & 1023);
        if (*e & PTE_P) {
            pmm_free(*e & PAGE_MASK);
            *e = 0;
            if (vm == installed)
                invlpg(p);
        }
    }
}

static bool map_range(struct vm *vm, uint32_t lin, uint32_t len)
{
    for (uint32_t p = lin; p < lin + len; p += PAGE_SIZE) {
        volatile uint32_t *e = pte(vm, p);
        uint32_t f = e ? pmm_alloc() : 0;
        if (!f) {
            unmap_range(vm, lin, p - lin);
            return false;
        }
        memset(phys_to_virt(f), 0, PAGE_SIZE);
        *e = f | PTE_VMX;
    }
    return true;
}

static struct vmx_block *find(struct vm *vm, uint32_t lin)
{
    for (int i = 0; i < VMX_BLOCKS; i++)
        if (vm->vmx[i].size && vm->vmx[i].base == lin)
            return &vm->vmx[i];
    return NULL;
}

/* The lowest gap of size bytes. */
static uint32_t gap(struct vm *vm, uint32_t size, const struct vmx_block *ignore)
{
    uint32_t at = VMX_BASE;
    for (;;) {
        bool moved = false;
        for (int i = 0; i < VMX_BLOCKS; i++) {
            const struct vmx_block *b = &vm->vmx[i];
            if (!b->size || b == ignore)
                continue;
            if (at < b->base + b->size && b->base < at + size) {
                at = b->base + b->size;
                moved = true;
            }
        }
        if (!moved)
            break;
    }
    return at + size <= VMX_BASE + VMX_SIZE ? at : 0;
}

uint32_t vmx_used(struct vm *vm, int owner)
{
    uint32_t n = 0;
    for (int i = 0; i < VMX_BLOCKS; i++)
        if (vm->vmx[i].size && vm->vmx[i].owner == owner)
            n += vm->vmx[i].size;
    return n;
}

static uint32_t quota(struct vm *vm, int owner)
{
    return (uint32_t)(owner == VMX_XMS ? vm->set.xms_kb : vm->set.dpmi_kb) * 1024;
}

/* What owner could still get, in bytes (the quota, and the frames). */
uint32_t vmx_avail(struct vm *vm, int owner)
{
    uint32_t q = quota(vm, owner), u = vmx_used(vm, owner);
    uint32_t left = q > u ? q - u : 0;
    uint32_t frames = pmm_free_pages() > 64 ? (pmm_free_pages() - 64) * PAGE_SIZE : 0;
    return left < frames ? left : frames;
}

/* A block of size bytes (rounded to pages) -> its linear address, or 0. */
uint32_t vmx_alloc(struct vm *vm, uint32_t size, int owner)
{
    size = PAGE_ALIGN_UP(size ? size : 1);
    if (size > vmx_avail(vm, owner))
        return 0;
    struct vmx_block *b = NULL;
    for (int i = 0; i < VMX_BLOCKS && !b; i++)
        if (!vm->vmx[i].size) b = &vm->vmx[i];
    uint32_t at = b ? gap(vm, size, NULL) : 0;
    if (!at || !map_range(vm, at, size))
        return 0;
    b->base = at;
    b->size = size;
    b->owner = (uint8_t)owner;
    b->locks = 0;
    return at;
}

bool vmx_free(struct vm *vm, uint32_t lin)
{
    struct vmx_block *b = find(vm, lin);
    if (!b)
        return false;
    unmap_range(vm, b->base, b->size);
    b->size = 0;
    return true;
}

uint32_t vmx_size(struct vm *vm, uint32_t lin)
{
    struct vmx_block *b = find(vm, lin);
    return b ? b->size : 0;
}

/* Resize a block; it may move (its new address is returned, 0 if it can't
 * be done). The contents up to the smaller size are kept. */
uint32_t vmx_resize(struct vm *vm, uint32_t lin, uint32_t size)
{
    struct vmx_block *b = find(vm, lin);
    if (!b)
        return 0;
    size = PAGE_ALIGN_UP(size ? size : 1);
    if (size <= b->size) {
        unmap_range(vm, b->base + size, b->size - size);
        b->size = size;
        return b->base;
    }
    uint32_t more = size - b->size;
    if (more > vmx_avail(vm, b->owner))
        return 0;
    if (gap(vm, size, b) == b->base) {          /* room right after it */
        if (!map_range(vm, b->base + b->size, more))
            return 0;
        b->size = size;
        return b->base;
    }
    uint32_t to = gap(vm, size, b);             /* elsewhere: move the frames over */
    if (!to)
        return 0;
    for (uint32_t off = 0; off < size; off += PAGE_SIZE) {
        volatile uint32_t *dst = pte(vm, to + off);
        if (!dst)
            return 0;                           /* (a page table: frames ran out) */
    }
    for (uint32_t off = 0; off < b->size; off += PAGE_SIZE) {
        volatile uint32_t *src = pte(vm, b->base + off), *dst = pte(vm, to + off);
        *dst = *src;
        *src = 0;
        if (vm == installed) invlpg(b->base + off);
    }
    if (!map_range(vm, to + b->size, more)) {   /* (undo) */
        for (uint32_t off = 0; off < b->size; off += PAGE_SIZE) {
            volatile uint32_t *src = pte(vm, to + off), *dst = pte(vm, b->base + off);
            *dst = *src;
            *src = 0;
        }
        if (vm == installed) tlb_flush();
        return 0;
    }
    b->base = to;
    b->size = size;
    return to;
}

int vmx_lock(struct vm *vm, uint32_t lin, int delta)
{
    struct vmx_block *b = find(vm, lin);
    if (!b)
        return -1;
    if (delta < 0 && !b->locks)
        return -1;
    b->locks = (uint8_t)(b->locks + delta);
    return b->locks;
}

/* A kernel pointer to len bytes of the VM's window at lin, if all mapped
 * (the window must be installed: the VM's own thread). */
void *vmx_ptr(struct vm *vm, uint32_t lin, uint32_t len)
{
    if (lin < VMX_BASE || len > VMX_SIZE || lin - VMX_BASE > VMX_SIZE - len || vm != installed)
        return NULL;
    for (uint32_t p = lin & PAGE_MASK; p < lin + len; p += PAGE_SIZE) {
        uint32_t i = (p - VMX_BASE) >> 22;
        if (!vm->vmx_pt[i] || !(((volatile uint32_t *)phys_to_virt(vm->vmx_pt[i]))[(p >> PAGE_SHIFT) & 1023] & PTE_P))
            return NULL;
    }
    return (void *)(uintptr_t)lin;
}

/* The VM ended: all of it goes. */
void vmx_vm_gone(struct vm *vm)
{
    for (int i = 0; i < VMX_BLOCKS; i++)
        if (vm->vmx[i].size)
            vmx_free(vm, vm->vmx[i].base);
    if (installed == vm)
        vmx_install(NULL);
    for (unsigned i = 0; i < VMX_PDES; i++)
        if (vm->vmx_pt[i]) {
            pmm_free(vm->vmx_pt[i]);
            vm->vmx_pt[i] = 0;
        }
    vm->vmx_pts = 0;
}
