/*
 * user.c -- ring-3 programs: loading, the INT 30h system calls, faults.
 *
 * There is one ring-3 program at a time: the GUI, which belongs to the
 * System VM (Windows ran its protected-mode code there too). It's a 32-bit
 * flat binary linked at USER_BASE. The VMM console thread loads it through
 * DOS -- the System VM's own file system -- into fresh user pages and starts
 * a ring-3 thread for it.
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

void user_enter(uint32_t eip, uint32_t esp, uint32_t ebx);  /* entry.asm */

static struct {
    struct thread *thread;
    volatile bool  running;
    int            result;
    uint32_t       entry;
    uint32_t       image_end;           /* user pages [USER_BASE, image_end) */
    bool           vga_mapped;
    uint32_t       prints;              /* SYS_PRINT calls (self-test) */
} u;

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
    if (u.image_end > USER_BASE)
        unmap_free(USER_BASE, u.image_end - USER_BASE);
    unmap_free(USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE);
    unmap_free(USER_INFO, 2 * PAGE_SIZE);
    if (u.vga_mapped)                       /* device memory: unmap, don't free */
        for (uint32_t i = 0; i < 16; i++)
            vmm_unmap(USER_VGA + i * PAGE_SIZE);
    u.image_end = 0;
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

int user_start(const char *name, uint32_t flags)
{
    if (u.running)
        return -1;
    if (u.thread && thread_reap(u.thread))
        u.thread = NULL;
    release_address_space();

    char path[128];
    program_path(name, path, sizeof path);
    int h = dos_open(path);
    if (h < 0) {
        klog("USER: cannot open %s\n", path);
        return -1;
    }

    uint32_t chunk = v86_buf_size() & ~0xFFFu;
    const uint8_t *buf = v86_ptr(v86_buf_seg(), v86_buf_off());
    int n = dos_read(h, chunk);
    struct user_image_header hdr;
    memcpy(&hdr, buf, sizeof hdr);
    if (n < (int)sizeof hdr || hdr.magic != UIMG_MAGIC || hdr.mem_size < hdr.file_size ||
        hdr.mem_size > MAX_IMAGE || hdr.entry < USER_BASE ||
        hdr.entry >= USER_BASE + hdr.file_size) {
        klog("USER: %s is not a ring-3 program image\n", path);
        dos_close(h);
        return -1;
    }

    u.image_end = USER_BASE + PAGE_ALIGN_UP(hdr.mem_size);
    if (!map_zeroed(USER_BASE, u.image_end - USER_BASE) ||
        !map_zeroed(USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE) ||
        !map_zeroed(USER_INFO, 2 * PAGE_SIZE)) {
        klog("USER: out of memory loading %s\n", path);
        dos_close(h);
        release_address_space();
        return -1;
    }

    uint32_t loaded = 0;
    while (n > 0 && loaded < hdr.file_size) {
        uint32_t take = (uint32_t)n;
        if (take > hdr.file_size - loaded)
            take = hdr.file_size - loaded;
        memcpy((void *)(uintptr_t)(USER_BASE + loaded), buf, take);
        loaded += take;
        n = dos_read(h, chunk);
    }
    dos_close(h);
    if (loaded != hdr.file_size) {
        klog("USER: short read on %s (%u of %u bytes)\n", path, loaded, hdr.file_size);
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

    u.entry = hdr.entry;
    u.result = 0;
    u.prints = 0;
    u.thread = thread_create("GUI", user_thread_main, NULL, NULL);
    if (!u.thread) {
        release_address_space();
        return -1;
    }
    u.thread->user = true;
    u.running = true;
    mouse_set_delivery((flags & UINFO_GRAPHICS) != 0);
    klog("USER: started %s (%u bytes, entry %08X)\n", path, hdr.file_size, hdr.entry);
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
        if (sc >= 0) {
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

void user_fault(struct regs *r)
{
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
