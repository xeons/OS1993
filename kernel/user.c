/*
 * user.c -- ring-3 processes: address spaces, loading, the INT 30h system
 * calls, waiting and waking, locks, faults.
 *
 * A ring-3 session (the GUI) is a set of processes, each with one thread
 * and its own address space below 2 GB. There is still a single page
 * directory: a process owns the user page-directory entries (1-511), which
 * are swapped in when one of its threads is scheduled -- the trick VMs use
 * for PDE 0 -- so the kernel half is the same everywhere by construction.
 * The shared region (video memory and the shared arena, where UI.DLL keeps
 * the window manager's state) is one page table installed in every process.
 *
 * Programs and DLLs are MOD3 modules (kernel/include/user.h). The VMM
 * console thread loads them through DOS, the System VM's own file system:
 * a process asking for another (SYS_EXEC) queues a request and waits. Each
 * process gets its own copy of every DLL it imports, at the DLL's base.
 *
 * Processes synchronise with SYS_WAIT / SYS_WAKE (a process sleeps until
 * another wakes it, input arrives, or a timeout) and with kernel locks,
 * which are handed to the next waiter and released if their owner dies.
 * The session ends when its first process (the shell) does.
 */
#include "kernel.h"
#include "devices.h"
#include "files.h"
#include "display.h"
#include "gdt.h"
#include "interrupts.h"
#include "io.h"
#include "mm.h"
#include "sched.h"
#include "user.h"
void speaker_beep(unsigned hz, unsigned ms);   /* timer.c */
#include "random.h"
#include "net.h"
#include "fs.h"
#include "v86.h"
#include "vga.h"
#include "vm.h"

#define MAX_IMAGE   (2u * 1024 * 1024)
#define USER_PDES   512                     /* PDEs 0-511 cover user space */
#define SHARED_FIRST (int)(USER_SHARED >> 22)     /* the shared region's PDEs */
#define SHARED_LAST  (int)((USER_SHARED_END - 1) >> 22)
#define SHARED_N     (SHARED_LAST - SHARED_FIRST + 1)

/* A module's image, kept while the session lasts so later processes can
 * share it: the frames of its read-only pages, mapped into each of them
 * read-only, and a pristine copy of its writable pages -- taken after its
 * imports were linked, before any of its code ran -- that each one gets a
 * copy of. (Every DLL sits at a fixed address, so linked import slots hold
 * the same values in every process.) */
#define IMAGE_MAX 16

struct image {
    char      name[16];
    uint32_t  kind, base, data, end, entry, exports, imports, guard;
    uint32_t *code;                         /* frames of [base, data) */
    uint32_t *pristine;                     /* frames: the first contents of [data, end) */
    int       refs;                         /* processes using it */
};

static struct image images[IMAGE_MAX];

struct module {
    char     name[16];
    uint32_t base, end;                     /* mapped [base, end) */
    uint32_t data;                          /* private from here; before, shared */
    uint32_t exports, imports;
    uint32_t guard;                         /* its stack canary, or 0 */
    bool     resolved;
    struct image *img;                      /* shared from this image, or NULL */
};

struct uthread {                            /* a thread of a process */
    struct thread *t;                       /* NULL once it ended and was freed */
    int      tid;                           /* 0: a free slot */
    bool     done, suspended;
    int      exit_code;
    uint32_t region;                        /* its VM region: guard, stack, TIB */
    uint32_t start, esp;                    /* where it enters ring 3 */
};

#define VREGION_MAX 64
struct vregion {                            /* a region of USER_VM */
    uint32_t base, size;
    bool     used;
};

enum pstate { P_FREE, P_RUNNING, P_DEAD };

struct process {
    enum pstate    state;
    int            pid;
    char           name[16];
    struct thread *thread;
    uint32_t      *pde;                     /* user PDEs while not installed */
    struct module  mods[MOD_MAX];           /* mods[0] is the program */
    int            nmods;
    uint32_t       entry;
    int            result;
    bool           killed;
    volatile bool  wake;                    /* a wake-up is pending */
    bool           waiting;                 /* in SYS_WAIT */
    bool           wait_input;              /* ... and input ends it */
    uint32_t       deadline;                /* tick to give up waiting, 0: never */
    int            lock_wait;               /* lock it waits for, -1 none */
    struct proc_files files;                /* open files, current directory */
    uint32_t       heap_end;                /* SYS_SBRK: USER_HEAP .. heap_end */
    uint32_t       shared_pages;            /* mapped from other processes' images */
    struct thread *wait_thread;             /* the thread in SYS_WAIT */
    struct thread *lock_thread;             /* the thread waiting for lock_wait */
    struct uthread th[THREADS_PER_PROC];    /* th[0]: the first; thread = its kernel thread */
    struct vregion vm[VREGION_MAX];
};

void user_enter(uint32_t eip, uint32_t esp, uint32_t ebx);  /* entry.asm */

static struct process procs[PROC_MAX];
static struct process *installed;           /* whose PDEs are in the directory */
static int next_pid = 1;

static struct {
    bool           running;                 /* a session is on */
    int            leader;                  /* its first process */
    int            result;
    uint32_t       flags;
    bool           shared_on;               /* the shared region exists */
    uint32_t       shared_pdes[SHARED_N];   /* ... its page tables */
    uint32_t       prints;                  /* SYS_PRINT calls (self-test) */
    uint32_t       exits;
    volatile int   focus_event;             /* -1 none, else EV_FOCUS key */
    bool           background;              /* VGA ports trapped */
    int            lock_owner[LOCK_MAX];    /* pid, 0 = free */
    volatile bool  cad_event;               /* Ctrl+Alt+Del for the GUI ... */
    volatile uint32_t cad_tick;             /* ... pressed then (0: no question) */
    volatile bool  cad_acked;               /* the shell showed its task list */
    volatile bool  cad_again;               /* pressed on the emergency screen */
    bool           emergency;               /* the VMM's own screen is up */
} u = { .focus_event = -1 };

/* Work only the console thread can do (it owns the System VM's DOS):
 * starting programs and file access. One request at a time. */
enum { REQ_EXEC, REQ_FILE };
static struct {
    volatile bool  pending;
    int            kind;
    char           name[16];                /* REQ_EXEC */
    char           args[ARGS_MAX];
    struct process *proc;                   /* REQ_FILE */
    uint32_t       op;
    struct file_args fa;
    struct thread *waiter;
    int            result;                  /* pid, or the file operation's */
} req;

static void strncpy0(char *d, const char *s, size_t size)
{
    size_t n = 0;
    while (n + 1 < size && s[n]) { d[n] = s[n]; n++; }
    d[n] = '\0';
}

static struct process *proc_of(int pid)
{
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].state == P_RUNNING && procs[i].pid == pid)
            return &procs[i];
    return NULL;
}

static struct process *cur_proc(void) { return current ? current->proc : NULL; }

/* ---- address spaces ------------------------------------------------------ */

/* Install p's user PDEs (scheduler, loader). Kernel threads keep whatever
 * is installed: they never touch user memory. */
void proc_switch(struct process *p)
{
    if (!p || p == installed)
        return;
    volatile uint32_t *pd = (volatile uint32_t *)PD_WINDOW;
    if (installed)
        for (int i = 1; i < USER_PDES; i++)
            installed->pde[i] = pd[i];
    for (int i = 1; i < USER_PDES; i++)
        pd[i] = p->pde[i];
    installed = p;
    tlb_flush();
}

static bool map_zeroed(uint32_t va, uint32_t len)
{
    for (uint32_t p = va; p < va + len; p += PAGE_SIZE) {
        uint32_t frame = pmm_alloc();
        if (!frame || vmm_map(p, frame, PTE_W | PTE_U) != 0) {
            if (frame) pmm_free(frame);
            return false;
        }
        memset((void *)(uintptr_t)p, 0, PAGE_SIZE);
    }
    return true;
}

static void unmap_free(uint32_t va, uint32_t len)
{
    for (uint32_t p = va; p < va + len; p += PAGE_SIZE) {
        uint32_t frame = vmm_unmap(p);
        if (frame)
            pmm_free(frame);
    }
}

/* Free everything p mapped (p must be installed), page tables included. */
static void release_address_space(struct process *p)
{
    for (int i = 0; i < p->nmods; i++) {
        struct module *m = &p->mods[i];
        if (m->img) {                           /* shared pages: the image keeps them */
            for (uint32_t va = m->base; va < m->data; va += PAGE_SIZE)
                vmm_unmap(va);
            m->img->refs--;
            unmap_free(m->data, m->end - m->data);
        } else {
            unmap_free(m->base, m->end - m->base);
        }
    }
    p->nmods = 0;
    unmap_free(USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE);
    unmap_free(USER_INFO, 2 * PAGE_SIZE);
    if (p->heap_end > USER_HEAP)
        unmap_free(USER_HEAP, PAGE_ALIGN_UP(p->heap_end) - USER_HEAP);
    p->heap_end = USER_HEAP;
    for (int i = 0; i < VREGION_MAX; i++)           /* thread stacks, VirtualAlloc */
        if (p->vm[i].used) {
            unmap_free(p->vm[i].base, p->vm[i].size);
            memset(&p->vm[i], 0, sizeof p->vm[i]);
        }
    volatile uint32_t *pd = (volatile uint32_t *)PD_WINDOW;
    for (int i = 1; i < USER_PDES; i++) {
        if (i >= SHARED_FIRST && i <= SHARED_LAST)  /* shared_destroy's business */
            continue;
        if (pd[i] & PTE_P)
            pmm_free(pd[i] & PAGE_MASK);
        pd[i] = 0;
    }
    tlb_flush();
}

/* The shared region, for the whole session: VGA memory, the arena, and
 * the display's back buffer and framebuffer. All its page tables exist
 * from the start, so every process has them and later mappings (a display
 * mode change) show up in all of them at once. */
static bool shared_create(void)
{
    volatile uint32_t *pd = (volatile uint32_t *)PD_WINDOW;
    for (int i = SHARED_FIRST; i <= SHARED_LAST; i++) {
        if (pd[i] & PTE_P)
            continue;
        uint32_t pt = pmm_alloc();
        if (!pt)
            return false;
        memset(phys_to_virt(pt), 0, PAGE_SIZE);
        pd[i] = pt | PTE_P | PTE_W | PTE_U;
    }
    u.shared_on = true;
    for (int i = SHARED_FIRST; i <= SHARED_LAST; i++)
        u.shared_pdes[i - SHARED_FIRST] = pd[i];
    for (uint32_t i = 0; i < 16; i++)
        if (vmm_map(USER_VGA + i * PAGE_SIZE, 0xA0000 + i * PAGE_SIZE, PTE_W | PTE_U) != 0)
            return false;
    return map_zeroed(USER_ARENA, USER_ARENA_SIZE) && display_session_start();
}

static void shared_destroy(void)                /* the last process is installed */
{
    if (!u.shared_on)
        return;
    display_session_end();
    for (uint32_t i = 0; i < 16; i++)
        vmm_unmap(USER_VGA + i * PAGE_SIZE);    /* device memory: don't free */
    unmap_free(USER_ARENA, USER_ARENA_SIZE);
    volatile uint32_t *pd = (volatile uint32_t *)PD_WINDOW;
    for (int i = SHARED_FIRST; i <= SHARED_LAST; i++) {
        if (pd[i] & PTE_P)
            pmm_free(pd[i] & PAGE_MASK);
        pd[i] = 0;
    }
    tlb_flush();
    u.shared_on = false;
}

/* Is [p, p+len) ordinary mapped user memory? */
bool user_range_ok(uint32_t p, uint32_t len)
{
    if (p < USER_BASE || p >= USER_LIMIT || len > USER_LIMIT - p)
        return false;
    for (uint32_t a = PAGE_ALIGN_DOWN(p); a < p + len; a += PAGE_SIZE)
        if (!vmm_translate(a, NULL))
            return false;
    return true;
}

/* ... and writable: the kernel can't write into read-only (shared) pages. */
bool user_writable(uint32_t p, uint32_t len)
{
    if (!user_range_ok(p, len))
        return false;
    for (uint32_t a = PAGE_ALIGN_DOWN(p); a < p + len; a += PAGE_SIZE)
        if (!vmm_writable(a))
            return false;
    return true;
}

bool user_string(uint32_t p, char *out, size_t size)
{
    for (size_t n = 0; n < size; n++) {
        if (!user_range_ok(p + n, 1))
            return false;
        out[n] = *(const char *)(uintptr_t)(p + n);
        if (!out[n])
            return true;
    }
    out[size - 1] = '\0';
    return true;
}

/* ---- loading ------------------------------------------------------------- */

static int dos_open(const char *path)
{
    uint16_t seg = v86_buf_seg(), buf = v86_buf_off();
    memcpy(v86_ptr(seg, buf), path, strlen(path) + 1);
    struct v86_regs r = { .eax = 0x3D00, .edx = buf, .ds = seg };
    if (v86_int(0x21, &r) != 0 || (r.flags & EFLAGS_CF))
        return -1;
    return (int)(r.eax & 0xFFFF);
}

static int dos_read(int h, uint32_t len)        /* into the transfer buffer */
{
    struct v86_regs r = { .eax = 0x3F00, .ebx = (uint32_t)h, .ecx = len,
                          .edx = v86_buf_off(), .ds = v86_buf_seg() };
    if (v86_int(0x21, &r) != 0 || (r.flags & EFLAGS_CF))
        return -1;
    return (int)(r.eax & 0xFFFF);
}

static void dos_close(int h)
{
    struct v86_regs r = { .eax = 0x3E00, .ebx = (uint32_t)h };
    v86_int(0x21, &r);
}

/* "<directory of VMM.BIN>\name" */
static void program_path(const char *name, char *out, size_t size)
{
    const char *kpath = v86_ptr(v86_buf_seg(), g_bootinfo->kpath_off);
    size_t dir = 0, n = strlen(kpath);
    for (size_t i = 0; i < n; i++)
        if (kpath[i] == '\\' || kpath[i] == ':')
            dir = i + 1;
    if (dir + strlen(name) + 1 > size)
        dir = 0;
    memcpy(out, kpath, dir);
    memcpy(out + dir, name, strlen(name) + 1);
}

static bool same_name(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a;
        char y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y) return false;
        if (!x) return true;
    }
}

static bool in_mod(const struct module *m, uint32_t va, uint32_t len)
{
    return va >= m->base && va < m->end && len <= m->end - va;
}

/* A NUL-terminated string inside the module, at most max bytes; else NULL */
static const char *mod_str(const struct module *m, uint32_t va, uint32_t max)
{
    for (uint32_t n = 0; n < max; n++) {
        if (!in_mod(m, va + n, 1))
            return NULL;
        if (*(const char *)(uintptr_t)(va + n) == '\0')
            return (const char *)(uintptr_t)va;
    }
    return NULL;
}

static struct module *find_module(struct process *p, const char *name)
{
    for (int i = 0; i < p->nmods; i++)
        if (same_name(p->mods[i].name, name))
            return &p->mods[i];
    return NULL;
}

static struct image *find_image(const char *name, uint32_t kind)
{
    for (int i = 0; i < IMAGE_MAX; i++)
        if (images[i].code && images[i].kind == kind && same_name(images[i].name, name))
            return &images[i];
    return NULL;
}

/* Map a cached image: its read-only pages shared, its writable ones copied. */
static struct module *map_image(struct process *p, struct image *img)
{
    for (int i = 0; i < p->nmods; i++)              /* no overlaps */
        if (!(img->end <= p->mods[i].base || img->base >= p->mods[i].end))
            return NULL;
    struct module *m = &p->mods[p->nmods];
    memset(m, 0, sizeof *m);
    m->base = img->base;
    m->data = img->data;
    m->end = img->data;                             /* private pages mapped so far */
    m->img = img;                                   /* release_address_space knows */
    img->refs++;
    p->nmods++;
    for (uint32_t va = img->base; va < img->data; va += PAGE_SIZE)
        if (vmm_map(va, img->code[(va - img->base) >> PAGE_SHIFT], PTE_U) != 0)
            return NULL;                            /* read-only for ring 3 */
    p->shared_pages += (img->data - img->base) >> PAGE_SHIFT;
    for (uint32_t va = img->data; va < img->end; va += PAGE_SIZE) {
        uint32_t frame = pmm_alloc();
        if (!frame || vmm_map(va, frame, PTE_W | PTE_U) != 0) {
            if (frame) pmm_free(frame);
            return NULL;
        }
        memcpy(phys_to_virt(frame), phys_to_virt(img->pristine[(va - img->data) >> PAGE_SHIFT]),
               PAGE_SIZE);
        m->end = va + PAGE_SIZE;
    }
    strncpy0(m->name, img->name, sizeof m->name);
    m->exports = img->exports;
    m->imports = img->imports;
    m->guard = img->guard;
    if (img->kind == MOD_PROGRAM)
        p->entry = img->entry;
    return m;
}

/* After a process loaded and linked: keep each module it read from disk
 * as an image, its read-only pages becoming shared (and read-only). */
static void keep_images(struct process *p)
{
    for (int i = 0; i < p->nmods; i++) {
        struct module *m = &p->mods[i];
        struct image *img = NULL;
        if (m->img || find_image(m->name, i == 0 ? MOD_PROGRAM : MOD_LIBRARY))
            continue;
        for (int k = 0; k < IMAGE_MAX && !img; k++)
            if (!images[k].code)
                img = &images[k];
        if (!img)
            return;                                 /* the cache is full: stays private */
        uint32_t ncode = (m->data - m->base) >> PAGE_SHIFT, ndata = (m->end - m->data) >> PAGE_SHIFT;
        uint32_t *code = kzalloc((ncode + 1) * sizeof(uint32_t));
        uint32_t *pristine = kzalloc((ndata + 1) * sizeof(uint32_t));
        bool ok = code && pristine;
        for (uint32_t k = 0; ok && k < ndata; k++) {
            uint32_t pa, frame = pmm_alloc();
            if (!frame || !vmm_translate(m->data + (k << PAGE_SHIFT), &pa)) {
                if (frame) pmm_free(frame);
                ok = false;
                break;
            }
            memcpy(phys_to_virt(frame), (const void *)(uintptr_t)(m->data + (k << PAGE_SHIFT)), PAGE_SIZE);
            pristine[k] = frame;
        }
        if (!ok) {
            for (uint32_t k = 0; pristine && k < ndata; k++)
                if (pristine[k]) pmm_free(pristine[k]);
            kfree(code);
            kfree(pristine);
            return;
        }
        for (uint32_t k = 0; k < ncode; k++) {      /* now shared: read-only */
            uint32_t va = m->base + (k << PAGE_SHIFT);
            code[k] = vmm_unmap(va);
            vmm_map(va, code[k], PTE_U);
        }
        strncpy0(img->name, m->name, sizeof img->name);
        img->kind = i == 0 ? MOD_PROGRAM : MOD_LIBRARY;
        img->base = m->base;
        img->data = m->data;
        img->end = m->end;
        img->entry = i == 0 ? p->entry : 0;
        img->exports = m->exports;
        img->imports = m->imports;
        img->guard = m->guard;
        img->pristine = pristine;
        img->code = code;
        img->refs = 1;
        m->img = img;
    }
}

/* At the end of a session: images nobody uses any more go. */
static void free_images(void)
{
    for (int i = 0; i < IMAGE_MAX; i++) {
        struct image *img = &images[i];
        if (!img->code || img->refs > 0)
            continue;
        for (uint32_t k = 0; k < (img->data - img->base) >> PAGE_SHIFT; k++)
            pmm_free(img->code[k]);
        for (uint32_t k = 0; k < (img->end - img->data) >> PAGE_SHIFT; k++)
            pmm_free(img->pristine[k]);
        kfree(img->code);
        kfree(img->pristine);
        memset(img, 0, sizeof *img);
    }
}

/* Read a module file into fresh user pages at its own base address (or
 * map it from an image another process loaded). */
static struct module *load_module(struct process *p, const char *name, uint32_t kind)
{
    if (p->nmods == MOD_MAX) {
        klog("USER: too many modules loading %s\n", name);
        return NULL;
    }
    struct image *img = find_image(name, kind);
    if (img)
        return map_image(p, img);
    char path[128];
    program_path(name, path, sizeof path);
    int h = dos_open(path);
    if (h < 0) {
        klog("USER: cannot open %s\n", path);
        return NULL;
    }

    uint32_t chunk = v86_buf_size() & ~0xFFFu;
    const uint8_t *buf = v86_ptr(v86_buf_seg(), v86_buf_off());
    int n = dos_read(h, chunk);
    struct mod_header hdr;
    memcpy(&hdr, buf, sizeof hdr);
    uint32_t end = hdr.base + PAGE_ALIGN_UP(hdr.mem_size);
    bool ok = n >= (int)sizeof hdr && hdr.magic == MOD_MAGIC && hdr.kind == kind &&
              hdr.mem_size >= hdr.file_size && hdr.mem_size <= MAX_IMAGE &&
              !(hdr.base & ~PAGE_MASK) && !(hdr.data & ~PAGE_MASK) &&
              hdr.data >= hdr.base && hdr.data <= end;
    if (ok && kind == MOD_PROGRAM)
        ok = hdr.base == USER_BASE && hdr.entry >= hdr.base && hdr.entry < hdr.base + hdr.file_size;
    if (ok && kind == MOD_LIBRARY)
        ok = hdr.base >= DLL_BASE && end <= DLL_LIMIT;
    for (int i = 0; ok && i < p->nmods; i++)         /* no overlaps */
        ok = end <= p->mods[i].base || hdr.base >= p->mods[i].end;
    if (!ok) {
        klog("USER: %s is not a valid %s module (or overlaps another)\n", path,
             kind == MOD_PROGRAM ? "program" : "library");
        dos_close(h);
        return NULL;
    }

    struct module *m = &p->mods[p->nmods];
    memset(m, 0, sizeof *m);
    m->base = hdr.base;
    m->end = end;
    m->data = hdr.data;
    if (!map_zeroed(m->base, m->end - m->base)) {
        unmap_free(m->base, m->end - m->base);
        dos_close(h);
        klog("USER: out of memory loading %s\n", path);
        return NULL;
    }
    p->nmods++;

    uint32_t loaded = 0;
    while (n > 0 && loaded < hdr.file_size) {
        uint32_t take = (uint32_t)n;
        if (take > hdr.file_size - loaded)
            take = hdr.file_size - loaded;
        memcpy((void *)(uintptr_t)(m->base + loaded), buf, take);
        loaded += take;
        n = dos_read(h, chunk);
    }
    dos_close(h);
    if (loaded != hdr.file_size) {
        klog("USER: short read on %s (%u of %u bytes)\n", path, loaded, hdr.file_size);
        return NULL;
    }
    memcpy(m->name, name, strlen(name) < sizeof m->name ? strlen(name) + 1 : sizeof m->name - 1);
    m->exports = hdr.exports;
    m->imports = hdr.imports;
    m->guard = hdr.guard >= m->data && hdr.guard <= m->end - 4 && !(hdr.guard & 3) ? hdr.guard : 0;
    if (kind == MOD_PROGRAM)
        p->entry = hdr.entry;
    return m;
}

static uint32_t find_export(const struct module *dll, const char *name)
{
    if (!dll->exports || !in_mod(dll, dll->exports, 4))
        return 0;
    const struct mod_exports *ex = (const void *)(uintptr_t)dll->exports;
    if (!in_mod(dll, dll->exports + 4, ex->count * sizeof(struct mod_export)))
        return 0;
    for (uint32_t i = 0; i < ex->count; i++) {
        const char *s = mod_str(dll, ex->e[i].name, 64);
        if (s && same_name(s, name) && in_mod(dll, ex->e[i].addr, 1))
            return ex->e[i].addr;
    }
    return 0;
}

/* Load (if needed) and link every DLL module m imports. */
static bool resolve(struct process *p, struct module *m, int depth)
{
    if (m->resolved)
        return true;
    m->resolved = true;                             /* breaks import cycles */
    if (!m->imports)
        return true;
    if (depth > MOD_MAX) {
        klog("USER: imports nested too deeply in %s\n", m->name);
        return false;
    }
    for (uint32_t d = m->imports;; d += sizeof(struct mod_import)) {
        if (!in_mod(m, d, sizeof(struct mod_import)))
            return false;
        const struct mod_import *imp = (const void *)(uintptr_t)d;
        if (!imp->module)
            break;
        const char *dll_name = mod_str(m, imp->module, 16);
        if (!dll_name)
            return false;
        struct module *dll = find_module(p, dll_name);
        if (!dll && !(dll = load_module(p, dll_name, MOD_LIBRARY)))
            return false;
        if (!resolve(p, dll, depth + 1))
            return false;
        for (uint32_t k = 0;; k++) {
            uint32_t np = imp->names + 4 * k, sp = imp->slots + 4 * k;
            if (!in_mod(m, np, 4))
                return false;
            uint32_t name_va = *(const uint32_t *)(uintptr_t)np;
            if (!name_va)
                break;
            const char *fn = mod_str(m, name_va, 64);
            uint32_t addr = fn ? find_export(dll, fn) : 0;
            if (!addr || !in_mod(m, sp, 4)) {
                klog("USER: %s imports %s from %s, which doesn't export it\n",
                     m->name, fn ? fn : "?", dll->name);
                return false;
            }
            *(uint32_t *)(uintptr_t)sp = addr;
        }
    }
    return true;
}

static void user_thread_main(void *arg)
{
    struct process *p = arg;
    if (p->killed)
        thread_exit();
    user_enter(p->entry, USER_STACK_TOP, USER_INFO);
}

/* ---- virtual memory regions (VirtualAlloc, thread stacks) ------------------ */

/* USER_VM is handed out in regions, 64 KB aligned, reserved first and then
 * committed page by page (a committed page is a mapped one). With no
 * demand paging, committing allocates the frames at once; a reserved page
 * that isn't committed faults -- a thread stack's guard page is one. */

static struct vregion *vm_region(struct process *p, uint32_t addr)
{
    for (int i = 0; i < VREGION_MAX; i++)
        if (p->vm[i].used && addr >= p->vm[i].base && addr - p->vm[i].base < p->vm[i].size)
            return &p->vm[i];
    return NULL;
}

/* -> the base of a new region of size bytes at addr (0: the lowest free
 * place), or 0. */
static uint32_t vm_reserve(struct process *p, uint32_t addr, uint32_t size)
{
    size = PAGE_ALIGN_UP(size);
    if (!size || size > USER_VM_LIMIT - USER_VM)
        return 0;
    struct vregion *slot = NULL;
    for (int i = 0; i < VREGION_MAX && !slot; i++)
        if (!p->vm[i].used)
            slot = &p->vm[i];
    if (!slot)
        return 0;
    uint32_t base = addr ? addr & ~(VM_GRANULE - 1) : USER_VM;
    for (;;) {
        if (base < USER_VM || base > USER_VM_LIMIT - size)
            return 0;
        uint32_t clash = 0;
        for (int i = 0; i < VREGION_MAX; i++) {
            const struct vregion *r = &p->vm[i];
            if (r->used && base < r->base + r->size && r->base < base + size && r->base + r->size > clash)
                clash = r->base + r->size;
        }
        if (!clash)
            break;
        if (addr)
            return 0;                               /* the place asked for is taken */
        base = (clash + VM_GRANULE - 1) & ~(VM_GRANULE - 1);
    }
    *slot = (struct vregion){ .base = base, .size = size, .used = true };
    return base;
}

static uint32_t pte_for(uint32_t prot) { return PTE_U | ((prot & VM_PROT_WRITE) ? PTE_W : 0); }

/* Commit [addr, addr + size), or give committed pages the new protection. */
static int vm_commit(uint32_t addr, uint32_t size, uint32_t prot)
{
    for (uint32_t va = addr; va - addr < size; va += PAGE_SIZE) {
        uint32_t pa;
        if (vmm_translate(va, &pa)) {
            vmm_unmap(va);
            vmm_map(va, pa & PAGE_MASK, pte_for(prot));
            continue;
        }
        uint32_t frame = pmm_alloc();
        if (!frame || vmm_map(va, frame, PTE_W | PTE_U) != 0) {
            if (frame)
                pmm_free(frame);
            return -ERROR_NOT_ENOUGH_MEMORY;
        }
        memset((void *)(uintptr_t)va, 0, PAGE_SIZE);
        if (!(prot & VM_PROT_WRITE)) {
            vmm_unmap(va);
            vmm_map(va, frame, pte_for(prot));
        }
    }
    return 0;
}

static void vm_release(struct process *p, uint32_t base)
{
    struct vregion *r = vm_region(p, base);
    if (!r)
        return;
    unmap_free(r->base, r->size);
    memset(r, 0, sizeof *r);
}

static uint32_t vm_committed_pages(struct process *p)
{
    uint32_t n = 0;
    for (int i = 0; i < VREGION_MAX; i++)
        for (uint32_t va = p->vm[i].base; p->vm[i].used && va - p->vm[i].base < p->vm[i].size; va += PAGE_SIZE)
            n += vmm_translate(va, NULL);
    return n;
}

/* SYS_VMEM (p installed) -> 0, or -ERROR_* */
static int vm_op(struct process *p, uint32_t op, struct vmem_args *a)
{
    uint32_t addr = a->addr, size = a->size;
    struct vregion *r;
    switch (op) {
    case VM_RESERVE:
    case VM_COMMIT:
        if (!size || size > USER_VM_LIMIT - USER_VM)
            return -ERROR_INVALID_PARAMETER;
        if (op == VM_RESERVE || !addr) {
            uint32_t base = vm_reserve(p, addr, size);
            if (!base)
                return addr ? -ERROR_INVALID_ADDRESS : -ERROR_NOT_ENOUGH_MEMORY;
            a->base = base;
            a->region_size = PAGE_ALIGN_UP(size);
            if (op == VM_RESERVE)
                return 0;
            addr = base;
        }
        r = vm_region(p, addr);
        size = PAGE_ALIGN_UP(addr + size) - PAGE_ALIGN_DOWN(addr);
        addr = PAGE_ALIGN_DOWN(addr);
        if (!r || size > r->base + r->size - addr)
            return -ERROR_INVALID_ADDRESS;
        a->base = addr;
        a->region_size = size;
        return vm_commit(addr, size, a->prot ? a->prot : VM_PROT_READ | VM_PROT_WRITE);
    case VM_DECOMMIT:
    case VM_PROTECT:
        r = vm_region(p, addr);
        size = PAGE_ALIGN_UP(addr + size) - PAGE_ALIGN_DOWN(addr);
        addr = PAGE_ALIGN_DOWN(addr);
        if (!r || !a->size || size > r->base + r->size - addr)
            return -ERROR_INVALID_ADDRESS;
        if (op == VM_DECOMMIT) {
            unmap_free(addr, size);
            return 0;
        }
        for (uint32_t va = addr; va - addr < size; va += PAGE_SIZE)
            if (!vmm_translate(va, NULL))
                return -ERROR_INVALID_ADDRESS;      /* only committed pages */
        a->old_prot = VM_PROT_READ | (vmm_writable(addr) ? VM_PROT_WRITE : 0);
        return vm_commit(addr, size, a->prot);
    case VM_RELEASE:
        r = vm_region(p, addr);
        if (!r || r->base != addr)
            return -ERROR_INVALID_ADDRESS;
        for (int i = 0; i < THREADS_PER_PROC; i++)  /* not a thread's stack */
            if (p->th[i].t && p->th[i].region == addr)
                return -ERROR_INVALID_ADDRESS;
        vm_release(p, addr);
        return 0;
    case VM_QUERY: {
        uint32_t page = PAGE_ALIGN_DOWN(addr);
        if (page < USER_VM || page >= USER_VM_LIMIT)
            return -ERROR_INVALID_ADDRESS;
        r = vm_region(p, page);
        if (!r) {                                   /* free: up to the next region */
            uint32_t end = USER_VM_LIMIT;
            for (int i = 0; i < VREGION_MAX; i++)
                if (p->vm[i].used && p->vm[i].base > page && p->vm[i].base < end)
                    end = p->vm[i].base;
            a->base = page;
            a->region_size = end - page;
            a->state = VM_STATE_FREE;
            a->old_prot = 0;
            return 0;
        }
        bool committed = vmm_translate(page, NULL), writable = committed && vmm_writable(page);
        uint32_t va = page;
        while (va - r->base < r->size && vmm_translate(va, NULL) == committed &&
               (!committed || vmm_writable(va) == writable))
            va += PAGE_SIZE;
        a->base = page;
        a->region_size = va - page;
        a->state = committed ? VM_STATE_COMMIT : VM_STATE_RESERVE;
        a->old_prot = committed ? VM_PROT_READ | (writable ? VM_PROT_WRITE : 0) : 0;
        return 0;
    }
    }
    return -ERROR_INVALID_PARAMETER;
}

/* ---- threads -------------------------------------------------------------- */

static int next_tid = 1;

static void user_thread_start(void *arg)            /* a new thread's first kernel code */
{
    struct uthread *ut = arg;
    struct process *p = current->proc;
    if (p->killed || p->state != P_RUNNING || current->killed) {
        ut->done = true;                            /* ended before it began */
        thread_exit();
    }
    user_enter(ut->start, ut->esp, 0);
}

/* A new thread of p (installed): a region for its stack (a guard page
 * below it) and its TIB, and a kernel thread, created stopped. The first
 * thread uses the fixed stack at USER_STACK_TOP. -> its slot, or -ERROR_* */
static int thread_new(struct process *p, const struct thread_args *a, bool first)
{
    int slot = -1;
    for (int i = 0; i < THREADS_PER_PROC && slot < 0; i++)
        if (!p->th[i].t && (!p->th[i].tid || p->th[i].done))
            slot = i;
    if (slot < 0)
        return -ERROR_TOO_MANY_THREADS;
    uint32_t stack = first ? 0 : PAGE_ALIGN_UP(a->stack_size ? a->stack_size : THREAD_STACK_DEFAULT);
    if (stack > THREAD_STACK_MAX)
        return -ERROR_INVALID_PARAMETER;
    uint32_t size = first ? PAGE_SIZE : PAGE_SIZE + stack + PAGE_SIZE;
    uint32_t base = vm_reserve(p, 0, size);
    if (!base)
        return -ERROR_NOT_ENOUGH_MEMORY;
    uint32_t from = first ? base : base + PAGE_SIZE;
    if (vm_commit(from, base + size - from, VM_PROT_READ | VM_PROT_WRITE) < 0) {
        vm_release(p, base);
        return -ERROR_NOT_ENOUGH_MEMORY;
    }
    struct uthread *ut = &p->th[slot];
    memset(ut, 0, sizeof *ut);
    ut->region = base;
    struct thread *t = first ? thread_create(p->name, user_thread_main, p, NULL)
                             : thread_create(p->name, user_thread_start, ut, NULL);
    if (!t) {
        vm_release(p, base);
        ut->region = 0;
        return -ERROR_TOO_MANY_THREADS;
    }
    ut->tid = next_tid++;
    uint32_t tib = base + size - PAGE_SIZE;
    struct user_tib *tb = (struct user_tib *)(uintptr_t)tib;
    tb->self = tib;
    tb->tid = (uint32_t)ut->tid;
    tb->pid = (uint32_t)p->pid;
    tb->flags = first ? TIB_MAIN : 0;
    if (first) {
        tb->stack_base = USER_STACK_TOP;
        tb->stack_limit = USER_STACK_TOP - USER_STACK_SIZE;
    } else {
        tb->stack_base = tib;
        tb->stack_limit = base + PAGE_SIZE;
        uint32_t *sp = (uint32_t *)(uintptr_t)tib;  /* the start code finds fn, arg */
        *--sp = a->arg;
        *--sp = a->fn;
        ut->esp = (uint32_t)(uintptr_t)sp;
        ut->start = a->start;
    }
    t->user = true;
    t->proc = p;
    t->tib = tib;
    t->uth = slot;
    ut->t = t;
    return slot;
}

static struct uthread *uthread_of(struct process *p, int tid)
{
    for (int i = 0; tid > 0 && i < THREADS_PER_PROC; i++)
        if (p->th[i].tid == tid)
            return &p->th[i];
    return NULL;
}

static void wake_threads(struct process *p)
{
    for (int i = 0; i < THREADS_PER_PROC; i++)
        if (p->th[i].t)
            thread_wake(p->th[i].t);
}

/* Futex keys: a user address, or one of these (kernel addresses, so they
 * can't clash) for waiting on a thread or a process to end. */
#define KEY_THREAD(tid) (0x80000000u | (uint32_t)(tid))
#define KEY_PROC(pid)   (0xC0000000u | (uint32_t)(pid))

static int futex_wake(struct process *p, uint32_t key, int n)
{
    int woke = 0;
    for (int i = 0; i < THREADS_PER_PROC && woke < n; i++) {
        struct thread *t = p->th[i].t;
        if (t && t->futex == key && !t->woken) {
            t->woken = true;
            thread_wake(t);
            woke++;
        }
    }
    return woke;
}

static bool must_stop(struct process *p)
{
    return p->killed || p->state != P_RUNNING || current->killed;
}

bool user_thread_stopping(void)                     /* the network stack's waits */
{
    struct process *p = cur_proc();
    return p && must_stop(p);
}

/* Block the calling thread on key until a wake, ms milliseconds, or the
 * end of the thread or its process. -> true if woken. */
static bool futex_block(struct process *p, uint32_t key, uint32_t ms)
{
    if (ms != WAIT_FOREVER && ms > 0x7FFFFFF)
        ms = 0x7FFFFFF;
    current->futex = key;
    current->woken = false;
    current->deadline = ms == WAIT_FOREVER ? 0 : (timer_ticks() + (ms * 10 + 548) / 549) | 1;
    while (!current->woken && !must_stop(p)) {
        if (current->deadline && (int32_t)(timer_ticks() - current->deadline) >= 0)
            break;
        current->state = T_BLOCKED;
        schedule();
    }
    bool woken = current->woken;
    current->futex = 0;
    current->woken = false;
    current->deadline = 0;
    return woken;
}

/* Recent ends, for SYS_PROC_WAIT after the process is gone. */
static struct { int pid, code; } ended[16];
static unsigned ended_next;

/* Create a process running program `name` (console thread only: loading
 * goes through DOS). -> its pid, or -1. */
static int spawn(const char *name, const char *args)
{
    struct process *p = NULL;
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].state == P_FREE) { p = &procs[i]; break; }
    if (!p) {
        klog("USER: too many processes to start %s\n", name);
        return -1;
    }
    uint32_t *pde = kzalloc(USER_PDES * sizeof(uint32_t));
    if (!pde)
        return -1;
    memset(p, 0, sizeof *p);
    p->pde = pde;
    p->pid = next_pid++;
    p->lock_wait = -1;
    p->heap_end = USER_HEAP;
    for (int i = SHARED_FIRST; i <= SHARED_LAST; i++)   /* none for the first one */
        p->pde[i] = u.shared_on ? u.shared_pdes[i - SHARED_FIRST] : 0;

    struct thread *self = current;                  /* load into p's space */
    struct process *was = self->proc;
    self->proc = p;
    proc_switch(p);
    bool first = !u.shared_on;
    struct module *prog = load_module(p, name, MOD_PROGRAM);
    bool ok = prog && resolve(p, prog, 0) &&
              map_zeroed(USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE) &&
              map_zeroed(USER_INFO, 2 * PAGE_SIZE) &&
              (!first || !(u.flags & UINFO_GRAPHICS) || shared_create());
    if (ok) {
        struct user_info *info = (struct user_info *)(uintptr_t)USER_INFO;
        info->flags = u.flags;
        info->width = VGA_GFX_W;
        info->height = VGA_GFX_H;
        info->font = USER_INFO + PAGE_SIZE;
        info->pid = (uint32_t)p->pid;
        memcpy((void *)(uintptr_t)info->font, vga_font8x16(), 256 * 16);
        if (u.shared_on) {
            info->vga = USER_VGA;
            info->arena = USER_ARENA;
            info->arena_size = USER_ARENA_SIZE;
        }
        strncpy0(info->name, name, sizeof info->name);
        char dir[128];                              /* the OS1993 directory */
        program_path("", dir, sizeof dir);
        size_t dn = strlen(dir);
        if (dn > 3 && dir[dn - 1] == '\\')
            dir[dn - 1] = '\0';
        files_init(&p->files, dir);
        strncpy0(info->args, args ? args : "", sizeof info->args);
        strncpy0(p->name, name, sizeof p->name);
        for (char *c = p->name; *c; c++)            /* DOS names: upper case */
            if (*c >= 'a' && *c <= 'z')
                *c = (char)(*c - 32);
        int slot = thread_new(p, NULL, true);      /* the first thread, and its TIB */
        ok = slot >= 0;
        if (ok)
            p->thread = p->th[slot].t;
        if (ok)
            keep_images(p);                         /* before any of its code runs */
        for (int i = 0; ok && i < p->nmods; i++)    /* then: canaries of its own */
            if (p->mods[i].guard)
                *(uint32_t *)(uintptr_t)p->mods[i].guard = random_u32() & ~0xFFu;
    }
    if (!ok) {
        klog("USER: could not start %s\n", name);
        release_address_space(p);
        if (first)
            shared_destroy();
        self->proc = was;
        installed = NULL;
        kfree(p->pde);
        memset(p, 0, sizeof *p);
        return -1;
    }
    p->thread->user = true;
    p->thread->proc = p;
    p->state = P_RUNNING;
    self->proc = was;                               /* stays installed (lazy) */
    klog("USER: started %s as process %d (%d modules, %u KB shared)\n", p->name, p->pid, p->nmods,
         p->shared_pages * 4);
    thread_wake(p->thread);
    return p->pid;
}

/* ---- the session ------------------------------------------------------------ */

int user_start(const char *name, uint32_t flags)
{
    if (u.running)
        return -1;
    u.flags = flags;
    u.result = 0;
    u.prints = 0;
    u.focus_event = -1;
    u.background = false;
    memset(u.lock_owner, 0, sizeof u.lock_owner);
    int pid = spawn(name, "");
    if (pid < 0)
        return -1;
    u.leader = pid;
    u.running = true;
    mouse_set_delivery((flags & UINFO_GRAPHICS) != 0);
    return 0;
}

bool user_running(void) { return u.running; }
int  user_result(void)  { return u.result; }

static void wake_proc(struct process *p)
{
    p->wake = true;
    if (p->waiting && p->wait_thread)
        thread_wake(p->wait_thread);
}

/* Give lock n to its next waiter, if any. Interrupts off. */
static void lock_handoff(int n)
{
    u.lock_owner[n] = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = &procs[i];
        if (p->state == P_RUNNING && p->lock_wait == n) {
            p->lock_wait = -1;
            u.lock_owner[n] = p->pid;
            thread_wake(p->lock_thread);
            return;
        }
    }
}

/* ---- Ctrl+Alt+Del ------------------------------------------------------------ */

/* Keyboard IRQ. The GUI gets EV_CAD and the shell answers by showing its
 * task list; if it hasn't within two seconds, the window system is stuck
 * (a program holding its lock in a loop) and the VMM steps in. */
bool user_cad(void)
{
    if (!u.running || !(u.flags & UINFO_GRAPHICS))
        return false;
    if (u.emergency) {
        u.cad_again = true;
        return true;
    }
    if (vm_focus())
        vm_set_focus(NULL);                         /* the GUI to the front */
    u.cad_event = true;
    u.cad_acked = false;
    u.cad_tick = timer_ticks() | 1;
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].state == P_RUNNING)
            wake_proc(&procs[i]);
    return true;
}

static void kill_proc(struct process *p)
{
    klog("VMM: ending %s (process %d)\n", p->name, p->pid);
    p->killed = true;
    p->lock_wait = -1;
    wake_threads(p);                                /* ends on its way to ring 3 */
}

#define EM_ATTR  0x1F                               /* white on blue */
#define EM_HI    0x71                               /* blue on grey */

static void em_text(int row, int col, uint8_t attr, const char *s)
{
    volatile uint8_t *v = KLOW(0xB8000);
    for (; *s && col < 80; s++, col++) {
        v[(row * 80 + col) * 2] = (uint8_t)*s;
        v[(row * 80 + col) * 2 + 1] = attr;
    }
}

/* The VMM's own screen, in text mode over the GUI, as Windows 3.1 had for
 * a program that stopped responding. Console thread. */
static void emergency(void)
{
    klog("VMM: Ctrl+Alt+Del: the window system is not responding\n");
    u.emergency = true;
    u.cad_again = false;
    user_screen(false);                             /* GUI: hands off the VGA */
    u.focus_event = -1;
    vga_console_suspend();                          /* save the desktop; text mode */
    while (keyboard_read() >= 0)
        ;
    int sel = 0, n = 0;
    struct process *list[PROC_MAX];
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].state == P_RUNNING && !procs[i].killed) {
            if (u.lock_owner[0] == procs[i].pid)
                sel = n;
            list[n++] = &procs[i];
        }
    bool done = false;
    while (!done && u.running) {
        volatile uint8_t *v = KLOW(0xB8000);
        for (int i = 0; i < 80 * 25; i++) { v[i * 2] = ' '; v[i * 2 + 1] = EM_ATTR; }
        em_text(4, 36, EM_HI, " OS1993 ");
        em_text(6, 6, EM_ATTR, "The window system is not responding. The program holding it is");
        em_text(7, 6, EM_ATTR, "marked; it may be busy, or stuck. Programs running:");
        for (int i = 0; i < n; i++) {
            char line[48];
            ksnprintf(line, sizeof line, " %c %-12s %s%s ", i == sel ? '>' : ' ', list[i]->name,
                      list[i]->pid == u.leader ? "(shell) " : "",
                      u.lock_owner[0] == list[i]->pid ? "(holds the window system)" : "");
            em_text(9 + i, 10, i == sel ? EM_HI : EM_ATTR, line);
        }
        em_text(18, 6, EM_ATTR, "*  Up and Down choose a program; ENTER ends it.");
        em_text(19, 6, EM_ATTR, "*  ESC returns to OS1993 to wait for it.");
        em_text(20, 6, EM_ATTR, "*  CTRL+ALT+DEL again ends every program and the GUI.");
        em_text(23, 24, EM_ATTR, "Press any of these keys to continue");

        int sc;
        while ((sc = keyboard_read()) < 0 && !u.cad_again && u.running)
            sched_wait();
        if (u.cad_again) {                          /* everything goes */
            klog("VMM: ending the GUI session\n");
            for (int i = 0; i < n; i++)
                kill_proc(list[i]);
            done = true;
        } else if (sc == 0x48 && sel > 0) {         /* Up */
            sel--;
        } else if (sc == 0x50 && sel < n - 1) {     /* Down */
            sel++;
        } else if (sc == 0x1C && n) {               /* Enter */
            kill_proc(list[sel]);
            done = true;
        } else if (sc == 0x01) {                    /* Esc */
            done = true;
        }
    }
    while (keyboard_read() >= 0)                    /* the key's release */
        ;
    vga_console_resume();
    u.emergency = false;
    u.cad_tick = 0;
    user_screen(true);                              /* the GUI repaints */
}

/* Console thread, every loop: run exec requests, free ended processes. */
void user_service(void)
{
    if (u.running && u.cad_tick && !u.cad_acked && (int32_t)(timer_ticks() - u.cad_tick) > 36)
        emergency();

    uint32_t f = irq_save();
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = &procs[i];
        if (p->state == P_RUNNING) {                /* threads that ended: free them */
            for (int k = 0; k < THREADS_PER_PROC; k++) {
                struct uthread *ut = &p->th[k];
                if (!ut->t || !ut->done || ut->t->state != T_DEAD)
                    continue;
                if (p->thread == ut->t)
                    p->thread = NULL;
                thread_reap(ut->t);
                ut->t = NULL;
                current->proc = p;
                proc_switch(p);
                vm_release(p, ut->region);
                ut->region = 0;
                current->proc = NULL;
            }
            continue;
        }
        if (p->state != P_DEAD)
            continue;
        bool all_dead = true;
        for (int k = 0; k < THREADS_PER_PROC; k++)
            if (p->th[k].t && p->th[k].t->state != T_DEAD)
                all_dead = false;
        if (!all_dead)
            continue;
        for (int k = 0; k < THREADS_PER_PROC; k++)
            if (p->th[k].t)
                thread_reap(p->th[k].t);
        current->proc = p;                          /* free its pages */
        proc_switch(p);
        files_close_all(&p->files);
        release_address_space(p);
        bool last = true;
        for (int k = 0; k < PROC_MAX; k++)
            if (&procs[k] != p && procs[k].state != P_FREE)
                last = false;
        if (last) {
            shared_destroy();
            free_images();                          /* nobody uses them now */
        }
        current->proc = NULL;
        installed = NULL;
        kfree(p->pde);
        memset(p, 0, sizeof *p);
        if (last && u.running) {                    /* the session is over */
            u.running = false;
            mouse_set_delivery(false);
            if (u.background) {
                for (uint16_t port = 0x3C0; port <= 0x3DA; port++)
                    if ((port >= 0x3C0 && port <= 0x3CF) || port == 0x3D4 || port == 0x3D5 || port == 0x3DA)
                        tss_trap_port(port, false);
                u.background = false;
            }
        }
    }
    irq_restore(f);

    if (req.pending) {
        int result;
        if (req.kind == REQ_EXEC) {
            result = u.running ? spawn(req.name, req.args) : -1;
        } else {                                    /* in the process's address space */
            struct process *was = current->proc;
            current->proc = req.proc;
            proc_switch(req.proc);
            result = file_op(&req.proc->files, req.op, &req.fa);
            current->proc = was;
        }
        f = irq_save();
        req.result = result;
        req.pending = false;
        thread_wake(req.waiter);
        irq_restore(f);
    }
}

/* The calling thread's process ends (interrupts off). */
__attribute__((noreturn))
static void finish(int result)
{
    struct process *p = cur_proc();
    klog("USER: %s exited with code %d\n", p->name, result);
    p->state = P_DEAD;
    p->result = result;
    p->killed = true;                               /* its other threads end too */
    sock_process_end(p->pid);
    p->th[current->uth].done = true;
    wake_threads(p);
    ended[ended_next % 16].pid = p->pid;
    ended[ended_next % 16].code = result;
    ended_next++;
    u.exits++;
    for (int n = 0; n < LOCK_MAX; n++)
        if (u.lock_owner[n] == p->pid)
            lock_handoff(n);
    if (p->pid == u.leader) {                       /* the shell: everyone goes */
        u.result = result;
        for (int i = 0; i < PROC_MAX; i++)
            if (procs[i].state == P_RUNNING) {
                procs[i].killed = true;
                procs[i].lock_wait = -1;
                wake_threads(&procs[i]);
            }
    }
    for (int i = 0; i < PROC_MAX; i++)              /* others may wait on us */
        if (procs[i].state == P_RUNNING) {
            wake_proc(&procs[i]);
            futex_wake(&procs[i], KEY_PROC(p->pid), THREADS_PER_PROC);
        }
    thread_exit();
}

/* The calling thread ends; the last one ends its process (with its code). */
__attribute__((noreturn))
static void thread_end(int code)
{
    struct process *p = cur_proc();
    struct uthread *ut = &p->th[current->uth];
    ut->done = true;
    ut->exit_code = code;
    if (p->state == P_RUNNING) {
        int live = 0;
        for (int i = 0; i < THREADS_PER_PROC; i++)
            if (p->th[i].t && !p->th[i].done)
                live++;
        if (!live)
            finish(code);
        futex_wake(p, KEY_THREAD(ut->tid), THREADS_PER_PROC);
    }
    thread_exit();
}

/* A killed process ends on its way back to ring 3. */
void user_return(struct regs *r)
{
    (void)r;
    struct process *p = cur_proc();
    if (!p)
        return;
    if (p->state == P_DEAD)                         /* another thread ended it */
        thread_end(0);
    if (p->killed)
        finish(-1);
    if (current->killed)                            /* TerminateThread */
        thread_end(current->kill_code);
}

static bool input_pending(void)
{
    return u.focus_event >= 0 || (vm_focus() == NULL && (keyboard_pending() || mouse_pending()));
}

/* Timer: wake processes whose wait timed out, or all waiters when input
 * is pending (whoever gets there first routes it). */
void user_tick(void)
{
    if (!u.running)
        return;
    uint32_t now = timer_ticks();
    bool input = input_pending();
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = &procs[i];
        if (p->state != P_RUNNING)
            continue;
        for (int k = 0; k < THREADS_PER_PROC; k++) {   /* futex waits that time out */
            struct thread *t = p->th[k].t;
            if (t && t->futex && t->deadline && (int32_t)(now - t->deadline) >= 0)
                thread_wake(t);
        }
        if (!p->waiting)
            continue;
        if ((input && p->wait_input) || (p->deadline && (int32_t)(now - p->deadline) >= 0))
            thread_wake(p->wait_thread);
    }
}

/* While a DOS session has the screen the GUI must not touch the VGA: its
 * port accesses trap and are emulated (writes dropped, reads FFh), and it
 * is told to stop drawing, then to repaint when the screen comes back --
 * what Windows' VDD did for a System VM in the background. */
static bool vga_port(uint16_t p)
{
    return (p >= 0x3C0 && p <= 0x3CF) || p == 0x3D4 || p == 0x3D5 || p == 0x3DA;
}

void user_screen(bool ours)
{
    if (!u.running || !(u.flags & UINFO_GRAPHICS) || ours == !u.background)
        return;
    u.background = !ours;
    if (!ours)                      /* keys still unread were for the switch (the */
        while (keyboard_read() >= 0)    /* Alt of Alt+Tab): read later, Alt sticks */
            ;
    for (uint16_t p = 0x3C0; p <= 0x3DA; p++)
        if (vga_port(p))
            tss_trap_port(p, !ours);
    u.focus_event = ours ? 1 : 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].state == P_RUNNING)
            wake_proc(&procs[i]);
}

/* ---- the real-time clock ------------------------------------------------------ */

static uint8_t cmos(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static void read_time(struct systime *t)
{
    uint32_t f = irq_save();
    while (cmos(0x0A) & 0x80)                       /* update in progress */
        ;
    uint8_t s = cmos(0x00), m = cmos(0x02), h = cmos(0x04), dow = cmos(0x06),
            d = cmos(0x07), mo = cmos(0x08), y = cmos(0x09), b = cmos(0x0B);
    irq_restore(f);
    if (!(b & 0x04)) {                              /* BCD */
#define BCD(x) (uint8_t)(((x) & 0x0F) + ((x) >> 4) * 10)
        s = BCD(s); m = BCD(m); d = BCD(d); mo = BCD(mo); y = BCD(y);
        h = (uint8_t)(BCD(h & 0x7F) | (h & 0x80));
#undef BCD
    }
    if (!(b & 0x02) && (h & 0x80))                  /* 12-hour clock, PM */
        h = (uint8_t)(((h & 0x7F) % 12) + 12);
    else if (!(b & 0x02) && (h % 12) == 0 && h == 12)
        h = 0;
    *t = (struct systime){ .year = (uint16_t)(y < 80 ? 2000 + y : 1900 + y), .month = mo,
                           .day_of_week = (uint16_t)(dow ? dow - 1 : 0), .day = d,
                           .hour = (uint16_t)(h & 0x7F), .minute = m, .second = s, .ms = 0 };
}

void rtc_dos_time(uint16_t *date, uint16_t *time)
{
    rtc_dos_time_odd(date, time, NULL);
}

/* ... and *odd: the second DOS's two-second times leave out (0 or 1). */
void rtc_dos_time_odd(uint16_t *date, uint16_t *time, uint8_t *odd)
{
    struct systime t;
    read_time(&t);
    *date = (uint16_t)((t.year - 1980) << 9 | t.month << 5 | t.day);
    *time = (uint16_t)(t.hour << 11 | t.minute << 5 | t.second / 2);
    if (odd)
        *odd = (uint8_t)(t.second & 1);
}

/* Another thread's request to the console thread is in progress: wait our
 * turn with interrupts on. The console may be asleep on a disk interrupt
 * (INT 13h is the VMM's), which interrupts kept off would never deliver. */
static void wait_turn(void)
{
    while (req.pending) {
        cpu_sti();
        sched_yield();
        cpu_cli();
    }
}

/* ---- INT 30h ------------------------------------------------------------------ */

void syscall_dispatch(struct regs *r)
{
    struct process *self = cur_proc();
    switch (r->eax) {
    case SYS_EXIT:
        finish((int)(r->ebx & 0xFF));
    case SYS_PRINT: {
        char text[160];
        uint32_t len = r->ecx < sizeof text - 1 ? r->ecx : sizeof text - 1;
        if (!user_range_ok(r->ebx, len)) {
            r->eax = (uint32_t)-1;
            break;
        }
        memcpy(text, (const void *)(uintptr_t)r->ebx, len);
        text[len] = '\0';
        klog("GUI: %s\n", text);
        u.prints++;
        r->eax = 0;
        break;
    }
    case SYS_TICKS:
        r->eax = timer_ticks();
        break;
    case SYS_BEEP:
        speaker_beep(r->ebx, r->ecx > 5000 ? 5000 : r->ecx);
        r->eax = 0;
        break;
    case SYS_RANDOM:
        if (r->ecx > 256 || !user_writable(r->ebx, r->ecx)) {
            r->eax = (uint32_t)-1;
            break;
        }
        random_bytes((void *)(uintptr_t)r->ebx, r->ecx);
        r->eax = 0;
        break;
    case SYS_FAIL: {
        static const char *const why[] = { "failure", "stack buffer overrun", "heap corruption",
                                           "buffer overrun", "runtime-constraint violation",
                                           "abort", "assertion failed" };
        char text[128] = "";
        for (uint32_t i = 0; r->ecx && i < sizeof text - 1 && user_range_ok(r->ecx + i, 1); i++) {
            text[i] = *(const char *)(uintptr_t)(r->ecx + i);
            if (!text[i])
                break;
        }
        klog("USER: %s: %s%s%s (fail fast)\n", self->name, why[r->ebx < 7 ? r->ebx : 0],
             text[0] ? ": " : "", text);
        finish(FAIL_EXIT);
        break;
    }
    case SYS_YIELD:
        schedule();
        r->eax = 0;
        break;
    case SYS_THREAD_CREATE: {
        struct thread_args a;
        if (!user_range_ok(r->ebx, sizeof a)) {
            r->eax = (uint32_t)-ERROR_NOACCESS;
            break;
        }
        memcpy(&a, (const void *)(uintptr_t)r->ebx, sizeof a);
        if (a.start >= USER_LIMIT || a.fn >= USER_LIMIT) {
            r->eax = (uint32_t)-ERROR_INVALID_PARAMETER;
            break;
        }
        int slot = thread_new(self, &a, false);
        if (slot < 0) {
            r->eax = (uint32_t)slot;
            break;
        }
        if (a.flags & THREAD_SUSPENDED)
            self->th[slot].suspended = true;
        else
            thread_wake(self->th[slot].t);
        r->eax = (uint32_t)self->th[slot].tid;
        break;
    }
    case SYS_THREAD_EXIT:
        thread_end((int)r->ebx);
    case SYS_THREAD_RESUME: {
        struct uthread *ut = uthread_of(self, (int)r->ebx);
        r->eax = (uint32_t)-1;
        if (ut && ut->t && ut->suspended) {
            ut->suspended = false;
            thread_wake(ut->t);
            r->eax = 0;
        }
        break;
    }
    case SYS_THREAD_KILL: {
        struct uthread *ut = uthread_of(self, (int)r->ebx);
        if (!ut || !ut->t || ut->done) {
            r->eax = (uint32_t)-1;
            break;
        }
        if (ut->t == current)
            thread_end((int)r->ecx);
        ut->t->killed = true;
        ut->t->kill_code = (int)r->ecx;
        ut->suspended = false;
        thread_wake(ut->t);
        r->eax = 0;
        break;
    }
    case SYS_THREAD_WAIT: {
        struct uthread *ut = uthread_of(self, (int)r->ebx);
        if (!ut || ut->t == current || (r->edx && !user_writable(r->edx, 4))) {
            r->eax = (uint32_t)-1;
            break;
        }
        int tid = ut->tid;
        if (!ut->done && r->ecx)
            futex_block(self, KEY_THREAD(tid), r->ecx);
        if (ut->tid == tid && ut->done) {
            if (r->edx)
                *(int *)(uintptr_t)r->edx = ut->exit_code;
            r->eax = 0;
        } else {
            r->eax = ut->tid == tid ? 1 : (uint32_t)-1;
        }
        break;
    }
    case SYS_FUTEX_WAIT: {
        if ((r->ebx & 3) || !user_range_ok(r->ebx, 4)) {
            r->eax = (uint32_t)-1;
            break;
        }
        if (*(volatile uint32_t *)(uintptr_t)r->ebx != r->ecx) {
            r->eax = 1;
            break;
        }
        r->eax = !r->edx ? 2 : futex_block(self, r->ebx, r->edx) ? 0 : 2;
        break;
    }
    case SYS_FUTEX_WAKE:
        r->eax = (r->ebx & 3) || r->ebx >= USER_LIMIT ? 0 : (uint32_t)futex_wake(self, r->ebx, (int)r->ecx);
        break;
    case SYS_PROC_WAIT: {
        if (r->edx && !user_writable(r->edx, 4)) {
            r->eax = (uint32_t)-1;
            break;
        }
        int pid = (int)r->ebx;
        if (pid == self->pid) {
            r->eax = (uint32_t)-1;
            break;
        }
        if (proc_of(pid) && r->ecx)
            futex_block(self, KEY_PROC(pid), r->ecx);
        if (proc_of(pid)) {
            r->eax = 1;
            break;
        }
        r->eax = (uint32_t)-1;
        for (unsigned i = 0; i < 16; i++)
            if (ended[i].pid == pid && pid) {
                if (r->edx)
                    *(int *)(uintptr_t)r->edx = ended[i].code;
                r->eax = 0;
            }
        break;
    }
    case SYS_VMEM: {
        struct vmem_args a;
        if (!user_writable(r->ecx, sizeof a)) {
            r->eax = (uint32_t)-ERROR_NOACCESS;
            break;
        }
        memcpy(&a, (const void *)(uintptr_t)r->ecx, sizeof a);
        int rc = vm_op(self, r->ebx, &a);
        memcpy((void *)(uintptr_t)r->ecx, &a, sizeof a);
        r->eax = (uint32_t)rc;
        break;
    }
    case SYS_SOCKET: {
        struct sock_args a;
        if (!user_writable(r->ecx, sizeof a)) {
            r->eax = (uint32_t)-EFAULT;
            break;
        }
        memcpy(&a, (const void *)(uintptr_t)r->ecx, sizeof a);
        int rc = sock_syscall(self->pid, r->ebx, &a);
        if (self->state == P_RUNNING)
            memcpy((void *)(uintptr_t)r->ecx, &a, sizeof a);
        r->eax = (uint32_t)rc;
        break;
    }
    case SYS_TIMEZONE: {                            /* EBX 1: set, 0: get; ECX: struct fs_timezone * */
        struct fs_timezone z;
        if (!user_writable(r->ecx, sizeof z)) {
            r->eax = (uint32_t)-1;
            break;
        }
        if (r->ebx == 1) {
            memcpy(&z, (const void *)(uintptr_t)r->ecx, sizeof z);
            fs_set_timezone(&z);
        }
        fs_get_timezone(&z);
        memcpy((void *)(uintptr_t)r->ecx, &z, sizeof z);
        uint16_t d, t;
        rtc_dos_time(&d, &t);
        r->eax = !fs_timezone_known() || !z.dst.month ? 0 : fs_bias_local(d, t) != z.bias + z.std_bias ? 2 : 1;
        break;
    }
    case SYS_MEMINFO: {
        struct mem_info mi = { pmm_total_pages(), pmm_free_pages(),
                               (PAGE_ALIGN_UP(self->heap_end) - USER_HEAP) / PAGE_SIZE +
                               vm_committed_pages(self) + USER_STACK_SIZE / PAGE_SIZE };
        if (!user_writable(r->ebx, sizeof mi)) {
            r->eax = (uint32_t)-1;
            break;
        }
        memcpy((void *)(uintptr_t)r->ebx, &mi, sizeof mi);
        r->eax = 0;
        break;
    }
    case SYS_WAIT_TICK:
        self->wait_thread = current;
        self->waiting = true;
        self->deadline = timer_ticks() + 1;
        current->state = T_BLOCKED;
        schedule();
        self->waiting = false;
        r->eax = 0;
        break;
    case SYS_WAIT: {
        if (self->wake) {
            self->wake = false;
            r->eax = 1;
            break;
        }
        uint32_t ms = r->ebx;
        if (ms == 0) {
            r->eax = 0;
            break;
        }
        bool for_input = r->ecx & WAIT_INPUT;
        if (for_input && input_pending()) {
            r->eax = 0;
            break;
        }
        uint32_t ticks = ms == WAIT_INFINITE ? 0 : (ms * 10 + 548) / 549;
        self->deadline = ticks ? timer_ticks() + ticks : 0;
        self->wait_thread = current;
        self->waiting = true;
        self->wait_input = for_input;
        while (!self->wake && !must_stop(self)) {
            current->state = T_BLOCKED;
            schedule();
            if (self->deadline && (int32_t)(timer_ticks() - self->deadline) >= 0)
                break;
            if (for_input && input_pending())
                break;                              /* input: look at it */
        }
        self->waiting = false;
        self->wait_input = false;
        r->eax = self->wake ? 1 : 0;
        self->wake = false;
        break;
    }
    case SYS_WAKE: {
        struct process *p = proc_of((int)r->ebx);
        if (p) wake_proc(p);
        r->eax = p ? 0 : (uint32_t)-1;
        break;
    }
    case SYS_GETPID:
        r->eax = (uint32_t)self->pid;
        break;
    case SYS_PROC_ALIVE:
        r->eax = proc_of((int)r->ebx) != NULL;
        break;
    case SYS_PROC_EXITS:
        r->eax = u.exits;
        break;
    case SYS_KILL: {
        struct process *p = proc_of((int)r->ebx);
        if (!p) {
            r->eax = (uint32_t)-1;
            break;
        }
        klog("USER: killing %s (process %d)\n", p->name, p->pid);
        p->killed = true;
        if (p->lock_wait >= 0)
            p->lock_wait = -1;
        wake_threads(p);
        r->eax = 0;
        if (p == self)
            finish(-1);
        break;
    }
    case SYS_LOCK: {
        unsigned n = r->ebx;
        if (n >= LOCK_MAX) {
            r->eax = (uint32_t)-1;
            break;
        }
        if (u.lock_owner[n] && !proc_of(u.lock_owner[n]))
            u.lock_owner[n] = 0;                    /* its owner died */
        if (!u.lock_owner[n]) {
            u.lock_owner[n] = self->pid;
        } else if (u.lock_owner[n] != self->pid) {
            self->lock_wait = (int)n;               /* lock_handoff wakes us */
            self->lock_thread = current;
            while (self->lock_wait == (int)n && !must_stop(self)) {
                current->state = T_BLOCKED;
                schedule();
            }
        }
        r->eax = u.lock_owner[n] == self->pid ? 0 : (uint32_t)-1;
        break;
    }
    case SYS_UNLOCK: {
        unsigned n = r->ebx;
        if (n < LOCK_MAX && u.lock_owner[n] == self->pid) {
            lock_handoff(n);
            r->eax = 0;
        } else {
            r->eax = (uint32_t)-1;
        }
        break;
    }
    case SYS_EXEC: {
        char name[16], args[ARGS_MAX];
        if (!user_string(r->ebx, name, sizeof name) ||
            (r->ecx && !user_string(r->ecx, args, sizeof args))) {
            r->eax = (uint32_t)-1;
            break;
        }
        if (!r->ecx)
            args[0] = '\0';
        wait_turn();                                /* one request at a time */
        req.kind = REQ_EXEC;
        memcpy(req.name, name, sizeof name);
        memcpy(req.args, args, sizeof args);
        req.waiter = current;
        req.pending = true;
        while (req.pending) {                       /* the console loads it */
            current->state = T_BLOCKED;
            schedule();
        }
        r->eax = (uint32_t)req.result;
        break;
    }
    case SYS_FILE: {
        struct file_args fa;
        if (!user_range_ok(r->ecx, sizeof fa)) {
            r->eax = (uint32_t)-ERROR_NOACCESS;
            break;
        }
        memcpy(&fa, (const void *)(uintptr_t)r->ecx, sizeof fa);
        wait_turn();
        req.kind = REQ_FILE;
        req.proc = self;
        req.op = r->ebx;
        req.fa = fa;
        req.waiter = current;
        req.pending = true;
        while (req.pending) {                       /* the console calls DOS */
            current->state = T_BLOCKED;
            schedule();
        }
        r->eax = (uint32_t)req.result;
        break;
    }
    case SYS_SBRK: {                                /* grow the heap, zeroed pages */
        uint32_t old = self->heap_end, add = r->ebx;
        uint32_t want = old + add;
        if (add > USER_HEAP_LIMIT - old) {
            r->eax = 0;
            break;
        }
        uint32_t from = PAGE_ALIGN_UP(old), to = PAGE_ALIGN_UP(want);
        if (to > from && !map_zeroed(from, to - from)) {
            unmap_free(from, to - from);
            r->eax = 0;
            break;
        }
        self->heap_end = want;
        r->eax = old;
        break;
    }
    case SYS_TIME: {
        struct systime t;
        if (!user_writable(r->ebx, sizeof t)) {
            r->eax = (uint32_t)-1;
            break;
        }
        read_time(&t);
        memcpy((void *)(uintptr_t)r->ebx, &t, sizeof t);
        r->eax = 0;
        break;
    }
    case SYS_GET_KEY:
        r->eax = vm_focus() == NULL ? (uint32_t)keyboard_read() : (uint32_t)-1;
        break;
    case SYS_GET_EVENT: {
        struct user_event e = { 0 };
        if (!user_writable(r->ebx, sizeof e)) {
            r->eax = (uint32_t)-1;
            break;
        }
        int sc;
        if (u.focus_event >= 0) {
            e.type = EV_FOCUS;
            e.key = (uint16_t)u.focus_event;
            u.focus_event = -1;
        } else if (u.cad_event) {
            e.type = EV_CAD;
            u.cad_event = false;
        } else if ((sc = vm_focus() == NULL && !u.emergency ? keyboard_read() : -1) >= 0) {
            e.type = EV_KEY;
            e.key = (uint16_t)sc;
        } else if (!mouse_get_event(&e)) {
            r->eax = 0;
            break;
        }
        memcpy((void *)(uintptr_t)r->ebx, &e, sizeof e);
        r->eax = 1;
        break;
    }
    case SYS_MOUSE:
        r->eax = mouse_present();
        break;
    case SYS_DISPLAY_INFO:
        if (!user_writable(r->ebx, sizeof(struct display_info))) {
            r->eax = (uint32_t)-1;
            break;
        }
        memcpy((void *)(uintptr_t)r->ebx, display_current(), sizeof(struct display_info));
        r->eax = 0;
        break;
    case SYS_DISPLAY_MODES: {
        struct display_mode list[16];
        int n = display_modes(list, 16), max = (int)r->ecx;
        if (max > n) max = n;
        if (max < 0 || !user_writable(r->ebx, (uint32_t)max * sizeof list[0])) {
            r->eax = (uint32_t)-1;
            break;
        }
        memcpy((void *)(uintptr_t)r->ebx, list, (uint32_t)max * sizeof list[0]);
        r->eax = (uint32_t)n;
        break;
    }
    case SYS_DISPLAY_SET:
        r->eax = u.shared_on && !u.background && !u.emergency
                 ? (uint32_t)display_set((int)r->ebx, (int)r->ecx, (int)r->edx) : (uint32_t)-1;
        break;
    case SYS_TASKLIST_ACK:
        u.cad_acked = true;
        r->eax = 0;
        break;
    case SYS_PROC_LIST: {
        uint32_t n = 0, max = r->ecx;
        if (max > PROC_MAX) max = PROC_MAX;
        if (!user_writable(r->ebx, max * sizeof(struct proc_entry))) {
            r->eax = (uint32_t)-1;
            break;
        }
        struct proc_entry *out = (struct proc_entry *)(uintptr_t)r->ebx;
        for (int i = 0; i < PROC_MAX && n < max; i++) {
            struct process *p = &procs[i];
            if (p->state != P_RUNNING || p->killed)
                continue;
            out[n].pid = (uint32_t)p->pid;
            out[n].flags = (p->pid == u.leader ? PROC_LEADER : 0) |
                           (u.lock_owner[0] == p->pid ? PROC_UI_LOCK : 0);
            memcpy(out[n].name, p->name, sizeof out[n].name);
            n++;
        }
        r->eax = n;
        break;
    }
    case SYS_NEW_SESSION: {
        struct vm *vm = dos_new_session();
        r->eax = vm ? (uint32_t)vm->id : (uint32_t)-1;
        break;
    }
    default:
        r->eax = (uint32_t)-1;
    }
}

/* Emulate IN/OUT to a VGA port from a GUI in the background. */
static bool emulate_vga_io(struct regs *r)
{
    if (!u.background || r->int_no != 13 || !user_range_ok(r->eip, 2))
        return false;
    const uint8_t *code = (const uint8_t *)(uintptr_t)r->eip;
    int len = 0, size = 1;
    if (code[0] == 0x66) { size = 4; len = 1; }
    uint8_t op = code[len++];
    uint16_t port;
    switch (op) {
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: port = code[len++]; break;
    case 0xEC: case 0xED: case 0xEE: case 0xEF: port = (uint16_t)r->edx; break;
    default: return false;
    }
    if (op & 1)
        size = size == 4 ? 4 : 2;
    if (!vga_port(port))
        return false;
    if (!(op & 2)) {                            /* IN */
        if (size == 1)      r->eax = (r->eax & ~0xFFu) | 0xFF;
        else if (size == 2) r->eax = (r->eax & ~0xFFFFu) | 0xFFFF;
        else                r->eax = 0xFFFFFFFFu;
    }
    r->eip += (uint32_t)len;
    return true;
}

void user_fault(struct regs *r)
{
    if (emulate_vga_io(r))
        return;
    struct process *p = cur_proc();
    uint32_t cr2 = read_cr2();
    for (int i = 0; r->int_no == 14 && i < THREADS_PER_PROC; i++)    /* a guard page */
        if (p->th[i].t && p->th[i].start && cr2 - p->th[i].region < PAGE_SIZE)
            klog("USER: %s: thread %d overflowed its stack\n", p->name, p->th[i].tid);
    if (r->int_no == 14)
        klog("USER: %s killed by exception 0E (page fault, %s %08X) at %04X:%08X\n",
             p->name, (r->err_code & 2) ? "write to" : "read from", cr2,
             r->cs & 0xFFFF, r->eip);
    else
        klog("USER: %s killed by exception %02X at %04X:%08X\n",
             p->name, r->int_no, r->cs & 0xFFFF, r->eip);
    finish(-(int)(r->int_no + 1));
}

/* ---- self-test ------------------------------------------------------------------ */

static int run_test(uint32_t flags)
{
    if (user_start("DEMO.BIN", flags) != 0)
        panic("Cannot load DEMO.BIN for the ring-3 self-test.");
    while (u.running) {
        user_service();
        sched_wait();
    }
    return u.result;
}

/* Ring 3 can't execute privileged instructions or touch kernel memory. */
void user_selftest(void)
{
    int rc = run_test(UINFO_TEST_CLI);
    if (rc != -(13 + 1) || u.prints < 2)
        panic("Ring-3 CLI test: result %d, %u prints (expected #GP).", rc, u.prints);
    rc = run_test(UINFO_TEST_KERNEL);
    if (rc != -(14 + 1))
        panic("Ring-3 kernel-write test: result %d (expected #PF).", rc);
    rc = run_test(UINFO_TEST_CODE);
    if (rc != -(14 + 1))
        panic("Ring-3 code-write test: result %d (expected #PF).", rc);
    kprintf("  Ring 3: system calls work; CLI, kernel and code writes are refused.\n");
    klog("VMM: ring-3 self-test passed\n");
}
