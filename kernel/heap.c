/*
 * heap.c -- kernel heap: first fit with boundary tags.
 *
 * Every block is   [hdr: size|USED, magic][payload ...][footer: size|USED]
 * with sizes a multiple of 8, so payloads are 8-byte aligned. Free blocks
 * keep doubly-linked free-list pointers at the start of their payload.
 * Neighbours are coalesced on free, so two free blocks are never adjacent.
 *
 *   KHEAP_BASE: [pad][prologue footer = USED] [block] [block] ... [epilogue hdr]
 *                                                                 ^ brk - 8
 * The heap grows by mapping fresh frames at `brk`; the old epilogue becomes
 * the header of the new free block.
 */
#include "kernel.h"
#include "io.h"
#include "mm.h"

#define USED        1u
#define HDR_MAGIC   0x4B4F4C42u             /* "BLOK" */
#define HDR_SIZE    8u
#define FTR_SIZE    4u
#define MIN_BLOCK   24u                     /* hdr + 2 links + footer, rounded */
#define GROW_MIN    (16u * 1024u)
#define FREE_POISON 0xDD

struct hdr {
    uint32_t size;                          /* block bytes | USED */
    uint32_t magic;
};

struct free_links {
    struct hdr *next, *prev;
};

static uint32_t brk;                        /* end of mapped heap */
static struct hdr *free_list;

static inline uint32_t bsize(const struct hdr *h)  { return h->size & ~7u; }
static inline bool     bused(const struct hdr *h)  { return h->size & USED; }
static inline uint32_t *footer(struct hdr *h)      { return (uint32_t *)((uint8_t *)h + bsize(h) - FTR_SIZE); }
static inline struct hdr *next_block(struct hdr *h){ return (struct hdr *)((uint8_t *)h + bsize(h)); }
static inline struct free_links *links(struct hdr *h) { return (struct free_links *)(h + 1); }
static inline struct hdr *first_block(void)        { return (struct hdr *)(KHEAP_BASE + 8); }

static void set_block(struct hdr *h, uint32_t size, uint32_t used)
{
    h->size  = size | used;
    h->magic = HDR_MAGIC;
    *footer(h) = size | used;
}

static void list_insert(struct hdr *h)
{
    links(h)->prev = NULL;
    links(h)->next = free_list;
    if (free_list)
        links(free_list)->prev = h;
    free_list = h;
}

static void list_remove(struct hdr *h)
{
    struct free_links *l = links(h);
    if (l->prev) links(l->prev)->next = l->next;
    else         free_list = l->next;
    if (l->next) links(l->next)->prev = l->prev;
}

/* Merge free block h with free neighbours. h must not be on the list. */
static struct hdr *coalesce(struct hdr *h)
{
    struct hdr *n = next_block(h);
    if (!bused(n)) {
        list_remove(n);
        set_block(h, bsize(h) + bsize(n), 0);
    }
    uint32_t prev_ftr = *((uint32_t *)h - 1);
    if (!(prev_ftr & USED)) {
        struct hdr *p = (struct hdr *)((uint8_t *)h - (prev_ftr & ~7u));
        list_remove(p);
        set_block(p, bsize(p) + bsize(h), 0);
        h = p;
    }
    return h;
}

static bool grow(uint32_t need)
{
    uint32_t bytes = PAGE_ALIGN_UP(need > GROW_MIN ? need : GROW_MIN);
    if (bytes > KHEAP_LIMIT - brk)
        return false;

    for (uint32_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint32_t frame = pmm_alloc();
        if (!frame || vmm_map(brk + off, frame, PTE_W) != 0) {
            if (frame) pmm_free(frame);
            while (off) {                   /* roll back this growth */
                off -= PAGE_SIZE;
                pmm_free(vmm_unmap(brk + off));
            }
            return false;
        }
    }

    struct hdr *h = (struct hdr *)(brk - HDR_SIZE);     /* old epilogue */
    brk += bytes;
    set_block(h, bytes, 0);
    struct hdr *epi = next_block(h);
    epi->size  = 0 | USED;
    epi->magic = HDR_MAGIC;
    list_insert(coalesce(h));
    return true;
}

void heap_init(void)
{
    uint32_t frame = pmm_alloc();
    if (!frame || vmm_map(KHEAP_BASE, frame, PTE_W) != 0)
        panic("heap_init: cannot map the first heap page.");
    brk = KHEAP_BASE + PAGE_SIZE;

    uint32_t *base = (uint32_t *)KHEAP_BASE;
    base[0] = 0;
    base[1] = 0 | USED;                     /* prologue: a used "footer" */
    struct hdr *h = first_block();
    set_block(h, PAGE_SIZE - 8 - HDR_SIZE, 0);
    struct hdr *epi = next_block(h);
    epi->size  = 0 | USED;
    epi->magic = HDR_MAGIC;
    free_list = NULL;
    list_insert(h);
}

void *kmalloc(size_t size)
{
    if (size == 0 || size > KHEAP_LIMIT - KHEAP_BASE)
        return NULL;
    uint32_t need = (size + HDR_SIZE + FTR_SIZE + 7) & ~7u;
    if (need < MIN_BLOCK)
        need = MIN_BLOCK;

    uint32_t irq = irq_save();
    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct hdr *h = free_list; h; h = links(h)->next) {
            if (bsize(h) < need)
                continue;
            list_remove(h);
            uint32_t rest = bsize(h) - need;
            if (rest >= MIN_BLOCK) {
                set_block(h, need, USED);
                struct hdr *r = next_block(h);
                set_block(r, rest, 0);
                list_insert(r);
            } else {
                set_block(h, bsize(h), USED);
            }
            irq_restore(irq);
            return h + 1;
        }
        if (!grow(need + HDR_SIZE))
            break;
    }
    irq_restore(irq);
    return NULL;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *p)
{
    if (!p)
        return;
    struct hdr *h = (struct hdr *)p - 1;
    if ((uint32_t)(uintptr_t)p < KHEAP_BASE + 16 || (uint32_t)(uintptr_t)p >= brk ||
        h->magic != HDR_MAGIC)
        panic("kfree: %08X is not a heap block.", (uint32_t)(uintptr_t)p);
    if (!bused(h))
        panic("kfree: block %08X freed twice.", (uint32_t)(uintptr_t)p);
    if (*footer(h) != h->size)
        panic("kfree: block %08X overran its end (footer %08X).",
              (uint32_t)(uintptr_t)p, *footer(h));

    uint32_t irq = irq_save();
    uint32_t size = bsize(h);
    memset(p, FREE_POISON, size - HDR_SIZE - FTR_SIZE);
    set_block(h, size, 0);
    list_insert(coalesce(h));
    irq_restore(irq);
}

static void walk(struct heap_stats *st, bool check)
{
    memset(st, 0, sizeof *st);
    st->mapped = brk - KHEAP_BASE;
    bool prev_free = false;

    for (struct hdr *h = first_block(); ; h = next_block(h)) {
        if ((uint32_t)(uintptr_t)h >= brk || h->magic != HDR_MAGIC)
            panic("heap: bad block header at %08X.", (uint32_t)(uintptr_t)h);
        if (bsize(h) == 0) {
            if (check && (uint32_t)(uintptr_t)h != brk - HDR_SIZE)
                panic("heap: epilogue at %08X, expected %08X.",
                      (uint32_t)(uintptr_t)h, brk - HDR_SIZE);
            break;
        }
        if (check && *footer(h) != h->size)
            panic("heap: block %08X header/footer mismatch.", (uint32_t)(uintptr_t)h);
        if (bused(h)) {
            st->used += bsize(h);
            st->used_blocks++;
            prev_free = false;
        } else {
            if (check && prev_free)
                panic("heap: adjacent free blocks at %08X.", (uint32_t)(uintptr_t)h);
            st->free += bsize(h);
            st->free_blocks++;
            prev_free = true;
        }
    }

    if (check) {
        uint32_t n = 0;
        for (struct hdr *h = free_list; h; h = links(h)->next) {
            if (bused(h) || ++n > st->free_blocks)
                panic("heap: free list corrupt at %08X.", (uint32_t)(uintptr_t)h);
        }
        if (n != st->free_blocks)
            panic("heap: free list has %u blocks, heap has %u.", n, st->free_blocks);
    }
}

void heap_check(void)
{
    struct heap_stats st;
    uint32_t irq = irq_save();
    walk(&st, true);
    irq_restore(irq);
}

void heap_get_stats(struct heap_stats *st)
{
    uint32_t irq = irq_save();
    walk(st, false);
    irq_restore(irq);
}
