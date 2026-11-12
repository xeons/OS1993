/*
 * panic.c -- the fatal error screen. White on blue, naturally.
 */
#include "kernel.h"
#include "console.h"
#include "devices.h"
#include "interrupts.h"
#include "io.h"
#include "vga.h"
#include "gdt.h"
#include "sched.h"
#include "kdbg.h"

static const char *const exc_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow",
    "BOUND Range Exceeded", "Invalid Opcode", "Coprocessor Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
    "Segment Not Present", "Stack Fault", "General Protection Fault",
    "Page Fault", "Reserved", "Coprocessor Error", "Alignment Check",
};

#define FATAL_ATTR ATTR(C_WHITE, C_BLUE)

static void fatal_begin(void)
{
    cpu_cli();
    vga_force_text();                     /* out of the graphics desktop */
    con_attach();                         /* take the screen back from any VM */
    con_set_window(0, CON_ROWS - 1);
    con_set_attr(FATAL_ATTR);
    con_clear();
    con_show_cursor(0);
    con_write_at(2, 36, ATTR(C_BLUE, C_LGRAY), " OS1993 ");
    con_goto(5, 0);
}

__attribute__((noreturn))
static void fatal_end(void)
{
    kprintf("\n\n     Press any key to return to DOS.\n");
    klog("*** FATAL: system halted\n");
    if (g_bootinfo && (g_bootinfo->flags & BI_FLAG_TEST))
        kernel_shutdown(0xFF);            /* automated run: report failure */
    keyboard_poll_key();
    kernel_shutdown(0xFF);
}

void panic_exception(const struct regs *r)
{
    kdbg_set_fault_regs(r);
    kdbg_panic("a fatal exception in the VMM");
    fatal_begin();
    const char *name = r->int_no < 32 && exc_names[r->int_no]
                     ? exc_names[r->int_no] : "Unknown";
    kprintf("     A fatal exception %02X (%s) has occurred at %04X:%08X.\n\n",
            r->int_no, name, r->cs & 0xFFFF, r->eip);
    kprintf("     EAX=%08X  EBX=%08X  ECX=%08X  EDX=%08X\n", r->eax, r->ebx, r->ecx, r->edx);
    kprintf("     ESI=%08X  EDI=%08X  EBP=%08X  ESP=%08X\n", r->esi, r->edi, r->ebp,
            (uint32_t)(uintptr_t)&r->user_esp);
    kprintf("     DS=%04X ES=%04X FS=%04X GS=%04X  EFLAGS=%08X  ERR=%08X\n",
            r->ds & 0xFFFF, r->es & 0xFFFF, r->fs & 0xFFFF, r->gs & 0xFFFF,
            r->eflags, r->err_code);
    kprintf("     CR0=%08X  CR2=%08X  CR3=%08X\n", read_cr0(), read_cr2(), read_cr3());
    if (r->int_no == 14) {
        kprintf("\n     Page fault: %s %08X in %s mode (%s).\n",
                (r->err_code & 2) ? "write to" : "read from",
                read_cr2(),
                (r->err_code & 4) ? "user" : "kernel",
                (r->err_code & 1) ? "protection violation" : "page not present");
    }
    fatal_end();
}

/* The double-fault task (isr.asm): the state that faulted is in
 * kernel_tss. A fault on a guard page is a kernel stack overflow. */
__attribute__((noreturn))
void double_fault(void)
{
    uint32_t cr2 = read_cr2();
    struct thread *t = thread_of_guard(kernel_tss.esp);
    if (!t) t = thread_of_guard(cr2);
    klog("*** double fault at %04X:%08X ESP=%08X CR2=%08X\n",
         kernel_tss.cs & 0xFFFF, kernel_tss.eip, kernel_tss.esp, cr2);
    struct regs r = {
        .eax = kernel_tss.eax, .ebx = kernel_tss.ebx, .ecx = kernel_tss.ecx, .edx = kernel_tss.edx,
        .esi = kernel_tss.esi, .edi = kernel_tss.edi, .ebp = kernel_tss.ebp,
        .ds = kernel_tss.ds, .es = kernel_tss.es, .fs = kernel_tss.fs, .gs = kernel_tss.gs,
        .int_no = 8, .eip = kernel_tss.eip, .cs = kernel_tss.cs, .eflags = kernel_tss.eflags,
        .user_esp = kernel_tss.esp, .user_ss = kernel_tss.ss,
    };
    kdbg_set_fault_regs(&r);
    if (t)
        panic("Kernel stack overflow in thread %s (EIP %08X, ESP %08X).",
              t->name, kernel_tss.eip, kernel_tss.esp);
    panic("Double fault at %04X:%08X (ESP %08X, CR2 %08X).",
          kernel_tss.cs & 0xFFFF, kernel_tss.eip, kernel_tss.esp, cr2);
}

void panic(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    klog("*** panic: %s\n", buf);
    kdbg_panic(buf);
    fatal_begin();
    kprintf("     The Virtual Machine Manager has stopped:\n\n     %s\n", buf);
    fatal_end();
}
