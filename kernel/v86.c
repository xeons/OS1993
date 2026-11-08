/*
 * v86.c -- the V86 monitor.
 *
 * V86 code runs at IOPL 0, so the IOPL-sensitive instructions (CLI, STI,
 * PUSHF, POPF, INT n, IRET) and HLT fault with #GP, and we emulate them.
 * Each VM's interrupt flag is virtual (vm->vif); the real IF stays on so
 * hardware interrupts always reach the kernel. Port I/O is checked against
 * the TSS I/O bitmap: only the PIC (20h/21h/A0h/A1h) and keyboard
 * controller (60h/64h) ports trap, to the per-VM virtual devices.
 *
 * A kernel V86 call (v86_int) pushes an INT-style frame whose return
 * address is an ARPL instruction in the loader's segment. ARPL is invalid
 * outside protected mode, so the handler's final IRET lands on it, raises
 * #UD, and we know the call is complete.
 *
 * A VM that executes HLT with nothing to deliver blocks its thread until an
 * interrupt is raised for it, so idle VMs cost nothing.
 *
 * Not yet emulated (the VM is stopped with a message): code that tries to
 * enter protected mode itself (LMSW, MOV CRn, LGDT), e.g. DOS extenders or
 * a real HIMEM's block move. That needs VCPI/DPMI support.
 */
#include "kernel.h"
#include "gdt.h"
#include "io.h"
#include "mm.h"
#include "sched.h"
#include "v86.h"
#include "vm.h"

/* The loader's segment, identical in every VM's address space. */
static struct {
    bool     ready;
    uint16_t seg, stub_off, stack_top;
    uint16_t buf_off, buf_size;
} g;

static inline uint8_t *lin(uint32_t seg, uint32_t off)
{
    return (uint8_t *)(uintptr_t)(((seg & 0xFFFF) << 4) + (off & 0xFFFF));
}

/* ---- the VM's stack and flags ------------------------------------------ */

static void push16(struct regs *r, uint16_t v)
{
    r->user_esp = (r->user_esp - 2) & 0xFFFF;
    *(uint16_t *)lin(r->user_ss, r->user_esp) = v;
}

static uint16_t pop16(struct regs *r)
{
    uint16_t v = *(uint16_t *)lin(r->user_ss, r->user_esp);
    r->user_esp = (r->user_esp + 2) & 0xFFFF;
    return v;
}

static void push32(struct regs *r, uint32_t v)
{
    push16(r, (uint16_t)(v >> 16));
    push16(r, (uint16_t)v);
}

static uint32_t pop32(struct regs *r)
{
    uint32_t lo = pop16(r);
    return lo | (uint32_t)pop16(r) << 16;
}

/* FLAGS as the VM should see them: virtual IF, IOPL and NT. */
static uint32_t vm_flags(const struct vm *vm, const struct regs *r)
{
    uint32_t f = r->eflags & ~(EFLAGS_IF | EFLAGS_IOPL | EFLAGS_NT | EFLAGS_VM | 0x10000u);
    return f | (vm->vif ? EFLAGS_IF : 0) | vm->vflags;
}

static void set_vm_flags(struct vm *vm, struct regs *r, uint32_t f)
{
    const uint32_t mask = 0x0DD5;   /* CF PF AF ZF SF TF DF OF */
    vm->vif = f & EFLAGS_IF;
    vm->vflags = (uint16_t)(f & (EFLAGS_IOPL | EFLAGS_NT));
    r->eflags = (r->eflags & ~mask) | (f & mask) | EFLAGS_IF | EFLAGS_VM;
}

/* Simulate INT n: push FLAGS/CS/IP, clear IF and TF, jump via the IVT. */
static void reflect_int(struct vm *vm, struct regs *r, uint8_t n)
{
    push16(r, (uint16_t)vm_flags(vm, r));
    push16(r, (uint16_t)r->cs);
    push16(r, (uint16_t)r->eip);
    vm->vif = false;
    r->eflags &= ~EFLAGS_TF;
    r->eip = LOWMEM16(n * 4u);          /* this VM's IVT (PDE 0 is ours) */
    r->cs  = LOWMEM16(n * 4u + 2);
}

void v86_deliver(struct regs *r)
{
    struct vm *vm = vm_current();
    if (!(r->eflags & EFLAGS_VM) || !vm->vif)
        return;
    int vec = vpic_ack(&vm->pic);
    if (vec >= 0) {
        vm->delivered[vec]++;
        reflect_int(vm, r, (uint8_t)vec);
    }
}

/* ---- ending a call ----------------------------------------------------- */

__attribute__((noreturn))
static void crash(struct vm *vm, const struct regs *r, const char *why)
{
    const uint8_t *code = lin(r->cs, r->eip);
    ksnprintf(vm->error, sizeof vm->error, "%s at %04X:%04X (%02X %02X %02X %02X)",
              why, r->cs & 0xFFFF, r->eip & 0xFFFF, code[0], code[1], code[2], code[3]);
    klog("V86: VM %d stopped: %s\n", vm->id, vm->error);
    vpic_reset_in_service(&vm->pic);
    v86_leave(-1, vm->kernel_esp);
}

__attribute__((noreturn))
static void complete(struct vm *vm, const struct regs *r)
{
    struct v86_regs *o = vm->out;
    o->eax = r->eax; o->ebx = r->ebx; o->ecx = r->ecx; o->edx = r->edx;
    o->esi = r->esi; o->edi = r->edi; o->ebp = r->ebp;
    o->ds = (uint16_t)r->v86_ds;
    o->es = (uint16_t)r->v86_es;
    o->flags = (uint16_t)vm_flags(vm, r);
    v86_leave(0, vm->kernel_esp);
}

/* ---- port I/O ---------------------------------------------------------- */

static uint8_t port_in8(struct vm *vm, uint16_t p)
{
    if (vpic_port(p)) return vpic_in(&vm->pic, p);
    if (vkbd_port(p)) return vkbd_in(vm, p);
    return inb(p);
}

static void port_out8(struct vm *vm, uint16_t p, uint8_t v)
{
    if (vpic_port(p))      vpic_out(&vm->pic, p, v);
    else if (vkbd_port(p)) vkbd_out(vm, p, v);
    else                   outb(p, v);
}

static uint32_t port_in(struct vm *vm, uint16_t port, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v |= (uint32_t)port_in8(vm, (uint16_t)(port + i)) << (8 * i);
    return v;
}

static void port_out(struct vm *vm, uint16_t port, int size, uint32_t v)
{
    for (int i = 0; i < size; i++)
        port_out8(vm, (uint16_t)(port + i), (uint8_t)(v >> (8 * i)));
}

static void set_acc(struct regs *r, int size, uint32_t v)
{
    if (size == 1)      r->eax = (r->eax & ~0xFFu) | (v & 0xFF);
    else if (size == 2) r->eax = (r->eax & ~0xFFFFu) | (v & 0xFFFF);
    else                r->eax = v;
}

/* ---- #GP: instruction emulation ---------------------------------------- */

static void emulate(struct vm *vm, struct regs *r)
{
    uint32_t ip = r->eip & 0xFFFF;
    bool o32 = false;
    uint8_t op;

    for (int n = 0;; n++) {                     /* prefixes */
        op = *lin(r->cs, ip);
        if (n > 8)
            crash(vm, r, "too many prefixes");
        if (op == 0x66) { o32 = true; ip++; continue; }
        if (op == 0x67 || op == 0xF0 || op == 0xF2 || op == 0xF3 || op == 0x26 ||
            op == 0x2E || op == 0x36 || op == 0x3E || op == 0x64 || op == 0x65) { ip++; continue; }
        break;
    }
    ip = (ip + 1) & 0xFFFF;
    int iosize = o32 ? 4 : 2;

    switch (op) {
    case 0xFA:                                  /* CLI */
        vm->vif = false;
        break;
    case 0xFB:                                  /* STI */
        vm->vif = true;
        break;
    case 0x9C:                                  /* PUSHF / PUSHFD */
        if (o32) push32(r, vm_flags(vm, r));
        else     push16(r, (uint16_t)vm_flags(vm, r));
        break;
    case 0x9D:                                  /* POPF / POPFD */
        set_vm_flags(vm, r, o32 ? pop32(r) : pop16(r));
        break;
    case 0xCD:                                  /* INT n */
        r->eip = (ip + 1) & 0xFFFF;
        reflect_int(vm, r, *lin(r->cs, ip));
        return;
    case 0xCC:                                  /* INT 3 */
        r->eip = ip;
        reflect_int(vm, r, 3);
        return;
    case 0xCE:                                  /* INTO */
        r->eip = ip;
        if (r->eflags & EFLAGS_OF)
            reflect_int(vm, r, 4);
        return;
    case 0xCF: {                                /* IRET / IRETD */
        uint32_t nip = o32 ? pop32(r) : pop16(r);
        uint32_t ncs = o32 ? pop32(r) : pop16(r);
        uint32_t nfl = o32 ? pop32(r) : pop16(r);
        r->eip = nip & 0xFFFF;
        r->cs  = ncs & 0xFFFF;
        set_vm_flags(vm, r, nfl);
        return;
    }
    case 0xF4:                                  /* HLT: block until an IRQ */
        r->eip = ip;
        if (!vm->vif)
            crash(vm, r, "HLT with interrupts disabled");
        while (!vpic_has_deliverable(&vm->pic)) {
            current->state = T_BLOCKED;
            schedule();
        }
        return;
    case 0xE4: set_acc(r, 1, port_in(vm, *lin(r->cs, ip), 1)); ip++; break;
    case 0xE5: set_acc(r, iosize, port_in(vm, *lin(r->cs, ip), iosize)); ip++; break;
    case 0xE6: port_out(vm, *lin(r->cs, ip), 1, r->eax); ip++; break;
    case 0xE7: port_out(vm, *lin(r->cs, ip), iosize, r->eax); ip++; break;
    case 0xEC: set_acc(r, 1, port_in(vm, (uint16_t)r->edx, 1)); break;
    case 0xED: set_acc(r, iosize, port_in(vm, (uint16_t)r->edx, iosize)); break;
    case 0xEE: port_out(vm, (uint16_t)r->edx, 1, r->eax); break;
    case 0xEF: port_out(vm, (uint16_t)r->edx, iosize, r->eax); break;
    case 0x0F:
        crash(vm, r, "VM tried a protected-mode instruction");
    default:
        crash(vm, r, "unhandled privileged instruction");
    }
    r->eip = ip & 0xFFFF;
}

void v86_exception(struct regs *r)
{
    struct vm *vm = vm_current();
    if (!vm->active)
        panic("V86 trap %02X in VM %d with no call active.", r->int_no, vm->id);

    switch (r->int_no) {
    case 13:                                    /* #GP: emulate */
        emulate(vm, r);
        break;
    case 6:                                     /* #UD */
        if ((r->cs & 0xFFFF) == g.seg && (r->eip & 0xFFFF) == g.stub_off)
            complete(vm, r);
        reflect_int(vm, r, 6);
        break;
    case 0: case 1: case 3: case 4: case 5: case 7:
        reflect_int(vm, r, (uint8_t)r->int_no); /* as on a real-mode CPU */
        break;
    default: {
        char why[32];
        ksnprintf(why, sizeof why, "exception %02X", r->int_no);
        crash(vm, r, why);
    }
    }
    v86_deliver(r);
}

/* ---- public API -------------------------------------------------------- */

void v86_init(const struct bootinfo *bi)
{
    g.seg       = (uint16_t)(bi->loader_base >> 4);
    g.stub_off  = bi->v86_stub_off;
    g.stack_top = bi->v86_stack_top;
    g.buf_off   = bi->buf_off;
    g.buf_size  = bi->buf_size;

    /* Let ring 3 (V86) code reach the first 1 MB + HMA; the kernel's own
     * pages stay supervisor-only, so a VM can't touch them. */
    volatile uint32_t *pte = (volatile uint32_t *)PT_WINDOW;
    volatile uint32_t *pde = (volatile uint32_t *)PD_WINDOW;
    for (unsigned i = 0; i < VM_V86_PAGES; i++)
        pte[i] |= PTE_U;
    pde[0] |= PTE_U;
    tlb_flush();

    /* Virtualized ports; everything else goes straight to the hardware. */
    static const uint16_t trapped[] = { 0x20, 0x21, 0xA0, 0xA1, 0x60, 0x64 };
    for (unsigned i = 0; i < ARRAY_SIZE(trapped); i++)
        tss_trap_port(trapped[i], true);

    vm_system_init(bi);

    /* IRQs DOS had enabled now reach the VMs through the virtual PICs. */
    for (unsigned irq = 0; irq < 16; irq++)
        if (!((bi->pic1_mask | bi->pic2_mask << 8) & (1u << irq)))
            pic_unmask(irq);

    g.ready = true;
}

/* Run V86 code at cs:ip in the current VM, with `frame_words` already
 * pushed below the V86 stack top (a return frame to the ARPL breakpoint). */
static int run(struct v86_regs *r, uint16_t cs, uint16_t ip, unsigned frame_words, bool vif)
{
    struct vm *vm = vm_current();
    if (!g.ready)
        panic("V86 call before v86_init.");
    if (vm->active)
        panic("Nested V86 call to %04X:%04X in VM %d.", cs, ip, vm->id);

    struct v86_frame f = {
        .eax = r->eax, .ebx = r->ebx, .ecx = r->ecx, .edx = r->edx,
        .esi = r->esi, .edi = r->edi, .ebp = r->ebp,
        .eip = ip, .cs = cs,
        .eflags = EFLAGS_VM | EFLAGS_IF | 0x2,
        .esp = (uint16_t)(g.stack_top - frame_words * 2), .ss = g.seg,
        .es = r->es, .ds = r->ds, .fs = 0, .gs = 0,
    };

    vm->out = r;
    vm->vif = vif;
    vm->vflags = 0;
    vm->error[0] = '\0';
    vm->active = true;
    int rc = v86_enter(&f, &vm->kernel_esp);
    vm->active = false;
    return rc;
}

int v86_int(uint8_t vec, struct v86_regs *r)
{
    uint16_t *sp = v86_ptr(g.seg, (uint16_t)(g.stack_top - 6));
    sp[0] = g.stub_off;                         /* IRET frame -> breakpoint */
    sp[1] = g.seg;
    sp[2] = 0x0202;                             /* IF=1 once the handler IRETs */
    return run(r, LOWMEM16(vec * 4u + 2), LOWMEM16(vec * 4u),   /* IVT: CS, IP */
               3, false);                       /* INT clears IF */
}

int v86_call_far(uint16_t seg, uint16_t off, struct v86_regs *r)
{
    uint16_t *sp = v86_ptr(g.seg, (uint16_t)(g.stack_top - 4));
    sp[0] = g.stub_off;                         /* RETF frame -> breakpoint */
    sp[1] = g.seg;
    return run(r, seg, off, 2, true);
}

const char *v86_last_error(void)      { return vm_current()->error; }
uint32_t v86_delivered(uint8_t vec)   { return vm_current()->delivered[vec]; }
uint16_t v86_buf_seg(void)  { return g.seg; }
uint16_t v86_buf_off(void)  { return g.buf_off; }
uint16_t v86_buf_size(void) { return g.buf_size; }
