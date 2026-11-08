/*
 * vmm.c -- kmain: bring-up of the OS1993 Virtual Machine Manager.
 */
#include "kernel.h"
#include "console.h"
#include "devices.h"
#include "gdt.h"
#include "interrupts.h"
#include "io.h"
#include "mm.h"
#include "sched.h"
#include "serial.h"
#include "v86.h"
#include "vm.h"

const struct bootinfo *g_bootinfo;
static struct bootinfo boot_copy;   /* the original lives in VM memory */

#define TITLE_ATTR  ATTR(C_WHITE, C_BLUE)
#define STATUS_ATTR ATTR(C_BLACK, C_LGRAY)
#define TEXT_ATTR   ATTR(C_LGRAY, C_BLACK)
#define OK_ATTR     ATTR(C_LGREEN, C_BLACK)
#define INFO_ATTR   ATTR(C_YELLOW, C_BLACK)

#define SC_N 0x31
#define SC_X 0x2D

#define RECURSIVE_PD ((volatile uint32_t *)0xFFFFF000)

static void draw_frame(void)
{
    con_fill_row(0, TITLE_ATTR, ' ');
    con_write_at(0, 2, TITLE_ATTR, "OS1993 Virtual Machine Manager  Version " OS1993_VERSION);
    con_fill_row(CON_ROWS - 1, STATUS_ATTR, ' ');
    con_write_at(CON_ROWS - 1, 2, STATUS_ATTR, "N = new DOS session   Alt+Tab = switch   X = exit to MS-DOS");
    con_set_window(2, CON_ROWS - 3);
    con_set_attr(TEXT_ATTR);
    con_clear();
}

static void ok(const char *what)
{
    kprintf("  %-44s", what);
    con_set_attr(OK_ATTR);
    kprintf("[ OK ]\n");
    con_set_attr(TEXT_ATTR);
}

static void check_paging(const struct bootinfo *bi)
{
    if (!(read_cr0() & 0x80000000u))
        panic("Paging is not enabled.");
    if ((RECURSIVE_PD[1023] & ~0xFFFu) != bi->pd_phys)
        panic("Recursive page directory mapping is broken (PDE 1023 = %08X).",
              RECURSIVE_PD[1023]);
    if ((read_cr3() & ~0xFFFu) != bi->pd_phys)
        panic("CR3 does not match the loader's page directory.");
}

void kernel_shutdown(uint8_t exit_code)
{
    cpu_cli();
    pic_mask_all();
    vm_restore_dos();                   /* the original DOS memory and screen */
    klog("VMM: returning to DOS (exit code %u)\n", exit_code);
    serial_flush();
    exit_to_dos(g_bootinfo, exit_code);
}

static void show_vm_line(void)
{
    char line[CON_COLS + 1];
    int n = vm_count();
    struct vm *f = vm_focus();
    ksnprintf(line, sizeof line, "  DOS sessions running: %d    Free memory: %u KB    Focus: %s",
              n, pmm_free_pages() * 4, f ? f->name : "VMM console");
    con_fill_row(CON_ROWS - 2, INFO_ATTR, ' ');
    con_write_at(CON_ROWS - 2, 0, INFO_ATTR, line);
}

static void new_session(void)
{
    static int serial;
    char name[16];
    ksnprintf(name, sizeof name, "DOS %d", ++serial);
    struct vm *vm = vm_create(name, dos_session_body, NULL);
    if (!vm) {
        kprintf("  Cannot start another DOS session: out of memory or VM slots.\n");
        return;
    }
    kprintf("  Started %s in VM %d. Alt+Tab returns here; EXIT ends it.\n", vm->name, vm->id);
    vm_set_focus(vm);
    vm_start(vm);
}

void kmain(const struct bootinfo *bi)
{
    boot_copy = *bi;
    g_bootinfo = &boot_copy;
    bi = &boot_copy;
    serial_init();
    con_init();
    klog("\nVMM: OS1993 " OS1993_VERSION " kernel entered\n");

    if (bi->magic != BOOTINFO_MAGIC)
        panic("Bad boot information block (magic %08X).", bi->magic);

    con_show_cursor(0);
    draw_frame();

    gdt_init();
    ok("Global descriptor table and TSS");
    idt_init();
    ok("Interrupt descriptor table");
    pic_init(IRQ_BASE, IRQ_BASE + 8);
    ok("8259 PICs remapped to vectors 20h-2Fh");
    check_paging(bi);
    ok("Paging (recursive page directory)");
    pmm_init(bi);
    heap_init();
    klow_init();
    con_use_klow();
    ok("Page frame allocator and kernel heap");
    timer_init();
    keyboard_init();
    cpu_sti();
    ok("Timer and keyboard interrupts");
    mm_selftest();
    ok("Memory manager self-test");
    v86_init(bi);
    sched_init(system_vm);
    ok("V86 monitor, System VM and scheduler");

    kprintf("\n");
    kprintf("  Arena           %6u KB  physical %p, mapped at %p\n",
            bi->arena_size / 1024, bi->arena_phys, bi->arena_virt);
    kprintf("  Free frames     %6u KB  (each DOS session needs about %u KB)\n",
            pmm_free_pages() * 4, (VM_PRIV_PAGES + VM_VIDEO_PAGES + 1) * 4);
    kprintf("\n");
    v86_selftest(bi);

    bool test = bi->flags & BI_FLAG_TEST;
    uint32_t limit = 0;
    if (test) {
        vm_selftest();
        dos_shell_selftest();
        kprintf("  Test mode: returning to DOS automatically.\n");
        limit = timer_ticks() + TIMER_HZ_X10 / 10;      /* ~1 s */
    }
    kprintf("\n");

    for (;;) {
        char line[40];
        uint32_t t = timer_ticks();
        ksnprintf(line, sizeof line, "Uptime: %u.%u s ", t * 10 / TIMER_HZ_X10,
                  (t * 100 / TIMER_HZ_X10) % 10);
        con_write_at(CON_ROWS - 1, CON_COLS - 20, STATUS_ATTR, line);
        vm_reap();
        show_vm_line();

        int sc = keyboard_read();
        if (sc == SC_N) {
            new_session();
        } else if (sc == SC_X) {
            if (vm_count() == 0)
                break;
            kprintf("  %d DOS session(s) still running. Type EXIT in each first.\n", vm_count());
        }
        if (test && t >= limit)
            break;
        sched_wait();
    }

    klog("VMM: %u timer ticks seen\n", timer_ticks());
    kernel_shutdown(0);
}
