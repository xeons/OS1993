/*
 * dosmgr.c -- one DOS shared by every VM (a small DOSMGR).
 *
 * DOS lives in shared memory, so its disk buffers, file tables and FAT are
 * coherent across VMs. What makes a DOS "context" -- current PSP, DTA,
 * error state, DOS's internal stacks -- is in the swappable data area
 * (SDA, INT 21h AX=5D06h), which vm.c swaps per VM along with the current
 * directory structures (CDS) and the IVT/BIOS data.
 *
 * DOS isn't reentrant, so only one VM may be inside it at a time. Every
 * DOS entry (INT 20h/21h/25h/26h/27h) from V86 code is intercepted; if
 * another VM holds DOS, the caller blocks until it doesn't. The return
 * address is pointed at an ARPL breakpoint (loader stub + 2), so we see
 * exactly when DOS returns. A VM holds DOS while
 *
 *   - its innermost DOS call is still open *and was made by the current
 *     process* (an EXEC stays open while the child runs, but the child has
 *     a different PSP, so the parent doesn't hold DOS meanwhile), or
 *   - its InDOS or critical-error flag (in its SDA) is set,
 *
 * and it isn't at a DOS idle point. When DOS waits for the keyboard it calls
 * INT 16h, and it calls INT 28h to say "safe to interrupt". A VM inside
 * either one doesn't hold DOS, and it must take DOS back before it may
 * continue in it.
 */
#include "kernel.h"
#include "devices.h"
#include "io.h"
#include "sched.h"
#include "v86.h"
#include "vm.h"

#define CDS_SIZE 0x58               /* DOS 4+ current directory structure */

static struct {
    bool     on;
    uint16_t first_mcb;             /* the start of DOS's memory chain */
    uint32_t sda;                   /* linear address of the SDA */
    uint32_t waits;                 /* times a VM had to wait for DOS */
} dm;

uint32_t dosmgr_waits(void) { return dm.waits; }

static uint32_t far_lin(uint16_t seg, uint16_t off)
{
    return ((uint32_t)seg << 4) + off;
}

bool dosmgr_enabled(void) { return dm.on; }
uint16_t dosmgr_first_mcb(void) { return dm.first_mcb; }

uint16_t dosmgr_current_psp(struct vm *vm)
{
    return *(uint16_t *)vm_ptr(vm, dm.sda + 0x10);
}

static bool in_dos_flags(struct vm *vm)
{
    const uint8_t *sda = vm_ptr(vm, dm.sda);
    return sda[0] || sda[1];        /* critical-error flag, InDOS */
}

bool dosmgr_is_dos_int(uint8_t vec, uint8_t ah)
{
    (void)ah;
    return vec == 0x20 || vec == 0x21 || vec == 0x25 || vec == 0x26 || vec == 0x27;
}

static uint8_t call_kind(uint8_t vec, uint8_t ah)
{
    if (vec == 0x20 || vec == 0x27)
        return DC_NORETURN;
    if (vec == 0x21 && (ah == 0x00 || ah == 0x31 || ah == 0x4C))
        return DC_NORETURN;
    if (vec == 0x25 || vec == 0x26)
        return DC_RET_FLAGS;
    return DC_RETURN;
}

bool dosmgr_holds(struct vm *vm)
{
    if (!dm.on || vm->state != VM_RUNNING || vm->int28_depth > 0)
        return false;
    if (vm->dos_depth > 0 &&
        vm->dos[vm->dos_depth - 1].psp == dosmgr_current_psp(vm))
        return true;
    return in_dos_flags(vm);
}

bool dosmgr_busy_for(struct vm *me)
{
    for (int id = 0; id < VM_MAX; id++) {
        struct vm *vm = vm_get(id);
        if (vm && vm != me && dosmgr_holds(vm))
            return true;
    }
    return false;
}

/* Block the calling VM's thread until no other VM holds DOS. */
static void wait_for_dos(struct vm *vm)
{
    if (dosmgr_busy_for(vm))
        dm.waits++;
    while (dosmgr_busy_for(vm)) {
        uint32_t f = irq_save();
        current->state = T_BLOCKED;         /* every timer tick wakes us */
        schedule();
        irq_restore(f);
    }
}

static struct dos_call *push_call(struct vm *vm, uint8_t kind, uint16_t cs, uint16_t ip, uint16_t sp)
{
    if (vm->dos_depth == VM_DOS_NEST)
        return NULL;
    struct dos_call *c = &vm->dos[vm->dos_depth++];
    c->ret_cs = cs;
    c->ret_ip = ip;
    c->sp = sp;
    c->psp = dosmgr_current_psp(vm);
    c->kind = kind;
    if (kind == DC_IDLE)
        vm->int28_depth++;
    return c;
}

static void pop_to(struct vm *vm, int depth)
{
    while (vm->dos_depth > depth) {
        if (vm->dos[--vm->dos_depth].kind == DC_IDLE)
            vm->int28_depth--;
    }
}

/* ---- called by the V86 monitor ----------------------------------------- */

/*
 * V86 code executed INT n (IP already past it in next_ip). Returns
 * DOSMGR_PASS to reflect it normally, DOSMGR_HOOK if the caller should
 * reflect it with its return address at the DOS-return breakpoint, or
 * DOSMGR_DONE if it was handled here.
 */
int dosmgr_int(struct vm *vm, struct regs *r, uint8_t n, uint16_t next_ip)
{
    if (!dm.on)
        return DOSMGR_PASS;
    uint16_t ax = (uint16_t)r->eax;
    uint8_t ah = (uint8_t)(ax >> 8);

    if (n == 0x2F && ax == 0x1680) {        /* release time slice */
        r->eip = next_ip;
        r->eax &= ~0xFFu;                   /* AL=0: supported */
        uint32_t f = irq_save();
        current->state = T_BLOCKED;         /* until the next tick */
        schedule();
        irq_restore(f);
        return DOSMGR_DONE;
    }

    bool idle = (n == 0x28 || n == 0x16) && dosmgr_holds(vm);
    if (!idle && !dosmgr_is_dos_int(n, ah))
        return DOSMGR_PASS;

    if (!idle)
        wait_for_dos(vm);

    uint8_t kind = idle ? DC_IDLE : call_kind(n, ah);
    if (!idle && n == 0x21) {               /* trace: registers in */
        unsigned t = vm->trace_n++ % 48;
        vm->trace[t].ax = ax;
        vm->trace[t].bx = (uint16_t)r->ebx;
        vm->trace[t].cx = (uint16_t)r->ecx;
        vm->trace[t].dx = (uint16_t)r->edx;
        vm->trace[t].ret_ax = 0xDEAD;       /* until it returns */
        vm->trace[t].ret_fl = 0;
        vm->trace[t].tick = timer_ticks();
        vm->trace[t].cs = (uint16_t)r->cs;
        vm->trace[t].ip = (uint16_t)(next_ip - 2);
    }
    if (!push_call(vm, kind, (uint16_t)r->cs, next_ip, (uint16_t)r->user_esp)) {
        klog("DOSMGR: VM %d: DOS calls nested too deeply\n", vm->id);
        return DOSMGR_PASS;
    }
    return kind == DC_NORETURN ? DOSMGR_PASS : DOSMGR_HOOK;
}

/* V86 code reached the DOS-return breakpoint. Continue the caller. */
bool dosmgr_return(struct vm *vm, struct regs *r)
{
    uint16_t sp = (uint16_t)r->user_esp;
    int i;
    for (i = vm->dos_depth - 1; i >= 0; i--) {
        struct dos_call *c = &vm->dos[i];
        if (c->kind == DC_NORETURN || c->kind == DC_KERNEL)
            continue;
        if (sp == c->sp || (c->kind == DC_RET_FLAGS && sp == (uint16_t)(c->sp - 2)))
            break;
    }
    if (i < 0)
        return false;

    struct dos_call c = vm->dos[i];
    if (c.kind == DC_RETURN && vm->trace_n) {   /* trace: results of the newest open call */
        for (unsigned k = 0; k < 48 && k < vm->trace_n; k++) {
            unsigned t = (vm->trace_n - 1 - k) % 48;
            if (vm->trace[t].ret_ax == 0xDEAD) {
                vm->trace[t].ret_ax = (uint16_t)r->eax;
                vm->trace[t].ret_fl = (uint16_t)r->eflags;
                break;
            }
        }
    }
    if (c.kind == DC_IDLE)
        wait_for_dos(vm);                   /* back into DOS: take it again */
    pop_to(vm, i);                          /* also drops orphaned entries */
    r->cs = c.ret_cs;
    r->eip = c.ret_ip;
    return true;
}

/* Kernel-initiated v86_int() of a DOS vector: same rules. Returns the depth
 * to restore afterwards with dosmgr_kernel_leave(). */
int dosmgr_kernel_enter(struct vm *vm, uint8_t vec, uint8_t ah)
{
    int depth = vm->dos_depth;
    if (!dm.on || !dosmgr_is_dos_int(vec, ah))
        return -1;
    wait_for_dos(vm);
    push_call(vm, DC_KERNEL, 0, 0, 0);
    return depth;
}

void dosmgr_kernel_leave(struct vm *vm, int depth)
{
    if (depth >= 0)
        pop_to(vm, depth);
}

/* ---- the Windows enhanced-mode startup protocol ------------------------- */

/*
 * INT 2Fh AX=1605h is what Windows 3.x broadcast before entering 386
 * enhanced mode. DOS, drivers and TSRs answer by chaining Win386 startup
 * info structures (ES:BX), each pointing at a list of {far address, size}
 * areas whose contents must be kept per VM -- DOS's console buffers, for
 * example. CX != 0 on return means somebody refuses to run under a VMM.
 */
static void startup_broadcast(void)
{
    struct v86_regs r = { .eax = 0x1605, .edx = 0x0000,     /* bit 0 clear: enhanced */
                          .edi = 0x030A };                  /* "Windows 3.10" */
    if (v86_int(0x2F, &r) != 0)
        panic("INT 2Fh AX=1605h crashed: %s", v86_last_error());
    if (r.ecx & 0xFFFF)
        klog("DOSMGR: warning: a driver or TSR asked us not to start (CX=%04X)\n",
             r.ecx & 0xFFFF);

    unsigned structs = 0, items = 0, added = 0, bytes = 0;
    uint16_t seg = r.es, off = (uint16_t)r.ebx;
    while ((seg || off) && structs < 32) {
        const uint8_t *sis = vm_ptr(system_vm, far_lin(seg, off));
        uint16_t list_off = *(const uint16_t *)(sis + 0x0E);
        uint16_t list_seg = *(const uint16_t *)(sis + 0x10);
        uint32_t list = far_lin(list_seg, list_off);
        klog("DOSMGR: startup info at %04X:%04X, version %u.%02u, instance list %04X:%04X\n",
             seg, off, sis[0], sis[1], list_seg, list_off);
        while ((list_seg || list_off) && items < 256) {
            const uint8_t *it = vm_ptr(system_vm, list);
            uint16_t ioff = *(const uint16_t *)it, iseg = *(const uint16_t *)(it + 2);
            uint16_t size = *(const uint16_t *)(it + 4);
            if (!ioff && !iseg)
                break;
            items++;
            bool ok = vm_add_instance(far_lin(iseg, ioff), size);
            if (ok) { added++; bytes += size; }
            else klog("DOSMGR:   %04X:%04X (%u bytes) is not in shared memory, skipped\n",
                      iseg, ioff, size);
            list += 6;
        }
        off = *(const uint16_t *)(sis + 2);
        seg = *(const uint16_t *)(sis + 4);
        structs++;
    }
    klog("DOSMGR: INT 2Fh 1605h: %u startup structures, %u instance items (%u bytes)\n",
         structs, added, bytes);
}

void dosmgr_started(void)
{
    struct v86_regs r = { .eax = 0x1608 };          /* init complete */
    v86_int(0x2F, &r);
}

void dosmgr_shutdown(void)
{
    struct v86_regs r = { .eax = 0x1609 };          /* begin exit */
    v86_int(0x2F, &r);
    r = (struct v86_regs){ .eax = 0x1606, .edx = 0x0000 };   /* exit */
    v86_int(0x2F, &r);
}

/* ---- setup ------------------------------------------------------------- */

static bool call21(struct v86_regs *r)
{
    return v86_int(0x21, r) == 0 && !(r->flags & EFLAGS_CF);
}

/* Find DOS's per-context data and register it as VM instance data. Runs in
 * the System VM before the VM snapshot is taken. */
void dosmgr_init(void)
{
    struct v86_regs r = { .eax = 0x5D06 };      /* swappable data area */
    if (!call21(&r))
        panic("DOS has no swappable data area (INT 21h AX=5D06h); VMs can't share it.");
    dm.sda = far_lin(r.ds, (uint16_t)r.esi);
    uint32_t sda_len = r.ecx & 0xFFFF;
    uint16_t sda_off = (uint16_t)r.esi;

    r = (struct v86_regs){ .eax = 0x6200 };     /* sanity: SDA+10h = current PSP */
    call21(&r);
    uint16_t psp = (uint16_t)r.ebx;
    if (*(uint16_t *)vm_ptr(system_vm, dm.sda + 0x10) != psp)
        panic("The DOS SDA layout is not the expected one (PSP %04X).", psp);

    if (!vm_add_instance(dm.sda, sda_len))
        panic("The DOS SDA at %05X is not in shared memory.", dm.sda);

    /* In the MS-DOS 4+ layout (FreeDOS keeps it) the SDA is at DOS data
     * segment offset 0320h, and the 0125h bytes below it hold per-process
     * console state: the line-input buffer and type-ahead buffer at 01FBh,
     * printer echo, verify, the console column, switch char, allocation
     * strategy. MS-DOS reports these to Windows through INT 2Fh 1605h; a
     * FreeDOS built without WIN31SUPPORT doesn't, so instance them here --
     * otherwise two VMs reading the console share one line buffer. */
    bool console_ok = sda_off == 0x320 && vm_add_instance(dm.sda - 0x125, 0x125);

    r = (struct v86_regs){ .eax = 0x5200 };     /* List of Lists */
    call21(&r);
    uint32_t lol = far_lin(r.es, (uint16_t)r.ebx);
    dm.first_mcb = *(uint16_t *)vm_ptr(system_vm, lol - 2);
    uint16_t cds_off = *(uint16_t *)vm_ptr(system_vm, lol + 0x16);
    uint16_t cds_seg = *(uint16_t *)vm_ptr(system_vm, lol + 0x18);
    uint8_t lastdrive = *(uint8_t *)vm_ptr(system_vm, lol + 0x21);
    uint32_t cds = far_lin(cds_seg, cds_off);
    const char *c_path = vm_ptr(system_vm, cds + 2 * CDS_SIZE);
    bool cds_ok = lastdrive >= 3 && lastdrive <= 32 && c_path[0] == 'C' && c_path[1] == ':';
    if (cds_ok)
        cds_ok = vm_add_instance(cds, (uint32_t)lastdrive * CDS_SIZE);

    startup_broadcast();

    dm.on = true;
    klog("DOSMGR: SDA at %05X (%u bytes), console buffers %s, CDS %s, current PSP %04X\n",
         dm.sda, sda_len, console_ok ? "instanced" : "shared (layout unknown)",
         cds_ok ? "instanced" : "shared (layout unknown)", psp);
}

void dosmgr_dump_trace(struct vm *vm)
{
    unsigned n = vm->trace_n < 48 ? vm->trace_n : 48;
    klog("DOSMGR: last %u DOS calls of VM %d (of %u):\n", n, vm->id, vm->trace_n);
    for (unsigned k = n; k > 0; k--) {
        unsigned t = (vm->trace_n - k) % 48;
        klog("  t=%u %04X:%04X AX=%04X BX=%04X CX=%04X DX=%04X -> AX=%04X CF=%u\n",
             vm->trace[t].tick, vm->trace[t].cs, vm->trace[t].ip,
             vm->trace[t].ax, vm->trace[t].bx, vm->trace[t].cx,
             vm->trace[t].dx, vm->trace[t].ret_ax, vm->trace[t].ret_fl & 1);
    }
}
