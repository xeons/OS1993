/*
 * pager.c -- demand paging and the swap file, for OS1993 programs' memory.
 *
 * A program's private memory -- its heap (SYS_SBRK), VirtualAlloc'd pages
 * and thread stacks -- is committed without frames: the page table entry
 * says "demand zero" (PTE_DEMAND, not present), and the first touch faults
 * a zeroed frame in. Such pages are marked swappable (PTE_SWAPPABLE).
 *
 * When free frames run low (below LOW_WATER), a clock hand goes round the
 * processes' page tables: a swappable page that wasn't accessed since the
 * hand last passed is written to the swap file (WIN386.SWP next to
 * VMM.BIN, SwapFile= KB in OS1993.INI's [386Enh]) and its entry says
 * "swapped, slot n" (PTE_SWAPPED); touching it reads it back. Pages of a
 * process with a thread inside a system call are left alone, so a buffer
 * the VMM has checked stays resident while it uses it (user_range_ok and
 * user_writable fault pages in first).
 *
 * Swap I/O goes through the VMM's own file system (fs_read/fs_write on the
 * open swap file), from the faulting thread with interrupts on. With no
 * VMM-owned drive (DOSBox-X), or SwapFile=0, there is no swap file:
 * demand-zero pages still save frames, and memory just runs out sooner.
 */
#include "kernel.h"
#include "fs.h"
#include "io.h"
#include "mm.h"
#include "sched.h"
#include "user.h"
#include "v86.h"

#define LOW_WATER   256                 /* frames: 1 MB kept free for the VMM ... */
#define HIGH_WATER  384                 /* ... evicting until 1.5 MB are */
#define PTE_DEMAND    0x200u            /* not present: a zero page on first touch */
#define PTE_SWAPPED   0x400u            /* not present: in the swap file, slot in 31:12 */
#define PTE_SWAPPABLE 0x800u            /* present: may be written out */

#define PD ((volatile uint32_t *)PD_WINDOW)
#define PT ((volatile uint32_t *)PT_WINDOW)

static struct {
    struct fnode *file;
    uint32_t slots, used;
    uint8_t *map;                       /* a bit per slot */
    uint32_t hand_proc, hand_va;        /* the clock */
    struct thread *busy;                /* holds the pager */
    uint32_t outs, ins, zeros;
} pg;

uint32_t pager_stats(uint32_t *outs, uint32_t *ins, uint32_t *zeros)
{
    if (outs) *outs = pg.outs;
    if (ins) *ins = pg.ins;
    if (zeros) *zeros = pg.zeros;
    return pg.used;
}

bool pager_swap_on(void) { return pg.file != NULL; }

static void lock(void)
{
    for (;;) {
        uint32_t f = irq_save();
        if (!pg.busy || pg.busy == current) {
            pg.busy = current;
            irq_restore(f);
            return;
        }
        cpu_sti();
        sched_yield();
        if (!(f & 0x200)) cpu_cli();
    }
}

static void unlock(void) { pg.busy = NULL; }

/* ---- the swap file -------------------------------------------------------- */

void pager_init(uint32_t kb)
{
    if (!kb)
        return;
    const char *kpath = v86_ptr(v86_buf_seg(), g_bootinfo->kpath_off);
    struct vol *v = fs_volume(kpath[0]);
    if (!v || kpath[1] != ':' || kpath[2] != '\\') {
        klog("PAGER: %c: is DOS's, not the VMM's: no swap file\n", kpath[0]);
        return;
    }
    char path[128];
    size_t dir = 3, n = strlen(kpath);
    for (size_t i = 3; i < n; i++)
        if (kpath[i] == '\\') dir = i + 1;
    if (dir + 11 > sizeof path) return;
    memcpy(path, kpath + 3, dir - 3);
    memcpy(path + dir - 3, "WIN386.SWP", 11);
    struct fnode *f;
    if (fs_open(v, path, CREATE_ALWAYS, true, 0x02 /* hidden */, &f, NULL) < 0) {
        klog("PAGER: cannot create %s\n", path);
        return;
    }
    uint32_t slots = kb / 4;
    if (fs_set_size(f, (uint64_t)slots * PAGE_SIZE) < 0 || !(pg.map = kzalloc((slots + 7) / 8))) {
        klog("PAGER: no room for a %u KB swap file\n", kb);
        fs_close(f);
        fs_delete(v, path);
        return;
    }
    pg.file = f;
    pg.slots = slots;
    klog("PAGER: swap file %c:\\%s, %u KB\n", kpath[0], path, kb);
}

void pager_shutdown(void)
{
    if (pg.file) {
        fs_set_size(pg.file, 0);            /* nothing in it is worth keeping */
        fs_close(pg.file);
        pg.file = NULL;
    }
}

static int slot_alloc(void)
{
    for (uint32_t i = 0; i < pg.slots; i++)
        if (!(pg.map[i / 8] & (1u << (i % 8)))) {
            pg.map[i / 8] |= (uint8_t)(1u << (i % 8));
            pg.used++;
            return (int)i;
        }
    return -1;
}

static void slot_free(uint32_t i)
{
    if (i < pg.slots && (pg.map[i / 8] & (1u << (i % 8)))) {
        pg.map[i / 8] &= (uint8_t)~(1u << (i % 8));
        pg.used--;
    }
}

/* ---- eviction --------------------------------------------------------------- */

/* The page table entry for va in process p's tables (installed or not). */
static volatile uint32_t *pte_of(int proc, uint32_t va)
{
    uint32_t pde = user_proc_pde(proc, va >> 22);
    if (!(pde & PTE_P))
        return NULL;
    if (user_proc_installed(proc))
        return &PT[va >> 12];
    return (volatile uint32_t *)phys_to_virt(pde & PAGE_MASK) + ((va >> 12) & 1023);
}

/* Write one page out; false if there was nothing to take. */
static bool evict_one(void)
{
    if (!pg.file || pg.used >= pg.slots)
        return false;
    for (uint32_t steps = 0; steps < PROC_MAX * 512u * 1024u / 64; steps++) {
        if (pg.hand_va < USER_HEAP || pg.hand_va >= USER_LIMIT) {
            pg.hand_va = USER_HEAP;
            pg.hand_proc = (pg.hand_proc + 1) % PROC_MAX;
        }
        int p = (int)pg.hand_proc;
        if (!user_proc_swappable(p)) {
            pg.hand_va = USER_LIMIT;
            continue;
        }
        uint32_t va = pg.hand_va;
        if (user_proc_pde(p, va >> 22) & PTE_P) {
            pg.hand_va += PAGE_SIZE;
        } else {                                        /* skip a missing page table */
            pg.hand_va = (va & ~0x3FFFFFu) + 0x400000u;
            continue;
        }
        if (va >= USER_SHARED && va < USER_SHARED_END)
            continue;
        volatile uint32_t *e = pte_of(p, va);
        if (!e || (*e & (PTE_P | PTE_SWAPPABLE)) != (PTE_P | PTE_SWAPPABLE))
            continue;
        bool inst = user_proc_installed(p);
        if (*e & PTE_A) {                               /* used lately: a second chance */
            *e &= ~PTE_A;
            if (inst) invlpg(va);
            continue;
        }
        int slot = slot_alloc();
        if (slot < 0)
            return false;
        uint32_t old = *e, frame = old & PAGE_MASK;
        *e = (uint32_t)slot << PAGE_SHIFT | PTE_SWAPPED | (old & (PTE_W | PTE_U));
        if (inst) invlpg(va);
        int rc = fs_write(pg.file, (uint64_t)slot * PAGE_SIZE, phys_to_virt(frame), PAGE_SIZE);
        if (rc != (int)PAGE_SIZE) {                     /* keep it after all */
            *e = old;
            slot_free((uint32_t)slot);
            klog("PAGER: swap file write failed (%d)\n", rc);
            return false;
        }
        pmm_free(frame);
        if (!pg.outs++)
            klog("PAGER: memory is short: writing pages to the swap file\n");
        return true;
    }
    return false;
}

/* Make room before taking frames for a program's memory. */
static void reserve(void)
{
    if (pmm_free_pages() >= LOW_WATER || !pg.file)
        return;
    uint32_t f = read_eflags();
    cpu_sti();
    while (pmm_free_pages() < HIGH_WATER && evict_one())
        ;
    if (!(f & 0x200)) cpu_cli();
}

/* ---- faulting pages in ----------------------------------------------------- */

/*
 * Make the page at va (installed address space) resident. Returns false if
 * it isn't a demand-zero or swapped page, or memory ran out. io: swapped
 * pages may be read (interrupts go on for it).
 */
static bool fault_in(uint32_t va, bool io)
{
    va &= PAGE_MASK;
    if (!(PD[va >> 22] & PTE_P))
        return false;
    volatile uint32_t *e = &PT[va >> 12];
    uint32_t v = *e;
    if (v & PTE_P)
        return true;
    if (!(v & (PTE_DEMAND | PTE_SWAPPED)) || ((v & PTE_SWAPPED) && !io))
        return false;
    lock();
    v = *e;                                             /* (someone else may have done it) */
    if (v & PTE_P) { unlock(); return true; }
    if (io) reserve();
    uint32_t frame = pmm_alloc();
    if (!frame) { unlock(); return false; }
    if (v & PTE_SWAPPED) {
        uint32_t slot = v >> PAGE_SHIFT;
        uint32_t f = read_eflags();
        cpu_sti();
        int rc = fs_read(pg.file, (uint64_t)slot * PAGE_SIZE, phys_to_virt(frame), PAGE_SIZE);
        if (!(f & 0x200)) cpu_cli();
        if (rc != (int)PAGE_SIZE) {
            pmm_free(frame);
            unlock();
            klog("PAGER: swap file read failed (%d) for %08X\n", rc, va);
            return false;
        }
        slot_free(slot);
        if (!pg.ins++)
            klog("PAGER: reading pages back from the swap file\n");
    } else {
        memset(phys_to_virt(frame), 0, PAGE_SIZE);
        pg.zeros++;
    }
    *e = frame | PTE_P | PTE_SWAPPABLE | (v & (PTE_W | PTE_U));
    unlock();
    return true;
}

/* A page fault: one of ours? (from ring 3, or the VMM touching a program's
 * memory) */
bool pager_fault(struct regs *r)
{
    uint32_t cr2 = read_cr2();
    if (r->err_code & 1 || cr2 < USER_BASE || cr2 >= USER_LIMIT || !current || !current->proc)
        return false;
    bool from_user = (r->cs & 3) == 3 && !(r->eflags & EFLAGS_VM);
    bool io = from_user || !fs_locked_by_me();          /* never re-enter the file system */
    return fault_in(cr2, io);
}

/* user_range_ok / user_writable: resident first. */
bool pager_resident(uint32_t va)
{
    if (!(PD[va >> 22] & PTE_P))
        return false;
    if (PT[va >> 12] & PTE_P)
        return true;
    return fault_in(va, !fs_locked_by_me());
}

/* Commit [va, va+len) as demand-zero, swappable pages (installed space). */
bool pager_commit(uint32_t va, uint32_t len, bool writable)
{
    for (uint32_t p = va; p < va + len; p += PAGE_SIZE) {
        if (!(PD[p >> 22] & PTE_P)) {
            uint32_t pt = pmm_alloc();
            if (!pt) return false;
            memset(phys_to_virt(pt), 0, PAGE_SIZE);
            PD[p >> 22] = pt | PTE_P | PTE_W | PTE_U;
        }
        volatile uint32_t *e = &PT[p >> 12];
        if (*e & (PTE_P | PTE_DEMAND | PTE_SWAPPED)) {  /* committed: new protection */
            *e = (*e & ~PTE_W) | (writable ? PTE_W : 0);
            if (*e & PTE_P) invlpg(p);
            continue;
        }
        *e = PTE_DEMAND | PTE_U | (writable ? PTE_W : 0);
    }
    return true;
}

/* Is the page committed (resident, demand-zero or swapped)? writable: and W. */
bool pager_committed(uint32_t va, bool *writable)
{
    if (!(PD[va >> 22] & PTE_P))
        return false;
    uint32_t e = PT[va >> 12];
    if (!(e & (PTE_P | PTE_DEMAND | PTE_SWAPPED)))
        return false;
    if (writable) *writable = e & PTE_W;
    return true;
}

/* Take a page out (installed space): frees its frame or swap slot. */
void pager_release(uint32_t va)
{
    if (!(PD[va >> 22] & PTE_P))
        return;
    volatile uint32_t *e = &PT[va >> 12];
    uint32_t v = *e;
    if (v & PTE_P) {
        *e = 0;
        invlpg(va);
        pmm_free(v & PAGE_MASK);
    } else {
        if (v & PTE_SWAPPED) slot_free(v >> PAGE_SHIFT);
        *e = 0;
    }
}

/* Never write this page out (a TIB: the VMM touches it on the way out). */
void pager_pin(uint32_t va)
{
    if (fault_in(va, true))
        PT[va >> 12] &= ~PTE_SWAPPABLE;
}

/* Before a program's memory is taken from the frames: room first. */
void pager_make_room(void)
{
    lock();
    reserve();
    unlock();
}
