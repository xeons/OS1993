/*
 * mmtest.c -- boot-time self-test of the frame allocator, paging and heap.
 * Cheap enough to run on every boot; any failure panics with a reason.
 */
#include "kernel.h"
#include "mm.h"

#define TEST_VA  0xE0000000u
#define NFRAMES  64
#define NALLOCS  200

static uint32_t rng = 0x1993;
static uint32_t rand32(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

#define CHECK(cond, ...) do { if (!(cond)) panic(__VA_ARGS__); } while (0)

static void test_pmm(void)
{
    static uint32_t frames[NFRAMES];
    uint32_t before = pmm_free_pages();

    for (int i = 0; i < NFRAMES; i++) {
        frames[i] = pmm_alloc();
        CHECK(frames[i] && !(frames[i] & ~PAGE_MASK), "pmm: bad frame %08X", frames[i]);
        for (int j = 0; j < i; j++)
            CHECK(frames[j] != frames[i], "pmm: frame %08X handed out twice", frames[i]);
        uint32_t *p = phys_to_virt(frames[i]);
        CHECK(p, "pmm: frame %08X outside the arena", frames[i]);
        for (unsigned k = 0; k < PAGE_SIZE / 4; k += 64)
            p[k] = frames[i] ^ k;
    }
    CHECK(pmm_free_pages() == before - NFRAMES, "pmm: free count off after alloc");
    for (int i = 0; i < NFRAMES; i++) {
        uint32_t *p = phys_to_virt(frames[i]);
        for (unsigned k = 0; k < PAGE_SIZE / 4; k += 64)
            CHECK(p[k] == (frames[i] ^ k), "pmm: frame %08X contents changed", frames[i]);
        pmm_free(frames[i]);
    }
    CHECK(pmm_free_pages() == before, "pmm: free count off after free");
}

static void test_paging(void)
{
    uint32_t frame = pmm_alloc(), pa;
    CHECK(frame, "paging: out of frames");
    CHECK(!vmm_translate(TEST_VA, NULL), "paging: test address already mapped");
    CHECK(vmm_map(TEST_VA, frame, PTE_W) == 0, "paging: vmm_map failed");
    CHECK(vmm_map(TEST_VA, frame, PTE_W) == VMM_EEXIST, "paging: double map not refused");
    CHECK(vmm_translate(TEST_VA + 0x123, &pa) && pa == frame + 0x123,
          "paging: translate gave %08X, expected %08X", pa, frame + 0x123);

    volatile uint32_t *va = (volatile uint32_t *)TEST_VA;
    uint32_t *direct = phys_to_virt(frame);
    va[10] = 0xC0FFEE93;
    CHECK(direct[10] == 0xC0FFEE93, "paging: alias does not see the write");
    direct[11] = 0x12345678;
    CHECK(va[11] == 0x12345678, "paging: mapping does not see the write");

    CHECK(vmm_unmap(TEST_VA) == frame, "paging: unmap returned the wrong frame");
    CHECK(!vmm_translate(TEST_VA, NULL), "paging: still mapped after unmap");
    pmm_free(frame);
}

static void test_heap(void)
{
    static uint8_t *ptrs[NALLOCS];
    static uint32_t sizes[NALLOCS];
    struct heap_stats st0, st;
    heap_get_stats(&st0);

    for (int i = 0; i < NALLOCS; i++) {
        sizes[i] = 1 + rand32() % ((i % 10 == 0) ? 20000 : 300);
        ptrs[i] = kmalloc(sizes[i]);
        CHECK(ptrs[i], "heap: kmalloc(%u) failed", sizes[i]);
        CHECK(!((uint32_t)(uintptr_t)ptrs[i] & 7), "heap: %08X misaligned", (uint32_t)(uintptr_t)ptrs[i]);
        memset(ptrs[i], (uint8_t)i, sizes[i]);
    }
    heap_check();

    for (int i = NALLOCS - 1; i > 0; i--) {     /* free in random order */
        int j = (int)(rand32() % (uint32_t)(i + 1));
        uint8_t *tp = ptrs[i]; ptrs[i] = ptrs[j]; ptrs[j] = tp;
        uint32_t ts = sizes[i]; sizes[i] = sizes[j]; sizes[j] = ts;
    }
    for (int i = 0; i < NALLOCS; i++) {
        uint8_t fill = ptrs[i][0];              /* each block is one byte value */
        for (uint32_t k = 0; k < sizes[i]; k++)
            CHECK(ptrs[i][k] == fill, "heap: block %08X corrupted", (uint32_t)(uintptr_t)ptrs[i]);
        kfree(ptrs[i]);
        if (i % 25 == 0)
            heap_check();
    }
    heap_check();
    heap_get_stats(&st);
    CHECK(st.used_blocks == st0.used_blocks, "heap: %u blocks leaked", st.used_blocks - st0.used_blocks);
    CHECK(st.free_blocks == 1, "heap: %u free blocks left, expected 1 (coalescing)", st.free_blocks);

    void *big = kmalloc(1024 * 1024);           /* forces growth */
    CHECK(big, "heap: 1 MB allocation failed");
    memset(big, 0xA5, 1024 * 1024);
    kfree(big);
    heap_check();
    CHECK(kmalloc(0) == NULL, "heap: kmalloc(0) should return NULL");
}

void mm_selftest(void)
{
    test_pmm();
    test_paging();
    test_heap();
    klog("VMM: memory self-test passed\n");
}
