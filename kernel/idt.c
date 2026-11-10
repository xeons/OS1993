#include "kernel.h"
#include "devices.h"
#include "random.h"
#include "gdt.h"
#include "interrupts.h"
#include "sched.h"
#include "v86.h"
#include "vm.h"
#include "user.h"

struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t off_hi;
} __attribute__((packed));

#define IDT_INT_GATE32 0x8E      /* present, DPL 0, 32-bit interrupt gate */
#define IDT_USER_GATE32 0xEE     /* present, DPL 3: callable with INT from ring 3 */

static struct idt_entry idt[256];
static irq_handler_t irq_handlers[16];

extern const uint32_t isr_stub_table[49];

static void idt_set(int vec, uint32_t handler, uint8_t flags)
{
    idt[vec].off_lo = handler & 0xFFFF;
    idt[vec].sel    = KCODE_SEL;
    idt[vec].zero   = 0;
    idt[vec].flags  = flags;
    idt[vec].off_hi = handler >> 16;
}

void idt_init(void)
{
    for (int i = 0; i < 48; i++)
        idt_set(i, isr_stub_table[i], IDT_INT_GATE32);
    idt_set(SYSCALL_VECTOR, isr_stub_table[48], IDT_USER_GATE32);

    static struct dtr idtr;
    idtr.limit = sizeof idt - 1;
    idtr.base  = (uint32_t)(uintptr_t)idt;
    idt_load(&idtr);
}

void irq_install(unsigned irq, irq_handler_t h)
{
    irq_handlers[irq] = h;
    pic_unmask(irq);
}

void irq_uninstall(unsigned irq)
{
    irq_handlers[irq] = NULL;           /* the IRQ now belongs to the VM */
}

/*
 * IRQs with a kernel handler belong to the kernel; all others are passed to
 * the DOS VM through the virtual PIC. (A kernel handler may also share its
 * IRQ with the VM by calling vpic_raise itself, as the timer does.)
 */
void interrupt_dispatch(struct regs *r)
{
    bool from_v86 = r->eflags & EFLAGS_VM;
    bool from_user = !from_v86 && (r->cs & 3) == 3;
    if (from_v86)
        vm_check("on entry from V86");

    if (r->int_no == SYSCALL_VECTOR) {
        if (!from_user)
            panic("System call from ring 0.");
        syscall_dispatch(r);
        sched_preempt();
        user_return(r);
        return;
    }

    if (r->int_no < IRQ_BASE) {
        if (from_v86) {
            v86_exception(r);           /* the V86 monitor */
            vm_check("returning to V86 after a trap");
        } else if (from_user) {
            user_fault(r);              /* kills the ring-3 process */
            user_return(r);
        } else {
            panic_exception(r);
        }
        return;
    }

    unsigned irq = r->int_no - IRQ_BASE;
    if (irq < 16) {
        if (pic_is_spurious(irq))
            return;
        random_mix(r->eip ^ r->esp_pushad ^ r->eax ^ irq << 24 ^ timer_ticks() << 8);
        if (irq_handlers[irq])
            irq_handlers[irq](r);
        else
            vm_hw_irq(irq);
        pic_eoi(irq);
        if (from_v86)
            v86_deliver(r);
        sched_preempt();            /* after the EOI: may switch threads */
        if (from_user)
            user_return(r);
        if (from_v86)
            vm_check("returning to V86 after an IRQ");
    }
}
