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
#include "serial.h"

const struct bootinfo *g_bootinfo;

#define TITLE_ATTR  ATTR(C_WHITE, C_BLUE)
#define STATUS_ATTR ATTR(C_BLACK, C_LGRAY)
#define TEXT_ATTR   ATTR(C_LGRAY, C_BLACK)
#define OK_ATTR     ATTR(C_LGREEN, C_BLACK)

#define RECURSIVE_PD ((volatile uint32_t *)0xFFFFF000)

static void draw_frame(void)
{
    con_fill_row(0, TITLE_ATTR, ' ');
    con_write_at(0, 2, TITLE_ATTR, "OS1993 Virtual Machine Manager  Version " OS1993_VERSION);
    con_fill_row(CON_ROWS - 1, STATUS_ATTR, ' ');
    con_write_at(CON_ROWS - 1, 2, STATUS_ATTR, "Press any key to return to MS-DOS");
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
    klog("VMM: returning to DOS (exit code %u)\n", exit_code);
    serial_flush();
    exit_to_dos(g_bootinfo, exit_code);
}

void kmain(const struct bootinfo *bi)
{
    g_bootinfo = bi;
    serial_init();
    con_init();
    klog("\nVMM: OS1993 " OS1993_VERSION " kernel entered, bootinfo at %p\n", (uint32_t)(uintptr_t)bi);

    if (bi->magic != BOOTINFO_MAGIC)
        panic("Bad boot information block (magic %08X).", bi->magic);

    con_show_cursor(0);
    draw_frame();

    gdt_init();
    ok("Global descriptor table");
    idt_init();
    ok("Interrupt descriptor table");
    pic_init(IRQ_BASE, IRQ_BASE + 8);
    ok("8259 PICs remapped to vectors 20h-2Fh");
    check_paging(bi);
    ok("Paging (recursive page directory)");
    pmm_init(bi);
    heap_init();
    ok("Page frame allocator and kernel heap");
    timer_init();
    keyboard_init();
    cpu_sti();
    ok("Timer and keyboard interrupts");
    mm_selftest();
    ok("Memory manager self-test");

    struct heap_stats hs;
    heap_get_stats(&hs);
    kprintf("\n");
    kprintf("  Arena           %6u KB  physical %p, mapped at %p\n",
            bi->arena_size / 1024, bi->arena_phys, bi->arena_virt);
    kprintf("  Kernel + tables %6u KB\n", bi->free_offset / 1024);
    kprintf("  Free frames     %6u KB  (%u of %u pages)\n",
            pmm_free_pages() * 4, pmm_free_pages(), pmm_total_pages());
    kprintf("  Kernel heap     %6u KB  mapped at %p, %u KB in use\n",
            hs.mapped / 1024, KHEAP_BASE, hs.used / 1024);
    kprintf("\n");

    bool test = bi->flags & BI_FLAG_TEST;
    uint32_t limit = 2 * TIMER_HZ_X10 / 10;  /* ~2 s in test mode */
    if (test)
        kprintf("  Test mode: returning to DOS automatically.\n");

    for (;;) {
        char line[40];
        uint32_t t = timer_ticks();
        ksnprintf(line, sizeof line, "Uptime: %u.%u s ", t * 10 / TIMER_HZ_X10,
                  (t * 100 / TIMER_HZ_X10) % 10);
        con_write_at(CON_ROWS - 1, CON_COLS - 20, STATUS_ATTR, line);

        int sc = keyboard_read();
        if (sc >= 0 && !(sc & 0x80))
            break;
        if (test && t >= limit)
            break;
        cpu_idle();
    }

    klog("VMM: %u timer ticks seen\n", timer_ticks());
    kernel_shutdown(0);
}
