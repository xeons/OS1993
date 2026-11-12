/*
 * v86.c -- the V86 monitor.
 *
 * V86 code runs at IOPL 0, so the IOPL-sensitive instructions (CLI, STI,
 * PUSHF, POPF, INT n, IRET) and HLT fault with #GP, and we emulate them.
 * Each VM's interrupt flag is virtual (vm->vif); the real IF stays on so
 * hardware interrupts always reach the kernel. Port I/O is checked against
 * the TSS I/O bitmap: the PIC (20h/21h/A0h/A1h), keyboard controller
 * (60h/64h) and timer (40h/43h) ports trap to the per-VM virtual devices,
 * COM and LPT ports to their owner arbitration (vdev.c), and an IDE
 * channel the VMM drives itself (ata.c) reads as empty.
 *
 * A kernel V86 call (v86_int) pushes an INT-style frame whose return
 * address is an ARPL instruction in the loader's segment. ARPL is invalid
 * outside protected mode, so the handler's final IRET lands on it, raises
 * #UD, and we know the call is complete.
 *
 * A VM that executes HLT with nothing to deliver blocks its thread until an
 * interrupt is raised for it, so idle VMs cost nothing.
 *
 * Code that tries to enter protected mode by itself (LMSW, MOV CRn, LGDT)
 * stops the VM with a message: VMs get DPMI (dpmi.c) and XMS (xms.c) from
 * the VMM instead, and a DOS extender that sees V86 mode asks for them.
 */
#include "kernel.h"
#include "devices.h"
#include "disk.h"
#include "fs.h"
#include "gdt.h"
#include "io.h"
#include "mm.h"
#include "sched.h"
#include "v86.h"
#include "vm.h"

/* The loader's segment, identical in every VM's address space. */
#define BRK_AREA 64                 /* at the end of the transfer buffer: monitor breakpoints */
static struct {
    bool     ready;
    uint16_t seg, stub_off, stack_top;
    uint16_t buf_off, buf_size;
    uint16_t brk_base;
    v86_brk_fn brk[BRK_MAX];
} g;

uint16_t v86_brk_off(int n) { return (uint16_t)(g.brk_base + 2 * n); }
void v86_set_brk(int n, v86_brk_fn fn) { g.brk[n] = fn; }

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

void v86_push16(struct regs *r, uint16_t v) { push16(r, v); }
uint16_t v86_pop16(struct regs *r) { return pop16(r); }

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

/* IOPL reads as 3 and can't be changed, as in V86 mode under EMM386: a
 * program that tries to clear it (the usual test for real mode) learns it
 * is in V86 mode, and a DOS extender then looks for VCPI or DPMI instead
 * of switching modes itself. */
static void set_vm_flags(struct vm *vm, struct regs *r, uint32_t f)
{
    const uint32_t mask = 0x0DD5;   /* CF PF AF ZF SF TF DF OF */
    vm->vif = f & EFLAGS_IF;
    vm->vflags = (uint16_t)((f & EFLAGS_NT) | EFLAGS_IOPL);
    r->eflags = (r->eflags & ~mask) | (f & mask) | EFLAGS_IF | EFLAGS_VM;
}

/* Simulate INT n: push FLAGS and a return address, clear IF and TF, jump
 * via the IVT. */
static void reflect_int_to(struct vm *vm, struct regs *r, uint8_t n, uint16_t ret_cs, uint16_t ret_ip)
{
    push16(r, (uint16_t)vm_flags(vm, r));
    push16(r, ret_cs);
    push16(r, ret_ip);
    vm->vif = false;
    r->eflags &= ~EFLAGS_TF;
    r->eip = LOWMEM16(n * 4u);          /* this VM's IVT (PDE 0 is ours) */
    r->cs  = LOWMEM16(n * 4u + 2);
}

static void reflect_int(struct vm *vm, struct regs *r, uint8_t n)
{
    reflect_int_to(vm, r, n, (uint16_t)r->cs, (uint16_t)r->eip);
}

uint32_t v86_vm_flags(const struct vm *vm, const struct regs *r) { return vm_flags(vm, r); }
void v86_set_vm_flags(struct vm *vm, struct regs *r, uint32_t f) { set_vm_flags(vm, r, f); }
void v86_reflect(struct vm *vm, struct regs *r, uint8_t n) { reflect_int(vm, r, n); }

void v86_deliver(struct regs *r)
{
    struct vm *vm = vm_current();
    if (!(r->eflags & EFLAGS_VM) || !vm->vif)
        return;
    if (vm->dpmi) {                             /* its client may have hooked the vector */
        dpmi_deliver(r);
        if (!(r->eflags & EFLAGS_VM) || !vm->vif)
            return;
    }
    int vec = vpic_ack(&vm->pic);
    if (vec >= 0) {
        vm->delivered[vec]++;
        reflect_int(vm, r, (uint8_t)vec);
        return;
    }
    vmouse_deliver(vm, r);                      /* an INT 33h event handler to call? */
}

/* ---- idle detection ----------------------------------------------------- */

/*
 * A keyboard poll that found nothing (INT 16h AH=01h/11h, or the
 * controller's status port). A VM that polls more than set.idle times in
 * one timer tick is busy-waiting for a key: it sleeps until the next tick
 * or a key, whichever comes first, and the other VMs get the time.
 */
void vm_idle_poll(struct vm *vm)
{
    if (!vm->set.idle || vm == system_vm)
        return;
    uint32_t t = timer_ticks();
    if (t != vm->poll_tick) {
        vm->poll_tick = t;
        vm->polls = 0;
    }
    if (++vm->polls < vm->set.idle)
        return;
    vm->polls = 0;
    uint32_t f = irq_save();
    if (!vpic_has_deliverable(&vm->pic) && vm->kbd.head == vm->kbd.tail && !vm->kbd.full) {
        vm->idled++;
        current->state = T_BLOCKED;         /* the tick, or a key, wakes it */
        schedule();
    }
    irq_restore(f);
}

static bool kbd_empty(struct vm *vm)
{
    return LOWMEM16(0x41A) == LOWMEM16(0x41C) && vm->kbd.head == vm->kbd.tail && !vm->kbd.full;
}

/* ---- ending a call ----------------------------------------------------- */

__attribute__((noreturn))
static void crash(struct vm *vm, const struct regs *r, const char *why)
{
    const uint8_t *code = lin(r->cs, r->eip);
    ksnprintf(vm->error, sizeof vm->error, "%s at %04X:%04X (%02X %02X %02X %02X)",
              why, r->cs & 0xFFFF, r->eip & 0xFFFF, code[0], code[1], code[2], code[3]);
    klog("V86: VM %d stopped: %s\n", vm->id, vm->error);
    klog("V86:   EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X\n",
         r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp);
    klog("V86:   SS:SP=%04X:%04X DS=%04X ES=%04X FL=%08X ERR=%X CR2=%08X CR3=%08X PDE0=%08X\n",
         r->user_ss & 0xFFFF, r->user_esp & 0xFFFF, r->v86_ds & 0xFFFF, r->v86_es & 0xFFFF,
         r->eflags, r->err_code, read_cr2(), read_cr3(),
         ((volatile uint32_t *)PD_WINDOW)[0]);
    klog("V86:   VM %d pt0=%08X, DOS depth %d, idle %d\n",
         vm->id, vm->pt0_phys, vm->dos_depth, vm->int28_depth);
    dosmgr_dump_trace(vm);
    vpic_reset_in_service(&vm->pic);
    vm->dos_depth = 0;
    vm->int28_depth = 0;
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
    if (vpit_port(p)) return vpit_in(vm, p);
    if (vdev_port(p)) return vdev_in(vm, p);
    if (ata_port_owned(p)) return 0xFF;         /* the VMM's disk channel: nothing there */
    return inb(p);
}

static void port_out8(struct vm *vm, uint16_t p, uint8_t v)
{
    if (vpic_port(p)) {
        vpic_out(&vm->pic, p, v);
        if (p & 1)
            vdev_imr_changed(vm);               /* its devices' IRQs, on the real PIC */
    }
    else if (vkbd_port(p)) vkbd_out(vm, p, v);
    else if (vpit_port(p)) vpit_out(vm, p, v);
    else if (vdev_port(p)) vdev_out(vm, p, v);
    else if (!ata_port_owned(p)) outb(p, v);
}

uint8_t v86_port_in8(struct vm *vm, uint16_t port) { return port_in8(vm, port); }
void v86_port_out8(struct vm *vm, uint16_t port, uint8_t v) { port_out8(vm, port, v); }

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

static bool mm_breakpoint(struct vm *vm, struct regs *r, uint16_t at);

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
    case 0xCD: {                                /* INT n */
        uint8_t n = *lin(r->cs, ip);
        uint16_t next = (uint16_t)(ip + 1);
        if (n == 0x21 && (r->eax & 0xFF00) == 0x7100) {
            int how = lfn_int21(vm, r);         /* a long-name call (lfn.c) */
            if (how == LFN_DONE) {
                r->eip = next;
                return;
            }
            if (how == LFN_REWRITE) {           /* as an INT to its routine, which calls DOS */
                uint16_t seg, off = lfn_stub(&seg);
                push16(r, (uint16_t)vm_flags(vm, r));
                push16(r, (uint16_t)r->cs);
                push16(r, next);
                r->cs = seg;
                r->eip = off;
                return;
            }
        }
        if (n == 0x2F && (r->eax & 0xFF00) == 0x1100 && redir_int2f(vm, r)) {
            r->eip = next;                      /* the redirector answered (redir.c) */
            return;
        }
        if (vm->dpmi && (n == 0x20 || n == 0x21))
            dpmi_rm_int(vm, r, n);              /* its client's program ending? */
        if ((n == 0x2F && (xms_int2f(vm, r) || dpmi_int2f(vm, r))) || (n == 0x15 && xms_int15(vm, r))) {
            r->eip = next;                      /* XMS and extended memory (xms.c) */
            return;
        }
        if (n == 0x67 && (g_bootinfo->flags & BI_FLAG_VCPI)) {
            r->eip = next;                      /* the memory manager sleeps: no EMS, no VCPI */
            r->eax = (r->eax & 0xFFFF00FFu) | 0x8400;
            return;
        }
        if (n == 0x33 && vmouse_int33(vm, r)) {
            r->eip = next;                      /* the virtual mouse driver (vmouse.c) */
            return;
        }
        struct disk *d;
        if (n == 0x13 && (r->edx & 0x80) && (d = ata_bios_disk((uint8_t)r->edx))) {
            r->eip = next;                      /* the VMM's disk: it does the call (int13.c) */
            int13_emulate(d, r);
            return;
        }
        uint8_t ah = (uint8_t)(r->eax >> 8);
        bool empty_poll = n == 0x16 && (ah == 0x01 || ah == 0x11) && kbd_empty(vm);
        int how = dosmgr_int(vm, r, n, next);
        if (how != DOSMGR_DONE && empty_poll)
            vm_idle_poll(vm);                   /* busy-waiting for a key? */
        switch (how) {
        case DOSMGR_DONE:
            return;
        case DOSMGR_HOOK:                       /* DOS returns to our breakpoint */
            r->eip = next;
            reflect_int_to(vm, r, n, g.seg, (uint16_t)(g.stub_off + 2));
            return;
        default:
            r->eip = next;
            reflect_int(vm, r, n);
            return;
        }
    }
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
        if (mm_breakpoint(vm, r, (uint16_t)(ip - 1)))
            return;                             /* (a sleeping memory manager's) */
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

/* A CPU exception in V86 code goes to the VM's own handler, as on a real
 * 8086; log it, since it usually means something went badly wrong. */
static void log_reflected(struct vm *vm, const struct regs *r)
{
    const uint8_t *c = lin(r->cs, r->eip);
    klog("V86: VM %d exception %02X at %04X:%04X (%02X %02X %02X %02X) SS:SP=%04X:%04X, "
         "DOS depth %d, idle %d -- reflected\n",
         vm->id, r->int_no, r->cs & 0xFFFF, r->eip & 0xFFFF, c[0], c[1], c[2], c[3],
         r->user_ss & 0xFFFF, r->user_esp & 0xFFFF, vm->dos_depth, vm->int28_depth);

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
        if ((r->cs & 0xFFFF) == g.seg && (r->eip & 0xFFFF) >= g.brk_base &&
            (r->eip & 0xFFFF) < (uint32_t)(g.brk_base + 2 * BRK_MAX) && !((r->eip - g.brk_base) & 1)) {
            int n = (int)((r->eip & 0xFFFF) - g.brk_base) / 2;
            if (!g.brk[n])
                crash(vm, r, "monitor breakpoint with no service");
            g.brk[n](vm, r, n);                 /* an entry point, or a callback's return */
            break;
        }
        if ((r->cs & 0xFFFF) == g.seg && (r->eip & 0xFFFF) == (uint16_t)(g.stub_off + 2)) {
            if (!dosmgr_return(vm, r))          /* a DOS call returned */
                crash(vm, r, "unexpected return from DOS");
            break;
        }
        log_reflected(vm, r);
        reflect_int(vm, r, 6);
        break;
    case 0: case 1: case 3: case 4: case 5: case 7:
        if (r->int_no != 1 && r->int_no != 3)
            log_reflected(vm, r);
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

/* ---- under a memory manager (VCPI) ----------------------------------------- */

/*
 * EMM386-style memory managers hook interrupts with real-mode stubs that
 * are rows of HLT instructions: breakpoints into their own monitor, asleep
 * while OS1993 runs. Such a vector goes back to the BIOS's fixed entry
 * point for it (every PC BIOS keeps them, for compatibility) for as long
 * as OS1993 runs, and the memory manager's comes back at the end. VDS
 * (INT 4Bh) is the memory manager's alone: its flag in the BIOS data area
 * goes off.
 */
static uint32_t mm_saved[256];
static bool mm_unhooked;
static uint16_t mm_seg;                     /* the memory manager's real-mode segment */
static uint16_t mm_bios[256];               /* its breakpoint at offset n -> a BIOS entry (F000:x) */

static void vcpi_unhook(void)
{
    uint16_t mm = LOWMEM16(0x67 * 4 + 2);
    if (!mm)
        return;
    static const struct { uint8_t vec; uint16_t off; } bios[] = {
        { 0x13, 0xEC59 }, { 0x40, 0xEC59 }, { 0x15, 0xF859 }, { 0x19, 0xE6F2 }, { 0x4B, 0xFF53 },
    };
    for (unsigned v = 0; v < 256; v++) {
        if (LOWMEM16(v * 4 + 2) != mm || v == 0x67)
            continue;
        mm_saved[v] = LOWMEM32(v * 4);
        unsigned i = 0;
        while (i < ARRAY_SIZE(bios) && bios[i].vec != v) i++;
        if (i < ARRAY_SIZE(bios)) {
            if ((mm_saved[v] & 0xFFFF) < 256)
                mm_bios[mm_saved[v] & 0xFFFF] = bios[i].off;
            LOWMEM16(v * 4) = bios[i].off;
            LOWMEM16(v * 4 + 2) = 0xF000;
            klog("V86: INT %02Xh: the memory manager's (%04X:%04X) -> the BIOS's, F000:%04X\n",
                 v, mm, mm_saved[v] & 0xFFFF, bios[i].off);
        } else {
            klog("V86: INT %02Xh stays the memory manager's (%04X:%04X)\n", v, mm, mm_saved[v] & 0xFFFF);
            mm_saved[v] = 0;
        }
    }
    LOWMEM8(0x47B) &= (uint8_t)~0x20;           /* no VDS */
    mm_seg = mm;
    mm_unhooked = true;
}

/*
 * V86 code ran into one of the memory manager's HLT breakpoints anyway --
 * through a pointer saved before we moved the vectors, or through the XMS
 * entry point, which EMM386-style managers hook to emulate A20. An
 * interrupt's breakpoint goes on to the BIOS (the INT frame is already on
 * the stack); anything else is taken for the XMS hook: A20 is always on
 * under OS1993, so its calls succeed, and the rest are refused.
 */
static bool mm_breakpoint(struct vm *vm, struct regs *r, uint16_t at)
{
    if (!mm_seg || (uint16_t)r->cs != mm_seg || at >= 256)
        return false;
    if (mm_bios[at]) {
        r->cs = 0xF000;
        r->eip = mm_bios[at];
        return true;
    }
    uint8_t ah = (uint8_t)(r->eax >> 8);
    static bool told;
    if (!told) {
        told = true;
        klog("V86: VM %d: the memory manager's breakpoint at %04X:%04X, AH=%02X: taken for its XMS hook\n",
             vm->id, mm_seg, at, ah);
    }
    if (ah >= 0x03 && ah <= 0x07) {             /* A20: on, always */
        r->eax = (r->eax & 0xFFFF0000u) | 1;
        r->ebx &= ~0xFFu;
    } else {
        r->eax &= 0xFFFF0000u;
        r->ebx = (r->ebx & ~0xFFu) | 0x80;      /* not implemented */
    }
    r->eip = pop16(r);                          /* RETF */
    r->cs = pop16(r);
    return true;
}

/* DOS's own pointer to the disk handler (INT 2Fh AH=13h), which the memory
 * manager hooked for its DMA buffers: to the original while OS1993 runs. */
static uint16_t dos13_seg, dos13_off, dos13_hseg, dos13_hoff;

void v86_vcpi_dos_unhook(void)
{
    uint16_t mm = LOWMEM16(0x67 * 4 + 2);
    if (!mm_unhooked || !mm)
        return;
    struct v86_regs r = { .eax = 0x1300 };          /* DS:DX = 0:0 for a moment ... */
    if (v86_int(0x2F, &r) != 0)
        return;
    uint16_t ds = r.ds, dx = (uint16_t)r.edx, es = r.es, bx = (uint16_t)r.ebx;
    if (ds == mm && es) {                           /* ... the memory manager's: the original */
        dos13_seg = ds; dos13_off = dx; dos13_hseg = es; dos13_hoff = bx;
        r = (struct v86_regs){ .eax = 0x1300, .ds = es, .edx = bx, .es = es, .ebx = bx };
        klog("V86: DOS's disk handler: the memory manager's (%04X:%04X) -> %04X:%04X\n", ds, dx, es, bx);
    } else {                                        /* someone else's: put it back */
        r = (struct v86_regs){ .eax = 0x1300, .ds = ds, .edx = dx, .es = es, .ebx = bx };
    }
    v86_int(0x2F, &r);
}

void v86_vcpi_dos_rehook(void)
{
    if (!dos13_seg || vm_current() != system_vm)
        return;
    struct v86_regs r = { .eax = 0x1300, .ds = dos13_seg, .edx = dos13_off, .es = dos13_hseg, .ebx = dos13_hoff };
    v86_int(0x2F, &r);
    dos13_seg = 0;
}

/* At the end, with the System VM's own IVT live again. */
void v86_vcpi_rehook(void)
{
    if (!mm_unhooked)
        return;
    for (unsigned v = 0; v < 256; v++)
        if (mm_saved[v])
            LOWMEM32(v * 4) = mm_saved[v];
    LOWMEM8(0x47B) |= 0x20;
}

/* ---- public API -------------------------------------------------------- */

void v86_init(const struct bootinfo *bi)
{
    g.seg       = (uint16_t)(bi->loader_base >> 4);
    g.stub_off  = bi->v86_stub_off;
    g.stack_top = bi->v86_stack_top;
    g.buf_off   = bi->buf_off;
    g.buf_size  = bi->buf_size;
    g.brk_base  = (uint16_t)(g.buf_off + g.buf_size - BRK_AREA);
    for (int i = 0; i < BRK_MAX; i++) {         /* ARPLs: the System VM's copy, then every VM's */
        uint8_t *b = lin(g.seg, v86_brk_off(i));
        b[0] = 0x63;
        b[1] = 0xC0;
    }

    /* Let ring 3 (V86) code reach the first 1 MB + HMA; the kernel's own
     * pages stay supervisor-only, so a VM can't touch them. */
    volatile uint32_t *pte = (volatile uint32_t *)PT_WINDOW;
    volatile uint32_t *pde = (volatile uint32_t *)PD_WINDOW;
    for (unsigned i = 0; i < VM_V86_PAGES; i++)
        pte[i] |= PTE_U;
    for (unsigned i = VM_V86_PAGES; i < 1024; i++)
        pte[i] &= ~PTE_U;                       /* (a VCPI server's own pages, up here) */
    pde[0] |= PTE_U;
    tlb_flush();

    /* Virtualized ports; everything else goes straight to the hardware. */
    static const uint16_t trapped[] = { 0x20, 0x21, 0xA0, 0xA1, 0x60, 0x64, 0x40, 0x43 };
    for (unsigned i = 0; i < ARRAY_SIZE(trapped); i++)
        tss_trap_port(trapped[i], true);
    vdev_init();                                /* COM and LPT: one owner each */
    vmouse_init();
    xms_init();
    dpmi_init();

    if (bi->flags & BI_FLAG_VCPI)
        vcpi_unhook();                          /* the memory manager's HLT stubs */
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
    vm->vflags = EFLAGS_IOPL;
    vm->error[0] = '\0';
    vm->active = true;
    int rc = v86_enter(&f, &vm->kernel_esp);
    vm->active = false;
    return rc;
}

int v86_int(uint8_t vec, struct v86_regs *r)
{
    struct vm *vm = vm_current();
    int depth = dosmgr_kernel_enter(vm, vec, (uint8_t)(r->eax >> 8));
    uint16_t *sp = v86_ptr(g.seg, (uint16_t)(g.stack_top - 6));
    sp[0] = g.stub_off;                         /* IRET frame -> breakpoint */
    sp[1] = g.seg;
    sp[2] = 0x0202;                             /* IF=1 once the handler IRETs */
    int rc = run(r, LOWMEM16(vec * 4u + 2), LOWMEM16(vec * 4u),   /* IVT: CS, IP */
                 3, false);                     /* INT clears IF */
    dosmgr_kernel_leave(vm, depth);
    return rc;
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
uint16_t v86_buf_size(void) { return (uint16_t)(g.buf_size - BRK_AREA); }
