/*
 * user.c -- ring-3 programs: loading, the INT 30h system calls, faults.
 *
 * There is one ring-3 program at a time: the GUI, which belongs to the
 * System VM (Windows ran its protected-mode code there too). Programs and
 * the DLLs they import are MOD3 modules (kernel/include/user.h), each
 * linked at a fixed address. The VMM console thread loads them through
 * DOS -- the System VM's own file system -- into fresh user pages, fills in
 * every program's and DLL's imports, and starts a ring-3 thread.
 */
#include "kernel.h"
#include "devices.h"
#include "gdt.h"
#include "io.h"
#include "mm.h"
#include "sched.h"
#include "user.h"
#include "v86.h"
#include "vga.h"
#include "vm.h"

#define MAX_IMAGE  (2u * 1024 * 1024)

struct module {
    char     name[16];
    uint32_t base, end;                 /* mapped [base, end) */
    uint32_t exports, imports;
    bool     resolved;
};

void user_enter(uint32_t eip, uint32_t esp, uint32_t ebx);  /* entry.asm */

static struct {
    struct thread *thread;
    volatile bool  running;
    int            result;
    uint32_t       entry;
    struct module  mods[MOD_MAX];       /* mods[0] is the program */
    int            nmods;
    bool           vga_mapped;
    uint32_t       prints;              /* SYS_PRINT calls (self-test) */
    volatile int   focus_event;         /* -1 none, else EV_FOCUS key */
    bool           background;          /* VGA ports trapped */
} u = { .focus_event = -1 };

/* VGA ports the display driver uses */
static bool vga_port(uint16_t p)
{
    return (p >= 0x3C0 && p <= 0x3CF) || p == 0x3D4 || p == 0x3D5 || p == 0x3DA;
}

static void trap_vga(bool trap)
{
    for (uint16_t p = 0x3C0; p <= 0x3DA; p++)
        if (vga_port(p))
            tss_trap_port(p, trap);
}

/* While a DOS session has the screen the GUI must not touch the VGA: its
 * port accesses trap and are emulated (writes dropped, reads FFh), and it
 * is told to stop drawing, then to repaint when the screen comes back --
 * what Windows' VDD did for a System VM in the background. */
void user_screen(bool ours)
{
    if (!u.running || ours == !u.background)
        return;
    u.background = !ours;
    trap_vga(!ours);
    u.focus_event = ours ? 1 : 0;
    if (u.thread)
        thread_wake(u.thread);
}

/* ---- user address space ------------------------------------------------ */

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

static void release_address_space(void)
{
    for (int i = 0; i < u.nmods; i++)
        unmap_free(u.mods[i].base, u.mods[i].end - u.mods[i].base);
    u.nmods = 0;
    unmap_free(USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE);
    unmap_free(USER_INFO, 2 * PAGE_SIZE);
    if (u.vga_mapped)                       /* device memory: unmap, don't free */
        for (uint32_t i = 0; i < 16; i++)
            vmm_unmap(USER_VGA + i * PAGE_SIZE);
    u.vga_mapped = false;
}

/* Is [p, p+len) ordinary mapped user memory? */
static bool user_range_ok(uint32_t p, uint32_t len)
{
    if (p < USER_BASE || p >= USER_LIMIT || len > USER_LIMIT - p)
        return false;
    for (uint32_t a = PAGE_ALIGN_DOWN(p); a < p + len; a += PAGE_SIZE)
        if (!vmm_translate(a, NULL))
            return false;
    return true;
}

/* ---- loading ------------------------------------------------------------ */

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

static void user_thread_main(void *arg)
{
    (void)arg;
    user_enter(u.entry, USER_STACK_TOP, USER_INFO);
}

/* ---- modules ------------------------------------------------------------- */

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

static struct module *find_module(const char *name)
{
    for (int i = 0; i < u.nmods; i++)
        if (same_name(u.mods[i].name, name))
            return &u.mods[i];
    return NULL;
}

/* Read a module file into fresh user pages at its own base address. */
static struct module *load_module(const char *name, uint32_t kind)
{
    if (u.nmods == MOD_MAX) {
        klog("USER: too many modules loading %s\n", name);
        return NULL;
    }
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
              !(hdr.base & ~PAGE_MASK);
    if (ok && kind == MOD_PROGRAM)
        ok = hdr.base == USER_BASE && hdr.entry >= hdr.base && hdr.entry < hdr.base + hdr.file_size;
    if (ok && kind == MOD_LIBRARY)
        ok = hdr.base >= DLL_BASE && end <= DLL_LIMIT;
    for (int i = 0; ok && i < u.nmods; i++)         /* no overlaps */
        ok = end <= u.mods[i].base || hdr.base >= u.mods[i].end;
    if (!ok) {
        klog("USER: %s is not a valid %s module (or overlaps another)\n", path,
             kind == MOD_PROGRAM ? "program" : "library");
        dos_close(h);
        return NULL;
    }

    struct module *m = &u.mods[u.nmods];
    memset(m, 0, sizeof *m);
    m->base = hdr.base;
    m->end = end;
    if (!map_zeroed(m->base, m->end - m->base)) {
        unmap_free(m->base, m->end - m->base);
        dos_close(h);
        klog("USER: out of memory loading %s\n", path);
        return NULL;
    }
    u.nmods++;

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
        u.entry = hdr.entry;
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
static bool resolve(struct module *m, int depth)
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
    unsigned linked = 0;
    for (uint32_t d = m->imports;; d += sizeof(struct mod_import)) {
        if (!in_mod(m, d, sizeof(struct mod_import)))
            return false;
        const struct mod_import *imp = (const void *)(uintptr_t)d;
        if (!imp->module)
            break;
        const char *dll_name = mod_str(m, imp->module, 16);
        if (!dll_name)
            return false;
        struct module *dll = find_module(dll_name);
        if (!dll) {
            dll = load_module(dll_name, MOD_LIBRARY);
            if (!dll)
                return false;
            const struct mod_exports *ex = (const void *)(uintptr_t)dll->exports;
            klog("USER: loaded %s at %08X (%u exports)\n", dll->name, dll->base,
                 dll->exports && in_mod(dll, dll->exports, 4) ? ex->count : 0);
        }
        if (!resolve(dll, depth + 1))
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
            linked++;
        }
    }
    klog("USER: %s: %u imports linked\n", m->name, linked);
    return true;
}

int user_start(const char *name, uint32_t flags)
{
    if (u.running)
        return -1;
    if (u.thread && thread_reap(u.thread))
        u.thread = NULL;
    release_address_space();

    struct module *prog = load_module(name, MOD_PROGRAM);
    if (!prog || !resolve(prog, 0) ||
        !map_zeroed(USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE) ||
        !map_zeroed(USER_INFO, 2 * PAGE_SIZE)) {
        klog("USER: could not start %s\n", name);
        release_address_space();
        return -1;
    }

    /* Info block, the font, and (for the GUI) the video memory */
    struct user_info *info = (struct user_info *)(uintptr_t)USER_INFO;
    info->flags = flags;
    info->width = VGA_GFX_W;
    info->height = VGA_GFX_H;
    info->font = USER_INFO + PAGE_SIZE;
    memcpy((void *)(uintptr_t)info->font, vga_font8x16(), 256 * 16);
    if (flags & UINFO_GRAPHICS) {
        for (uint32_t i = 0; i < 16; i++)
            vmm_map(USER_VGA + i * PAGE_SIZE, 0xA0000 + i * PAGE_SIZE, PTE_W | PTE_U);
        u.vga_mapped = true;
        info->vga = USER_VGA;
    }

    u.result = 0;
    u.prints = 0;
    u.thread = thread_create("GUI", user_thread_main, NULL, NULL);
    if (!u.thread) {
        release_address_space();
        return -1;
    }
    u.thread->user = true;
    u.running = true;
    u.focus_event = -1;
    u.background = false;
    mouse_set_delivery((flags & UINFO_GRAPHICS) != 0);
    klog("USER: started %s (entry %08X, %d modules)\n", name, u.entry, u.nmods);
    thread_wake(u.thread);
    return 0;
}

bool user_running(void) { return u.running; }
int  user_result(void)  { return u.result; }

/* Called by the timer: a GUI waiting for a tick may run again. */
void user_tick(void)
{
    if (u.running && u.thread)
        thread_wake(u.thread);
}

__attribute__((noreturn))
static void finish(int result)
{
    mouse_set_delivery(false);
    if (u.background) {
        trap_vga(false);
        u.background = false;
    }
    u.result = result;
    u.running = false;
    thread_exit();
}

/* ---- INT 30h ------------------------------------------------------------- */

void syscall_dispatch(struct regs *r)
{
    switch (r->eax) {
    case SYS_EXIT:
        klog("USER: exited with code %u\n", r->ebx);
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
        current->state = T_BLOCKED;         /* user_tick() wakes us */
        schedule();
        r->eax = 0;
        break;
    case SYS_GET_KEY:
        r->eax = vm_focus() == NULL ? (uint32_t)keyboard_read() : (uint32_t)-1;
        break;
    case SYS_GET_EVENT: {
        struct user_event e = { 0 };
        int sc = vm_focus() == NULL ? keyboard_read() : -1;
        if (u.focus_event >= 0) {
            e.type = EV_FOCUS;
            e.key = (uint16_t)u.focus_event;
            u.focus_event = -1;
        } else if (sc >= 0) {
            e.type = EV_KEY;
            e.key = (uint16_t)sc;
        } else if (!mouse_get_event(&e)) {
            r->eax = 0;
            break;
        }
        if (!user_range_ok(r->ebx, sizeof e)) {
            r->eax = (uint32_t)-1;
            break;
        }
        memcpy((void *)(uintptr_t)r->ebx, &e, sizeof e);
        r->eax = 1;
        break;
    }
    case SYS_MOUSE:
        r->eax = mouse_present();
        break;
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
    if (r->int_no == 14)
        klog("USER: GUI killed by exception 0E (page fault, %s %08X) at %04X:%08X\n",
             (r->err_code & 2) ? "write to" : "read from", read_cr2(),
             r->cs & 0xFFFF, r->eip);
    else
        klog("USER: GUI killed by exception %02X at %04X:%08X\n",
             r->int_no, r->cs & 0xFFFF, r->eip);
    finish(-(int)(r->int_no + 1));
}

/* ---- self-test ----------------------------------------------------------- */

static int run_test(uint32_t flags)
{
    if (user_start("GUI.BIN", flags) != 0)
        panic("Cannot load GUI.BIN for the ring-3 self-test.");
    while (u.running)
        sched_wait();
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
    release_address_space();
    kprintf("  Ring 3: system calls work; CLI and kernel writes are refused.\n");
    klog("VMM: ring-3 self-test passed\n");
}
