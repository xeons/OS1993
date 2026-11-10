/*
 * heap.c -- memory: virtual memory (VirtualAlloc ...) and heaps (HeapAlloc
 * ..., LocalAlloc), as in Win32.
 *
 * The process heap grows with SYS_SBRK; a heap made with HeapCreate lives
 * in a region of its own, committed as it grows. Both are first-fit lists
 * of blocks, each checked as it is walked (a block whose header was
 * overwritten ends the process rather than corrupting more), and locked
 * so a process's threads can share them.
 */
#include "kpriv.h"

/* ---- virtual memory ------------------------------------------------------------- */

static int vmem(uint32_t op, struct vmem_args *a)
{
    int rc = (int)syscall3(SYS_VMEM, op, (uint32_t)(uintptr_t)a, 0);
    if (rc < 0)
        k_error((uint32_t)-rc);
    return rc;
}

static int prot_of(uint32_t protect)                /* PAGE_* -> VM_PROT_*, or -1 */
{
    switch (protect) {
    case PAGE_READONLY: case PAGE_EXECUTE: case PAGE_EXECUTE_READ:
        return VM_PROT_READ;
    case PAGE_READWRITE: case PAGE_EXECUTE_READWRITE: case PAGE_WRITECOPY: case PAGE_EXECUTE_WRITECOPY:
        return VM_PROT_READ | VM_PROT_WRITE;
    }
    return -1;                                      /* PAGE_NOACCESS: decommit instead */
}

static uint32_t page_of(uint32_t prot)
{
    return !prot ? PAGE_NOACCESS : (prot & VM_PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
}

void *VirtualAlloc(void *addr, uint32_t size, uint32_t type, uint32_t protect)
{
    int prot = prot_of(protect);
    if (prot < 0 || !(type & (MEM_RESERVE | MEM_COMMIT)) || !size) {
        k_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    struct vmem_args a = { (uint32_t)(uintptr_t)addr, size, (uint32_t)prot, 0, 0, 0, 0 };
    if ((type & MEM_RESERVE) && addr) {             /* there, then (maybe) commit */
        if (vmem(VM_RESERVE, &a) < 0)
            return NULL;
        if (!(type & MEM_COMMIT))
            return (void *)(uintptr_t)a.base;
        a.addr = a.base;
    } else if (type == MEM_RESERVE) {
        return vmem(VM_RESERVE, &a) < 0 ? NULL : (void *)(uintptr_t)a.base;
    }
    return vmem(VM_COMMIT, &a) < 0 ? NULL : (void *)(uintptr_t)a.base;
}

bool VirtualFree(void *addr, uint32_t size, uint32_t type)
{
    struct vmem_args a = { (uint32_t)(uintptr_t)addr, size, 0, 0, 0, 0, 0 };
    if (type == MEM_RELEASE && !size)
        return vmem(VM_RELEASE, &a) == 0;
    if (type == MEM_DECOMMIT && size)
        return vmem(VM_DECOMMIT, &a) == 0;
    k_error(ERROR_INVALID_PARAMETER);
    return false;
}

bool VirtualProtect(void *addr, uint32_t size, uint32_t protect, uint32_t *old)
{
    int prot = prot_of(protect);
    if (prot < 0) {
        k_error(ERROR_INVALID_PARAMETER);
        return false;
    }
    struct vmem_args a = { (uint32_t)(uintptr_t)addr, size, (uint32_t)prot, 0, 0, 0, 0 };
    if (vmem(VM_PROTECT, &a) < 0)
        return false;
    if (old)
        *old = page_of(a.old_prot);
    return true;
}

uint32_t VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, uint32_t len)
{
    struct vmem_args a = { (uint32_t)(uintptr_t)addr, 0, 0, 0, 0, 0, 0 };
    if (len < sizeof *mbi || vmem(VM_QUERY, &a) < 0)
        return 0;
    mbi->BaseAddress = (void *)(uintptr_t)a.base;
    mbi->AllocationBase = a.state == VM_STATE_FREE ? NULL : (void *)(uintptr_t)a.base;
    mbi->AllocationProtect = a.state == VM_STATE_FREE ? 0 : PAGE_READWRITE;
    mbi->RegionSize = a.region_size;
    mbi->State = a.state;
    mbi->Protect = page_of(a.old_prot);
    mbi->Type = a.state == VM_STATE_FREE ? 0 : MEM_PRIVATE;
    return sizeof *mbi;
}

void GlobalMemoryStatus(MEMORYSTATUS *ms)
{
    struct mem_info mi = { 0, 0, 0 };
    syscall3(SYS_MEMINFO, (uint32_t)(uintptr_t)&mi, 0, 0);
    ms->dwLength = sizeof *ms;
    ms->dwMemoryLoad = mi.total_pages ? 100 - mi.free_pages * 100 / mi.total_pages : 0;
    ms->dwTotalPhys = mi.total_pages * 4096;
    ms->dwAvailPhys = mi.free_pages * 4096;
    ms->dwTotalPageFile = ms->dwAvailPageFile = 0;  /* no paging */
    ms->dwTotalVirtual = USER_LIMIT - USER_BASE;
    ms->dwAvailVirtual = ms->dwTotalVirtual - mi.process_pages * 4096;
}

/* ---- heaps -------------------------------------------------------------------- */

#define HEAP_MAGIC  0x48454150u                     /* "HEAP" */
#define BLOCK_MAGIC 0xB10Cu
#define GROW        (16u * 1024)

struct block {                                      /* 8 bytes, before each block */
    uint32_t size;                                  /* the header included */
    uint16_t free;
    uint16_t magic;
};

struct heap {
    uint32_t         magic;
    bool             process;                       /* grows with SYS_SBRK */
    bool             serialize;
    CRITICAL_SECTION lock;
    struct block    *first, *end;                   /* [first, end) */
    uint32_t         base, committed, reserved;     /* HeapCreate's region */
};

static struct heap process_heap = { .magic = HEAP_MAGIC, .process = true, .serialize = true };

static struct block *next_block(struct block *b) { return (struct block *)((uint8_t *)b + b->size); }

static void corrupt(void)
{
    __asm__ volatile("int $0x30" : : "a"(SYS_FAIL), "b"(FAIL_HEAP_CORRUPT),
                     "c"("a heap block's header was overwritten") : "memory");
    for (;;)
        ;
}

static struct block *checked(struct heap *h, struct block *b)
{
    if (b->magic != BLOCK_MAGIC || b->size < sizeof *b || (b->size & 7) ||
        (uint32_t)((uint8_t *)h->end - (uint8_t *)b) < b->size)
        corrupt();
    return b;
}

static bool grow(struct heap *h, uint32_t need)
{
    uint32_t n = need > GROW ? (need + GROW - 1) / GROW * GROW : GROW;
    struct block *b;
    if (h->process) {
        uint32_t old = sys_sbrk(n);
        if (!old)
            return false;
        b = (struct block *)(uintptr_t)old;
    } else {
        if (n > h->reserved - h->committed)
            n = (h->reserved - h->committed) & ~4095u;
        if (n < need)
            return false;
        struct vmem_args a = { h->base + h->committed, n, VM_PROT_READ | VM_PROT_WRITE, 0, 0, 0, 0 };
        if (vmem(VM_COMMIT, &a) < 0)
            return false;
        b = (struct block *)(uintptr_t)(h->base + h->committed);
        h->committed += n;
    }
    if (!h->first)
        h->first = b;
    b->size = n;
    b->free = 1;
    b->magic = BLOCK_MAGIC;
    struct block *old_end = h->end;
    h->end = next_block(b);
    if (old_end == b)                               /* merge with a free block just before */
        for (struct block *p = h->first; p != b; p = next_block(checked(h, p)))
            if (next_block(p) == b && p->free) {
                p->size += b->size;
                break;
            }
    return true;
}

static void lock(struct heap *h)   { if (h->serialize) EnterCriticalSection(&h->lock); }
static void unlock(struct heap *h) { if (h->serialize) LeaveCriticalSection(&h->lock); }

static struct heap *heap_of(HANDLE hh)
{
    struct heap *h = (struct heap *)(uintptr_t)hh;
    if (h && h->magic == HEAP_MAGIC)
        return h;
    k_error(ERROR_INVALID_HANDLE);
    return NULL;
}

static void *heap_alloc(struct heap *h, uint32_t flags, uint32_t size)
{
    if (size > 0x3FFFFFFF) {
        k_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    uint32_t need = (size + sizeof(struct block) + 7) & ~7u;
    lock(h);
    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block *b = h->first; b && b != h->end; b = next_block(b)) {
            checked(h, b);
            if (!b->free || b->size < need)
                continue;
            if (b->size - need >= 32) {             /* split */
                struct block *rest = (struct block *)((uint8_t *)b + need);
                rest->size = b->size - need;
                rest->free = 1;
                rest->magic = BLOCK_MAGIC;
                b->size = need;
            }
            b->free = 0;
            unlock(h);
            void *p = b + 1;
            if (flags & HEAP_ZERO_MEMORY)
                memset(p, 0, b->size - sizeof *b);
            return p;
        }
        if (!grow(h, need))
            break;
    }
    unlock(h);
    k_error(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

static struct block *block_of(struct heap *h, void *p)
{
    struct block *b = (struct block *)p - 1;
    if ((uint8_t *)b < (uint8_t *)h->first || b >= h->end || b->free)
        corrupt();                                  /* not ours, or freed already */
    return checked(h, b);
}

static bool heap_free(struct heap *h, void *p)
{
    if (!p)
        return true;
    lock(h);
    struct block *b = block_of(h, p);
    b->free = 1;
    while (next_block(b) != h->end && checked(h, next_block(b))->free)
        b->size += next_block(b)->size;             /* with the free ones after it */
    for (struct block *q = h->first; q != b; q = next_block(checked(h, q)))
        if (next_block(q) == b && q->free) {        /* and the one before */
            q->size += b->size;
            break;
        }
    unlock(h);
    return true;
}

static uint32_t heap_size(struct heap *h, void *p)
{
    lock(h);
    uint32_t n = block_of(h, p)->size - sizeof(struct block);
    unlock(h);
    return n;
}

static void *heap_realloc(struct heap *h, uint32_t flags, void *p, uint32_t size)
{
    if (!p)
        return heap_alloc(h, flags, size);
    uint32_t have = heap_size(h, p);
    if (size <= have)
        return p;
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
        k_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    void *q = heap_alloc(h, flags, size);
    if (!q)
        return NULL;
    memcpy(q, p, have);
    heap_free(h, p);
    return q;
}

HANDLE GetProcessHeap(void) { return (HANDLE)(uintptr_t)&process_heap; }

/* A heap in a region of its own: max bytes (0: up to 16 MB), initial
 * committed at once. Its header is at the start of the region. */
HANDLE HeapCreate(uint32_t flags, uint32_t initial, uint32_t max)
{
    uint32_t reserve = max ? (max + 4095) & ~4095u : 16u << 20;
    if (reserve < 8192)
        reserve = 8192;
    struct vmem_args a = { 0, reserve, 0, 0, 0, 0, 0 };
    if (vmem(VM_RESERVE, &a) < 0)
        return 0;
    uint32_t first = initial > 4096 ? (initial + 4095) & ~4095u : 4096;
    struct vmem_args c = { a.base, first, VM_PROT_READ | VM_PROT_WRITE, 0, 0, 0, 0 };
    if (first > reserve || vmem(VM_COMMIT, &c) < 0) {
        struct vmem_args r = { a.base, 0, 0, 0, 0, 0, 0 };
        vmem(VM_RELEASE, &r);
        return 0;
    }
    struct heap *h = (struct heap *)(uintptr_t)a.base;
    memset(h, 0, sizeof *h);
    h->magic = HEAP_MAGIC;
    h->serialize = !(flags & HEAP_NO_SERIALIZE);
    h->base = a.base;
    h->reserved = reserve;
    h->committed = first;
    uint32_t start = (sizeof *h + 7) & ~7u;         /* the rest of the first pages: one free block */
    struct block *b = (struct block *)(uintptr_t)(a.base + start);
    b->size = first - start;
    b->free = 1;
    b->magic = BLOCK_MAGIC;
    h->first = b;
    h->end = next_block(b);
    return (HANDLE)(uintptr_t)h;
}

bool HeapDestroy(HANDLE hh)
{
    struct heap *h = heap_of(hh);
    if (!h || h->process) {
        k_error(ERROR_INVALID_HANDLE);
        return false;
    }
    h->magic = 0;
    struct vmem_args r = { h->base, 0, 0, 0, 0, 0, 0 };
    return vmem(VM_RELEASE, &r) == 0;
}

void *HeapAlloc(HANDLE hh, uint32_t flags, uint32_t size)
{
    struct heap *h = heap_of(hh);
    return h ? heap_alloc(h, flags, size) : NULL;
}

void *HeapReAlloc(HANDLE hh, uint32_t flags, void *p, uint32_t size)
{
    struct heap *h = heap_of(hh);
    return h ? heap_realloc(h, flags, p, size) : NULL;
}

bool HeapFree(HANDLE hh, uint32_t flags, void *p)
{
    (void)flags;
    struct heap *h = heap_of(hh);
    return h && heap_free(h, p);
}

uint32_t HeapSize(HANDLE hh, uint32_t flags, const void *p)
{
    (void)flags;
    struct heap *h = heap_of(hh);
    return h && p ? heap_size(h, (void *)p) : 0xFFFFFFFFu;
}

/* Walk every block: false if one's header is damaged (rather than ending
 * the process, as allocating would). */
bool HeapValidate(HANDLE hh, uint32_t flags, const void *p)
{
    (void)flags; (void)p;
    struct heap *h = heap_of(hh);
    if (!h)
        return false;
    lock(h);
    bool ok = true;
    for (struct block *b = h->first; ok && b && b != h->end; b = next_block(b))
        ok = b->magic == BLOCK_MAGIC && b->size >= sizeof *b && !(b->size & 7) &&
             (uint32_t)((uint8_t *)h->end - (uint8_t *)b) >= b->size;
    unlock(h);
    return ok;
}

/* ---- the process heap, as Win16 and early Win32 called it ------------------------- */

void *LocalAlloc(unsigned flags, uint32_t size)
{
    return heap_alloc(&process_heap, (flags & LMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, size);
}

void *LocalFree(void *p)
{
    heap_free(&process_heap, p);
    return NULL;
}

uint32_t LocalSize(void *p) { return p ? heap_size(&process_heap, p) : 0; }

void *LocalReAlloc(void *p, uint32_t size, unsigned flags)
{
    return heap_realloc(&process_heap, (flags & LMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, p, size);
}
