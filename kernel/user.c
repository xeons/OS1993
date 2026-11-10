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
#include "gdt.h"
#include "interrupts.h"
#include "io.h"
#include "mm.h"
#include "sched.h"
#include "user.h"
#include "v86.h"
#include "vga.h"
#include "vm.h"

#define MAX_IMAGE   (2u * 1024 * 1024)
#define USER_PDES   512                     /* PDEs 0-511 cover user space */
#define SHARED_PDE  (USER_SHARED >> 22)

/* A module's image, kept while the session lasts so later processes can
 * share it: the frames of its read-only pages, mapped into each of them
 * read-only, and a pristine copy of its writable pages -- taken after its
 * imports were linked, before any of its code ran -- that each one gets a
 * copy of. (Every DLL sits at a fixed address, so linked import slots hold
 * the same values in every process.) */
#define IMAGE_MAX 16

struct image {
    char      name[16];
    uint32_t  kind, base, data, end, entry, exports, imports;
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
    bool     resolved;
    struct image *img;                      /* shared from this image, or NULL */
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
    uint32_t       shared_pde;              /* the shared region's page table */
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
    volatile uint32_t *pd = (volatile uint32_t *)PD_WINDOW;
    for (int i = 1; i < USER_PDES; i++) {
        if (i == SHARED_PDE)                    /* shared_destroy's business */
            continue;
        if (pd[i] & PTE_P)
            pmm_free(pd[i] & PAGE_MASK);
        pd[i] = 0;
    }
    tlb_flush();
}

/* The shared region: video memory and the arena, for the whole session. */
static bool shared_create(void)
{
    for (uint32_t i = 0; i < 16; i++)
        if (vmm_map(USER_VGA + i * PAGE_SIZE, 0xA0000 + i * PAGE_SIZE, PTE_W | PTE_U) != 0)
            return false;
    if (!map_zeroed(USER_ARENA, USER_ARENA_SIZE))
        return false;
    u.shared_pde = ((volatile uint32_t *)PD_WINDOW)[SHARED_PDE];
    return true;
}

static void shared_destroy(void)                /* the last process is installed */
{
    if (!u.shared_pde)
        return;
    for (uint32_t i = 0; i < 16; i++)
        vmm_unmap(USER_VGA + i * PAGE_SIZE);    /* device memory: don't free */
    unmap_free(USER_ARENA, USER_ARENA_SIZE);
    volatile uint32_t *pd = (volatile uint32_t *)PD_WINDOW;
    pmm_free(u.shared_pde & PAGE_MASK);
    pd[SHARED_PDE] = 0;
    tlb_flush();
    u.shared_pde = 0;
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
    user_enter(p->entry, USER_STACK_TOP, USER_INFO);
}

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
    p->pde[SHARED_PDE] = u.shared_pde;              /* 0 for the first one */

    struct thread *self = current;                  /* load into p's space */
    struct process *was = self->proc;
    self->proc = p;
    proc_switch(p);
    bool first = !u.shared_pde;
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
        if (u.shared_pde) {
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
        p->thread = thread_create(p->name, user_thread_main, p, NULL);
        ok = p->thread != NULL;
        if (ok)
            keep_images(p);                         /* before any of its code runs */
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
    if (p->waiting && p->thread)
        thread_wake(p->thread);
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
            thread_wake(p->thread);
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
    thread_wake(p->thread);                         /* ends on its way to ring 3 */
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
    if (u.running && u.cad_tick && !u.cad_acked && timer_ticks() - u.cad_tick > 36)
        emergency();

    uint32_t f = irq_save();
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = &procs[i];
        if (p->state != P_DEAD || p->thread->state != T_DEAD)
            continue;
        thread_reap(p->thread);
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
                thread_wake(procs[i].thread);
            }
    }
    for (int i = 0; i < PROC_MAX; i++)              /* others may wait on us */
        if (procs[i].state == P_RUNNING)
            wake_proc(&procs[i]);
    thread_exit();
}

/* A killed process ends on its way back to ring 3. */
void user_return(struct regs *r)
{
    (void)r;
    struct process *p = cur_proc();
    if (p && p->killed && p->state == P_RUNNING)
        finish(-1);
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
        if (p->state != P_RUNNING || !p->waiting)
            continue;
        if ((input && p->wait_input) || (p->deadline && (int32_t)(now - p->deadline) >= 0))
            thread_wake(p->thread);
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
    case SYS_YIELD:
        schedule();
        r->eax = 0;
        break;
    case SYS_WAIT_TICK:
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
        self->waiting = true;
        self->wait_input = for_input;
        while (!self->wake && !self->killed) {
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
        thread_wake(p->thread);
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
            while (self->lock_wait == (int)n && !self->killed) {
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
        while (req.pending)                         /* one request at a time */
            schedule();
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
        while (req.pending)
            schedule();
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
    if (r->int_no == 14)
        klog("USER: %s killed by exception 0E (page fault, %s %08X) at %04X:%08X\n",
             p->name, (r->err_code & 2) ? "write to" : "read from", read_cr2(),
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
