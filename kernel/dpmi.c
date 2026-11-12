/*
 * dpmi.c -- a DPMI 0.9 host for DOS sessions, so DOS extenders (DOS/4GW,
 * DOS32A, PMODE/W) and other protected-mode DOS programs run in a VM, as
 * Windows' enhanced mode hosted them.
 *
 * A VM whose settings allow DPMI memory answers INT 2Fh AX=1687h with a
 * mode-switch entry point (a monitor breakpoint). A program that calls it
 * becomes the VM's client: it gets an LDT of its own, its code, data,
 * stack and PSP as 16-bit descriptors, and from then on runs in ring 3
 * protected mode in the VM's thread, as its V86 code did. Both are the
 * same trap frame on the VM's kernel stack (TSS.ESP0 leaves 16 bytes
 * spare, so a ring-3 frame can be rewritten into a V86 one in place):
 * every mode switch is the monitor rewriting that frame.
 *
 *   - Software interrupts in protected mode trap (their gates are ring 0).
 *     INT 31h is the DPMI API; a vector the client hooked (0205h) goes to
 *     its handler; anything else is reflected to real mode, where it runs
 *     as an INT instruction in a little stub in the host's data area -- so
 *     DOS calls still pass the DOS critical section, the redirector, the
 *     mouse and XMS -- and comes back with the general registers.
 *   - Hardware interrupts go to the client's protected-mode handler if it
 *     hooked the vector, through a host stub that restores the virtual
 *     interrupt flag after its IRET; otherwise to real mode.
 *   - Exceptions go to its exception handlers (0203h), on a host stack,
 *     returning through a stub; an exception it doesn't handle ends it.
 *   - Real-mode calls (0300h-0302h), real-mode callbacks (0303h), raw mode
 *     switches (0306h) and the DOS memory calls (0100h-0102h) are mode
 *     switches with a record of what to do when the other side is done.
 *   - Memory (0501h-0503h) comes from the VM's extended memory window
 *     (vmx.c), up to set.dpmi_kb; paging keeps the client out of the VMM.
 *
 * One client per VM. INT 21h AH=4Ch (or INT 20h, AH=00h) by the client's
 * process ends it: everything it had goes, and DOS ends the program.
 */
#include "kernel.h"
#include "devices.h"
#include "gdt.h"
#include "interrupts.h"
#include "io.h"
#include "mm.h"
#include "sched.h"
#include "user.h"
#include "v86.h"
#include "vm.h"

#define LDT_N        512
#define FIRST_CLIENT 16                 /* LDT slots below are the host's */
#define L_STUB32     1
#define L_STUB16     2
#define L_HSTACK     3
#define L_RMSS       4
#define L_TRAMPSS    5                  /* a 32-bit stack for the espfix trampoline */
#define TRAMP        0x400              /* the trampoline in the stub page ... */
#define TRAMP_TARGET 0x410              /* ... and the SS:ESP it loads */
#define SEL(i)       ((uint16_t)((i) << 3 | 7))     /* LDT, RPL 3 */
#define IDX(s)       ((s) >> 3)
#define STUBS        512                /* INT 30h instructions in the stub page */
#define S_EXC_RET    0x100              /* stub numbers below 100h: default handler of vector n */
#define S_IRQ_RET    0x101
#define S_CB_RET     0x102
#define S_RAW        0x103
#define S_SAVE       0x104
#define HSTACK_SIZE  8192
#define NEST         12
#define CALLBACKS    8
#define RM_SP_TOP    0x400              /* the host data area: stubs at 0, its real-mode stack */
#define HOST_PARAS   (RM_SP_TOP / 16)

enum { K_INT, K_STUB, K_CALL, K_IRQ, K_DOSALLOC, K_DOSFREE, K_DOSRESIZE,   /* PM -> RM */
       K_CB, K_PMIRQ };                                                     /* RM -> PM */

struct state {                          /* one side of a mode switch, to come back to */
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint32_t eip, cs, eflags, esp, ss, ds, es, fs, gs;
    uint8_t  kind;
    bool     vif;
    uint16_t rm_sp;                     /* the host's real-mode stack pointer before */
    uint32_t hstack;                    /* ... and its protected-mode one */
    uint32_t arg;                       /* K_CALL: the register structure; K_CB: the callback */
    uint16_t arg2;
};

struct callback {
    bool     used;
    uint16_t pm_sel, regs_sel;
    uint32_t pm_off, regs_off;
};

struct dpmi {
    bool     bits32;
    uint16_t psp, host_seg, env_seg;
    uint64_t ldt[LDT_N];
    uint32_t stub_lin, hstack_lin, hstack;   /* host stack: next free offset (grows down) */
    uint16_t rm_sp;
    uint16_t vec_sel[256];
    uint32_t vec_off[256];
    uint16_t exc_sel[32];
    uint32_t exc_off[32];
    struct state nest[NEST];
    int      depth;
    struct callback cb[CALLBACKS];
    struct { uint16_t seg, sel; } seg2sel[32];
};

#define SET16(reg, v) ((reg) = ((reg) & 0xFFFF0000u) | (uint16_t)(v))
#define AX(r) ((uint16_t)(r)->eax)
#define FLAGS_ARITH 0x0CD5u             /* CF PF AF ZF SF DF OF */

static void kill(struct vm *vm, struct regs *r, const char *why);

/* ---- descriptors --------------------------------------------------------- */

static uint64_t make_desc(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    if (limit > 0xFFFFF) { limit >>= 12; flags |= 0x8; }    /* page granular */
    uint64_t d = (limit & 0xFFFF) | (uint64_t)(base & 0xFFFFFF) << 16 |
                 (uint64_t)access << 40 | (uint64_t)((limit >> 16) & 0xF) << 48 |
                 (uint64_t)(flags & 0xF) << 52 | (uint64_t)(base >> 24) << 56;
    return d;
}

static uint32_t desc_base(uint64_t d)
{
    return (uint32_t)((d >> 16) & 0xFFFFFF) | (uint32_t)(d >> 56) << 24;
}

static uint32_t desc_limit(uint64_t d)
{
    uint32_t l = (uint32_t)(d & 0xFFFF) | (uint32_t)((d >> 48) & 0xF) << 16;
    return (d >> 55) & 1 ? l << 12 | 0xFFF : l;
}

static uint64_t *desc(struct dpmi *dp, uint16_t sel)
{
    if (!(sel & 4) || IDX(sel) >= LDT_N)
        return NULL;
    return &dp->ldt[IDX(sel)];
}

/* A selector of the client's (allocated, present or not), or NULL. */
static uint64_t *client_desc(struct dpmi *dp, uint16_t sel)
{
    uint64_t *d = desc(dp, sel);
    return d && IDX(sel) >= FIRST_CLIENT && (*d >> 44 & 1) ? d : NULL;   /* S bit: in use */
}

static int alloc_desc(struct dpmi *dp, int n)
{
    for (int i = FIRST_CLIENT; i + n <= LDT_N; i++) {
        int k = 0;
        while (k < n && !(dp->ldt[i + k] >> 44 & 1)) k++;
        if (k == n) {
            for (k = 0; k < n; k++)                 /* data, base 0, limit 0, present */
                dp->ldt[i + k] = make_desc(0, 0, 0xF2, dp->bits32 ? 0x4 : 0);
            return i;
        }
        i += k;
    }
    return -1;
}

static uint16_t new_sel(struct dpmi *dp, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    int i = alloc_desc(dp, 1);
    if (i < 0) return 0;
    dp->ldt[i] = make_desc(base, limit, access, flags);
    return SEL(i);
}

static uint32_t sel_base(struct dpmi *dp, uint16_t sel)
{
    uint64_t *d = desc(dp, sel);
    return d ? desc_base(*d) : 0;
}

static bool sel_big(struct dpmi *dp, uint16_t sel)      /* D/B bit */
{
    uint64_t *d = desc(dp, sel);
    return d && (*d >> 54 & 1);
}

void dpmi_install_ldt(struct vm *vm)
{
    struct dpmi *dp = vm ? vm->dpmi : NULL;
    if (dp) gdt_set_ldt((uint32_t)(uintptr_t)dp->ldt, sizeof dp->ldt - 1);
    else    gdt_set_ldt(0, 0);
    uint16_t s = dp ? LDT_SEL : 0;
    __asm__ volatile("lldt %0" : : "r"(s));
}

/* ---- the client's memory ------------------------------------------------- */

/* A kernel pointer to len bytes at a linear address the client may use. */
static void *ulin(uint32_t lin, uint32_t len, bool write)
{
    if (!len || lin + len < lin || lin + len > 0xFFC00000u)
        return NULL;
    for (uint32_t p = lin & PAGE_MASK; p < lin + len; p += PAGE_SIZE) {
        uint32_t pde = ((volatile uint32_t *)PD_WINDOW)[p >> 22];
        if ((pde & (PTE_P | PTE_U)) != (PTE_P | PTE_U))
            return NULL;
        uint32_t pte = ((volatile uint32_t *)PT_WINDOW)[p >> 12];
        if ((pte & (PTE_P | PTE_U)) != (PTE_P | PTE_U) || (write && !(pte & PTE_W)))
            return NULL;
    }
    return (void *)(uintptr_t)lin;
}

static void *uptr(struct dpmi *dp, uint16_t sel, uint32_t off, uint32_t len, bool write)
{
    uint64_t *d = desc(dp, sel);
    if (!d || !(*d >> 47 & 1) || off > desc_limit(*d) || len - 1 > desc_limit(*d) - off)
        return NULL;
    return ulin(desc_base(*d) + off, len, write);
}

/* The client's ES:(E)DI, DS:(E)SI. */
static uint32_t ofs(struct dpmi *dp, uint32_t v) { return dp->bits32 ? v : (uint16_t)v; }

static bool pm_push(struct dpmi *dp, struct regs *r, uint32_t v, bool wide)
{
    bool big = sel_big(dp, (uint16_t)r->user_ss);
    uint32_t sp = big ? r->user_esp : (uint16_t)r->user_esp;
    sp -= wide ? 4 : 2;
    if (!big) sp &= 0xFFFF;
    void *p = uptr(dp, (uint16_t)r->user_ss, sp, wide ? 4 : 2, true);
    if (!p) return false;
    if (wide) *(uint32_t *)p = v; else *(uint16_t *)p = (uint16_t)v;
    r->user_esp = big ? sp : (r->user_esp & 0xFFFF0000u) | sp;
    return true;
}

static bool pm_pop(struct dpmi *dp, struct regs *r, uint32_t *v, bool wide)
{
    bool big = sel_big(dp, (uint16_t)r->user_ss);
    uint32_t sp = big ? r->user_esp : (uint16_t)r->user_esp;
    void *p = uptr(dp, (uint16_t)r->user_ss, sp, wide ? 4 : 2, false);
    if (!p) return false;
    *v = wide ? *(uint32_t *)p : *(uint16_t *)p;
    sp += wide ? 4 : 2;
    if (!big) sp &= 0xFFFF;
    r->user_esp = big ? sp : (r->user_esp & 0xFFFF0000u) | sp;
    return true;
}

/* ---- frames: protected mode <-> real mode ------------------------------- */

static void save(struct regs *r, struct state *s)
{
    s->eax = r->eax; s->ebx = r->ebx; s->ecx = r->ecx; s->edx = r->edx;
    s->esi = r->esi; s->edi = r->edi; s->ebp = r->ebp;
    s->eip = r->eip; s->cs = r->cs & 0xFFFF; s->esp = r->user_esp; s->ss = r->user_ss & 0xFFFF;
    if (r->eflags & EFLAGS_VM) {
        s->ds = r->v86_ds & 0xFFFF; s->es = r->v86_es & 0xFFFF;
        s->fs = r->v86_fs & 0xFFFF; s->gs = r->v86_gs & 0xFFFF;
    } else {
        s->ds = r->ds & 0xFFFF; s->es = r->es & 0xFFFF; s->fs = r->fs & 0xFFFF; s->gs = r->gs & 0xFFFF;
    }
}

/* Make the frame a ring-3 one: flags keep the arithmetic bits and TF. */
static void to_pm(struct regs *r, uint16_t cs, uint32_t eip, uint16_t ss, uint32_t esp,
                  uint16_t ds, uint16_t es, uint16_t fs, uint16_t gs, uint32_t flags)
{
    r->eflags = (flags & (FLAGS_ARITH | EFLAGS_TF)) | EFLAGS_IF | 0x2;
    r->cs = cs; r->eip = eip; r->user_ss = ss; r->user_esp = esp;
    r->ds = ds; r->es = es; r->fs = fs; r->gs = gs;
}

static void to_rm(struct regs *r, uint16_t cs, uint16_t ip, uint16_t ss, uint16_t sp,
                  uint16_t ds, uint16_t es, uint32_t flags)
{
    r->eflags = (flags & (FLAGS_ARITH | EFLAGS_TF)) | EFLAGS_IF | EFLAGS_VM | 0x2;
    r->cs = cs; r->eip = ip; r->user_ss = ss; r->user_esp = sp;
    r->v86_ds = ds; r->v86_es = es; r->v86_fs = 0; r->v86_gs = 0;
    r->ds = r->es = r->fs = r->gs = 0;
}

static void restore_pm(struct regs *r, const struct state *s)
{
    r->eax = s->eax; r->ebx = s->ebx; r->ecx = s->ecx; r->edx = s->edx;
    r->esi = s->esi; r->edi = s->edi; r->ebp = s->ebp;
    to_pm(r, (uint16_t)s->cs, s->eip, (uint16_t)s->ss, s->esp,
          (uint16_t)s->ds, (uint16_t)s->es, (uint16_t)s->fs, (uint16_t)s->gs, s->eflags);
}

static void restore_rm(struct regs *r, const struct state *s)
{
    r->eax = s->eax; r->ebx = s->ebx; r->ecx = s->ecx; r->edx = s->edx;
    r->esi = s->esi; r->edi = s->edi; r->ebp = s->ebp;
    to_rm(r, (uint16_t)s->cs, (uint16_t)s->eip, (uint16_t)s->ss, (uint16_t)s->esp,
          (uint16_t)s->ds, (uint16_t)s->es, s->eflags);
    r->v86_fs = s->fs;
    r->v86_gs = s->gs;
}

/* A selector as a real-mode segment, if it is one (base a paragraph below
 * 1 MB); else 0. */
static uint16_t as_seg(struct dpmi *dp, uint16_t sel)
{
    if (!sel) return 0;
    uint32_t b = sel_base(dp, sel);
    return b < 0x110000 && !(b & 15) ? (uint16_t)(b >> 4) : 0;
}

static struct state *push_state(struct vm *vm, struct regs *r, int kind)
{
    struct dpmi *dp = vm->dpmi;
    if (dp->depth == NEST) {
        kill(vm, r, "mode switches nested too deeply");
        return NULL;
    }
    struct state *s = &dp->nest[dp->depth++];
    save(r, s);
    s->kind = (uint8_t)kind;
    s->vif = vm->vif;
    s->rm_sp = dp->rm_sp;
    s->hstack = dp->hstack;
    return s;
}

/* Real-mode code in the host's data area: INT n, a far call, an IRET-style
 * call; each then jumps to the BRK_DPMI_RMRET breakpoint. */
#define RM_INT_STUB   0x00
#define RM_CALL_STUB  0x08
#define RM_ICALL_STUB 0x14

static void rm_stubs(struct vm *vm)
{
    struct dpmi *dp = vm->dpmi;
    uint8_t *c = (uint8_t *)(uintptr_t)((uint32_t)dp->host_seg << 4);
    uint16_t brk = v86_brk_off(BRK_DPMI_RMRET), seg = v86_buf_seg();
    uint8_t jmp[5] = { 0xEA, (uint8_t)brk, (uint8_t)(brk >> 8), (uint8_t)seg, (uint8_t)(seg >> 8) };
    c[RM_INT_STUB] = 0xCD; c[RM_INT_STUB + 1] = 0;
    memcpy(c + RM_INT_STUB + 2, jmp, 5);
    c[RM_CALL_STUB] = 0x9A;                                     /* call far ptr16:16 */
    memcpy(c + RM_CALL_STUB + 5, jmp, 5);
    c[RM_ICALL_STUB] = 0x9C;                                    /* pushf */
    c[RM_ICALL_STUB + 1] = 0x9A;
    memcpy(c + RM_ICALL_STUB + 6, jmp, 5);
}

/* The host's real-mode stack: below what's in use. */
static uint16_t rm_stack(struct dpmi *dp) { return dp->rm_sp; }

/* Switch to real mode to run INT n (kind K_INT/K_STUB/K_IRQ/...), with the
 * client's general registers. */
static void reflect_rm(struct vm *vm, struct regs *r, uint8_t n, int kind)
{
    struct dpmi *dp = vm->dpmi;
    struct state *s = push_state(vm, r, kind);
    if (!s) return;
    uint8_t *c = (uint8_t *)(uintptr_t)((uint32_t)dp->host_seg << 4);
    c[RM_INT_STUB + 1] = n;
    uint16_t ds = as_seg(dp, (uint16_t)r->ds), es = as_seg(dp, (uint16_t)r->es);
    uint32_t flags = r->eflags;
    to_rm(r, dp->host_seg, RM_INT_STUB, dp->host_seg, rm_stack(dp), ds, es, flags);
    dp->rm_sp = (uint16_t)(rm_stack(dp) - 0x20);
    if (kind == K_IRQ)
        vm->vif = false;
}

/* ---- the client's real-mode register structure (0300h, callbacks) -------- */

struct rmregs {                         /* the DPMI real-mode call structure */
    uint32_t edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    uint16_t flags, es, ds, fs, gs, ip, cs, sp, ss;
} __attribute__((packed));

static void rmregs_to_frame(struct regs *r, const struct rmregs *m)
{
    r->edi = m->edi; r->esi = m->esi; r->ebp = m->ebp; r->ebx = m->ebx;
    r->edx = m->edx; r->ecx = m->ecx; r->eax = m->eax;
}

static void frame_to_rmregs(struct rmregs *m, const struct regs *r)
{
    m->edi = r->edi; m->esi = r->esi; m->ebp = r->ebp; m->ebx = r->ebx;
    m->edx = r->edx; m->ecx = r->ecx; m->eax = r->eax;
    m->flags = (uint16_t)(r->eflags & 0xFFFF);
    m->es = (uint16_t)r->v86_es; m->ds = (uint16_t)r->v86_ds;
    m->fs = (uint16_t)r->v86_fs; m->gs = (uint16_t)r->v86_gs;
}

/* 0300h-0302h: BL = vector (0300h), ES:(E)DI = structure, CX = words of
 * the client's stack to copy. */
static void rm_call(struct vm *vm, struct regs *r, int how)
{
    struct dpmi *dp = vm->dpmi;
    uint32_t mo = ofs(dp, r->edi);
    struct rmregs *m = uptr(dp, (uint16_t)r->es, mo, sizeof *m, true);
    uint16_t words = (uint16_t)r->ecx;
    if (!m || words > 128) {
        SET16(r->eax, 0x8021);
        r->eflags |= EFLAGS_CF;
        return;
    }
    struct rmregs mm = *m;
    uint16_t ss = mm.ss, sp = mm.sp;
    if (!ss && !sp) { ss = dp->host_seg; sp = rm_stack(dp); }
    /* the words from the client's stack, onto the real-mode one */
    bool big = sel_big(dp, (uint16_t)r->user_ss);
    uint32_t csp = big ? r->user_esp : (uint16_t)r->user_esp;
    const uint16_t *from = words ? uptr(dp, (uint16_t)r->user_ss, csp, words * 2u, false) : NULL;
    if (words && !from) {
        SET16(r->eax, 0x8022);
        r->eflags |= EFLAGS_CF;
        return;
    }
    struct state *s = push_state(vm, r, K_CALL);
    if (!s) return;
    s->arg = mo;
    s->arg2 = (uint16_t)r->es;
    sp = (uint16_t)(sp - words * 2);
    if (words)
        memcpy((void *)(uintptr_t)(((uint32_t)ss << 4) + sp), from, words * 2u);
    if (ss == dp->host_seg)
        dp->rm_sp = (uint16_t)(sp - 0x20);
    uint8_t *c = (uint8_t *)(uintptr_t)((uint32_t)dp->host_seg << 4);
    uint16_t ip;
    if (how == 0) {                                 /* 0300h: INT BL */
        c[RM_INT_STUB + 1] = (uint8_t)r->ebx;
        ip = RM_INT_STUB;
    } else {                                        /* 0301h / 0302h: CS:IP from the structure */
        uint16_t at = how == 1 ? RM_CALL_STUB + 1 : RM_ICALL_STUB + 2;
        c[at] = (uint8_t)mm.ip; c[at + 1] = (uint8_t)(mm.ip >> 8);
        c[at + 2] = (uint8_t)mm.cs; c[at + 3] = (uint8_t)(mm.cs >> 8);
        ip = how == 1 ? RM_CALL_STUB : RM_ICALL_STUB;
    }
    rmregs_to_frame(r, &mm);
    to_rm(r, dp->host_seg, ip, ss, sp, mm.ds, mm.es, mm.flags);
    r->v86_fs = mm.fs;
    r->v86_gs = mm.gs;
    vm->vif = (mm.flags & EFLAGS_IF) != 0;
    if (how == 0) vm->vif = false;                  /* as an INT does */
}

/* ---- calling into the client's protected-mode code ---------------------- */

static uint16_t stub_cs(struct dpmi *dp) { return SEL(dp->bits32 ? L_STUB32 : L_STUB16); }
static uint32_t stub_off(int n) { return (uint32_t)n * 2; }

/* A frame on the host's protected-mode stack (it nests). */
static uint32_t host_push(struct dpmi *dp, uint32_t v, bool wide)
{
    dp->hstack -= wide ? 4 : 2;
    void *p = (void *)(uintptr_t)(dp->hstack_lin + dp->hstack);
    if (wide) *(uint32_t *)p = v; else *(uint16_t *)p = (uint16_t)v;
    return dp->hstack;
}

/* Hardware interrupt vec to the client's protected-mode handler: its IRET
 * comes back to S_IRQ_RET, which resumes what was interrupted. */
static void pm_irq(struct vm *vm, struct regs *r, uint8_t vec)
{
    struct dpmi *dp = vm->dpmi;
    bool from_rm = r->eflags & EFLAGS_VM;
    struct state *s = push_state(vm, r, K_PMIRQ);
    if (!s) return;
    uint32_t flags = (r->eflags & FLAGS_ARITH) | 0x202;
    if (from_rm && (uint16_t)r->user_ss == dp->host_seg)
        dp->rm_sp = (uint16_t)(r->user_esp - 0x20);
    host_push(dp, flags, dp->bits32);                   /* IRET frame -> the stub */
    host_push(dp, stub_cs(dp), dp->bits32);
    host_push(dp, stub_off(S_IRQ_RET), dp->bits32);
    uint16_t ds = from_rm ? 0 : (uint16_t)r->ds, es = from_rm ? 0 : (uint16_t)r->es;
    to_pm(r, dp->vec_sel[vec], dp->vec_off[vec], SEL(L_HSTACK), dp->hstack, ds, es,
          from_rm ? 0 : (uint16_t)r->fs, from_rm ? 0 : (uint16_t)r->gs, 0x2);
    vm->vif = false;
}

/* Exception n (with err) to the client's handler, on the host stack. */
static void pm_exception(struct vm *vm, struct regs *r, int n, uint32_t err)
{
    struct dpmi *dp = vm->dpmi;
    bool w = dp->bits32;
    uint32_t save_hs = dp->hstack;
    host_push(dp, r->user_ss & 0xFFFF, w);
    host_push(dp, r->user_esp, w);
    host_push(dp, r->eflags & ~EFLAGS_VM, w);
    host_push(dp, r->cs & 0xFFFF, w);
    host_push(dp, r->eip, w);
    host_push(dp, err, w);
    host_push(dp, stub_cs(dp), w);                      /* its RETF -> S_EXC_RET */
    host_push(dp, stub_off(S_EXC_RET), w);
    struct state *s = push_state(vm, r, K_PMIRQ);       /* only to keep hstack nested */
    if (!s) return;
    s->hstack = save_hs;
    s->kind = 0xFF;                                     /* (an exception record) */
    to_pm(r, dp->exc_sel[n], dp->exc_off[n], SEL(L_HSTACK), dp->hstack,
          (uint16_t)r->ds, (uint16_t)r->es, (uint16_t)r->fs, (uint16_t)r->gs, r->eflags);
}

/* ---- the end ------------------------------------------------------------- */

static void end_client(struct vm *vm)
{
    struct dpmi *dp = vm->dpmi;
    if (!dp) return;
    if (dp->env_seg)                                    /* DOS frees the environment by segment */
        *(uint16_t *)(uintptr_t)(((uint32_t)dp->psp << 4) + 0x2C) = dp->env_seg;
    vmx_free(vm, dp->stub_lin);
    vmx_free(vm, dp->hstack_lin);
    for (int i = 0; i < VMX_BLOCKS; i++)               /* its memory blocks */
        if (vm->vmx[i].size && vm->vmx[i].owner == VMX_DPMI)
            vmx_free(vm, vm->vmx[i].base);
    vm->dpmi = NULL;
    dpmi_install_ldt(NULL);
    kfree(dp);
    klog("DPMI: VM %d's client ended\n", vm->id);
}

/* The program ends from protected mode: clean up, then INT 21h AH=4Ch
 * (code al) in real mode, which ends the program. */
static void terminate(struct vm *vm, struct regs *r, uint8_t code)
{
    struct dpmi *dp = vm->dpmi;
    uint16_t hs = dp->host_seg;
    uint8_t *c = (uint8_t *)(uintptr_t)((uint32_t)hs << 4);
    c[RM_INT_STUB + 1] = 0x21;
    end_client(vm);
    to_rm(r, hs, RM_INT_STUB, hs, RM_SP_TOP, 0, 0, 0);
    r->eax = 0x4C00u | code;
}

static void kill(struct vm *vm, struct regs *r, const char *why)
{
    struct dpmi *dp = vm->dpmi;
    klog("DPMI: VM %d: client stopped: %s at %04X:%08X (ESP %08X, error %X)\n",
         vm->id, why, r->cs & 0xFFFF, r->eip, r->user_esp, r->err_code);
    if (!(r->eflags & EFLAGS_VM)) {
        const uint8_t *c = uptr(dp, (uint16_t)r->cs, r->eip, 4, false);
        klog("DPMI:   CR2=%08X CS base %08X SS=%04X base %08X DS=%04X base %08X ES=%04X base %08X code %02X %02X %02X %02X\n",
             read_cr2(), sel_base(dp, (uint16_t)r->cs), r->user_ss & 0xFFFF, sel_base(dp, (uint16_t)r->user_ss),
             r->ds & 0xFFFF, sel_base(dp, (uint16_t)r->ds), r->es & 0xFFFF, sel_base(dp, (uint16_t)r->es),
             c ? c[0] : 0, c ? c[1] : 0, c ? c[2] : 0, c ? c[3] : 0);
        klog("DPMI:   EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X\n",
             r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp);
    }
    terminate(vm, r, 0xFF);
}

/* ---- INT 31h ------------------------------------------------------------- */

static void ok(struct regs *r)              { r->eflags &= ~EFLAGS_CF; }
static void err(struct regs *r, uint16_t c) { SET16(r->eax, c); r->eflags |= EFLAGS_CF; }

static bool valid_access(uint8_t access)    /* the client's own data/code, ring 3 */
{
    return (access & 0x10) && ((access >> 5) & 3) == 3;
}

static void int31(struct vm *vm, struct regs *r)
{
    struct dpmi *dp = vm->dpmi;
    uint16_t ax = AX(r), bx = (uint16_t)r->ebx;
    uint64_t *d;
    ok(r);
    switch (ax) {
    case 0x0000: {                                      /* allocate descriptors */
        int n = (uint16_t)r->ecx, i = n ? alloc_desc(dp, n) : -1;
        if (i < 0) { err(r, 0x8011); break; }
        SET16(r->eax, SEL(i));
        break;
    }
    case 0x0001:                                        /* free */
        if (!(d = client_desc(dp, bx))) { err(r, 0x8022); break; }
        *d = 0;
        break;
    case 0x0002: {                                      /* real-mode segment -> descriptor */
        for (int i = 0; i < 32; i++)
            if (dp->seg2sel[i].sel && dp->seg2sel[i].seg == bx) { SET16(r->eax, dp->seg2sel[i].sel); return; }
        uint16_t s = new_sel(dp, (uint32_t)bx << 4, 0xFFFF, 0xF2, 0);
        if (!s) { err(r, 0x8011); break; }
        for (int i = 0; i < 32; i++)
            if (!dp->seg2sel[i].sel) { dp->seg2sel[i].seg = bx; dp->seg2sel[i].sel = s; break; }
        SET16(r->eax, s);
        break;
    }
    case 0x0003: SET16(r->eax, 8); break;               /* selector increment */
    case 0x0006:                                        /* get base */
        if (!(d = desc(dp, bx)) || !(*d >> 44 & 1)) { err(r, 0x8022); break; }
        SET16(r->ecx, desc_base(*d) >> 16);
        SET16(r->edx, desc_base(*d));
        break;
    case 0x0007: {                                      /* set base */
        if (!(d = client_desc(dp, bx))) { err(r, 0x8022); break; }
        uint32_t base = (uint32_t)(uint16_t)r->ecx << 16 | (uint16_t)r->edx;
        *d = (*d & ~0xFF0000FFFFFF0000ull) | (uint64_t)(base & 0xFFFFFF) << 16 | (uint64_t)(base >> 24) << 56;
        break;
    }
    case 0x0008: {                                      /* set limit */
        if (!(d = client_desc(dp, bx))) { err(r, 0x8022); break; }
        uint32_t lim = (uint32_t)(uint16_t)r->ecx << 16 | (uint16_t)r->edx;
        if (lim > 0xFFFFF && (lim & 0xFFF) != 0xFFF) { err(r, 0x8021); break; }
        uint8_t flags = (uint8_t)((*d >> 52) & 0x7);
        *d = make_desc(desc_base(*d), lim, (uint8_t)(*d >> 40), flags);
        break;
    }
    case 0x0009: {                                      /* set access rights */
        if (!(d = client_desc(dp, bx))) { err(r, 0x8022); break; }
        uint8_t acc = (uint8_t)r->ecx, fl = (uint8_t)(r->ecx >> 8);
        if (!valid_access(acc)) { err(r, 0x8021); break; }
        *d = (*d & ~0x00F0FF0000000000ull) | (uint64_t)acc << 40 | (uint64_t)(fl >> 4 & 0xD) << 52;
        break;
    }
    case 0x000A: {                                      /* alias: a data descriptor for it */
        if (!(d = client_desc(dp, bx))) { err(r, 0x8022); break; }
        int i = alloc_desc(dp, 1);
        if (i < 0) { err(r, 0x8011); break; }
        dp->ldt[i] = (*d & ~0x00000F0000000000ull) | 0x0000020000000000ull;   /* type: data, writable */
        SET16(r->eax, SEL(i));
        break;
    }
    case 0x000B: {                                      /* get descriptor */
        uint64_t *out = uptr(dp, (uint16_t)r->es, ofs(dp, r->edi), 8, true);
        if (!(d = desc(dp, bx)) || !out) { err(r, 0x8022); break; }
        *out = *d;
        break;
    }
    case 0x000C: {                                      /* set descriptor */
        const uint64_t *in = uptr(dp, (uint16_t)r->es, ofs(dp, r->edi), 8, false);
        if (!(d = client_desc(dp, bx)) || !in) { err(r, 0x8022); break; }
        if (!valid_access((uint8_t)(*in >> 40))) { err(r, 0x8021); break; }
        *d = *in;
        break;
    }
    case 0x000D:                                        /* allocate a specific descriptor */
        if (!(bx & 4) || IDX(bx) >= FIRST_CLIENT || IDX(bx) < 8) { err(r, 0x8022); break; }
        dp->ldt[IDX(bx)] = make_desc(0, 0, 0xF2, 0);
        break;

    case 0x0100:                                        /* DOS memory: allocate */
    case 0x0101:                                        /* ... free */
    case 0x0102: {                                      /* ... resize */
        int kind = ax == 0x0100 ? K_DOSALLOC : ax == 0x0101 ? K_DOSFREE : K_DOSRESIZE;
        uint16_t sel = (uint16_t)r->edx;
        if (ax != 0x0100 && !client_desc(dp, sel)) { err(r, 0x8022); break; }
        uint16_t seg = ax == 0x0100 ? 0 : (uint16_t)(sel_base(dp, sel) >> 4);
        reflect_rm(vm, r, 0x21, kind);
        if (!vm->dpmi) break;
        r->eax = ax == 0x0100 ? 0x4800 : ax == 0x0101 ? 0x4900 : 0x4A00;
        r->v86_es = seg;
        break;
    }

    case 0x0200:                                        /* real-mode vector */
        SET16(r->ecx, *(uint16_t *)(uintptr_t)((uint8_t)bx * 4u + 2));
        SET16(r->edx, *(uint16_t *)(uintptr_t)((uint8_t)bx * 4u));
        break;
    case 0x0201:
        *(uint16_t *)(uintptr_t)((uint8_t)bx * 4u + 2) = (uint16_t)r->ecx;
        *(uint16_t *)(uintptr_t)((uint8_t)bx * 4u) = (uint16_t)r->edx;
        break;
    case 0x0202:                                        /* exception handler */
        if ((uint8_t)bx >= 32) { err(r, 0x8021); break; }
        SET16(r->ecx, dp->exc_sel[(uint8_t)bx] ? dp->exc_sel[(uint8_t)bx] : stub_cs(dp));
        r->edx = dp->exc_sel[(uint8_t)bx] ? dp->exc_off[(uint8_t)bx] : stub_off(0x120 + (uint8_t)bx);
        break;
    case 0x0203:
        if ((uint8_t)bx >= 32) { err(r, 0x8021); break; }
        if ((uint16_t)r->ecx == stub_cs(dp)) dp->exc_sel[(uint8_t)bx] = 0;   /* the default again */
        else { dp->exc_sel[(uint8_t)bx] = (uint16_t)r->ecx; dp->exc_off[(uint8_t)bx] = ofs(dp, r->edx); }
        break;
    case 0x0204:                                        /* protected-mode vector */
        SET16(r->ecx, dp->vec_sel[(uint8_t)bx] ? dp->vec_sel[(uint8_t)bx] : stub_cs(dp));
        r->edx = dp->vec_sel[(uint8_t)bx] ? dp->vec_off[(uint8_t)bx] : stub_off((uint8_t)bx);
        if (!dp->bits32) r->edx &= 0xFFFF;
        break;
    case 0x0205:
        if ((uint16_t)r->ecx == stub_cs(dp)) dp->vec_sel[(uint8_t)bx] = 0;
        else { dp->vec_sel[(uint8_t)bx] = (uint16_t)r->ecx; dp->vec_off[(uint8_t)bx] = ofs(dp, r->edx); }
        break;

    case 0x0300: rm_call(vm, r, 0); break;
    case 0x0301: rm_call(vm, r, 1); break;
    case 0x0302: rm_call(vm, r, 2); break;
    case 0x0303: {                                      /* real-mode callback */
        int n = -1;
        for (int i = 0; i < CALLBACKS && n < 0; i++)
            if (!dp->cb[i].used) n = i;
        if (n < 0) { err(r, 0x8015); break; }
        dp->cb[n] = (struct callback){ true, (uint16_t)r->ds, (uint16_t)r->es, ofs(dp, r->esi), ofs(dp, r->edi) };
        SET16(r->ecx, v86_buf_seg());
        SET16(r->edx, v86_brk_off(BRK_DPMI_CB + n));
        break;
    }
    case 0x0304: {
        int n = ((uint16_t)r->edx - v86_brk_off(BRK_DPMI_CB)) / 2;
        if ((uint16_t)r->ecx != v86_buf_seg() || n < 0 || n >= CALLBACKS || !dp->cb[n].used) { err(r, 0x8024); break; }
        dp->cb[n].used = false;
        break;
    }
    case 0x0305:                                        /* state save/restore: nothing to save */
        SET16(r->eax, 0);
        SET16(r->ebx, v86_buf_seg());
        SET16(r->ecx, v86_brk_off(BRK_DPMI_SAVE));
        SET16(r->esi, stub_cs(dp));
        r->edi = stub_off(S_SAVE);
        break;
    case 0x0306:                                        /* raw mode switch */
        SET16(r->ebx, v86_buf_seg());
        SET16(r->ecx, v86_brk_off(BRK_DPMI_RAW));
        SET16(r->esi, stub_cs(dp));
        r->edi = stub_off(S_RAW);
        break;

    case 0x0400:                                        /* version */
        SET16(r->eax, 0x005A);
        SET16(r->ebx, 0x0003);                          /* 32-bit, interrupts reflected through V86 */
        r->ecx = (r->ecx & ~0xFFu) | 4;                 /* a 486 */
        SET16(r->edx, (uint16_t)(vm->pic.vbase[0] << 8 | vm->pic.vbase[1]));
        break;
    case 0x0500: {                                      /* free memory information */
        uint32_t *o = uptr(dp, (uint16_t)r->es, ofs(dp, r->edi), 48, true);
        if (!o) { err(r, 0x8021); break; }
        uint32_t avail = vmx_avail(vm, VMX_DPMI);
        memset(o, 0xFF, 48);
        o[0] = avail;                                   /* largest free block, bytes */
        o[1] = o[2] = avail / PAGE_SIZE;                /* unlocked/locked pages to be had */
        o[3] = VMX_SIZE / PAGE_SIZE;                    /* linear space, pages */
        o[5] = pmm_free_pages();
        o[6] = pmm_total_pages();
        o[7] = VMX_SIZE / PAGE_SIZE;
        break;
    }
    case 0x0501: {                                      /* allocate memory */
        uint32_t size = (uint32_t)bx << 16 | (uint16_t)r->ecx;
        uint32_t lin = size ? vmx_alloc(vm, size, VMX_DPMI) : 0;
        if (!lin) { err(r, 0x8013); break; }
        SET16(r->ebx, lin >> 16); SET16(r->ecx, lin);
        SET16(r->esi, lin >> 16); SET16(r->edi, lin);
        break;
    }
    case 0x0502: {                                      /* free */
        uint32_t h = (uint32_t)(uint16_t)r->esi << 16 | (uint16_t)r->edi;
        if (!vmx_size(vm, h) || !vmx_free(vm, h)) err(r, 0x8023);
        break;
    }
    case 0x0503: {                                      /* resize */
        uint32_t h = (uint32_t)(uint16_t)r->esi << 16 | (uint16_t)r->edi;
        uint32_t size = (uint32_t)bx << 16 | (uint16_t)r->ecx;
        uint32_t lin = vmx_size(vm, h) && size ? vmx_resize(vm, h, size) : 0;
        if (!lin) { err(r, vmx_size(vm, h) ? 0x8013 : 0x8023); break; }
        SET16(r->ebx, lin >> 16); SET16(r->ecx, lin);
        SET16(r->esi, lin >> 16); SET16(r->edi, lin);
        break;
    }
    case 0x0600: case 0x0601: case 0x0602: case 0x0603:    /* locking: all of it is locked */
    case 0x0702: case 0x0703:
        break;
    case 0x0604:                                        /* page size */
        SET16(r->ebx, 0); SET16(r->ecx, PAGE_SIZE);
        break;
    case 0x0800: {                                      /* physical address mapping */
        uint32_t phys = (uint32_t)bx << 16 | (uint16_t)r->ecx;
        uint32_t size = (uint32_t)(uint16_t)r->esi << 16 | (uint16_t)r->edi;
        if (phys >= 0xA0000 && phys + size <= 0x100000) break;  /* adapter space: where it is */
        err(r, 0x8021);
        break;
    }
    case 0x0900: case 0x0901: case 0x0902: {            /* virtual interrupt state */
        bool was = vm->vif;
        if (ax == 0x0900) vm->vif = false;
        if (ax == 0x0901) vm->vif = true;
        r->eax = (r->eax & ~0xFFu) | (was ? 1 : 0);
        break;
    }
    case 0x0E00:                                        /* coprocessor status: none */
        SET16(r->eax, 0x0040);                          /* a 486SX */
        break;
    default:
        err(r, 0x8001);
    }
}

/* ---- software interrupts in protected mode ------------------------------- */

static void soft_int(struct vm *vm, struct regs *r, uint8_t n)
{
    struct dpmi *dp = vm->dpmi;
    if (n == 0x31) { int31(vm, r); return; }
    if (n == 0x2F && AX(r) == 0x1686) { SET16(r->eax, 0); return; }     /* in protected mode */
    if (dp->vec_sel[n]) {                               /* its own handler */
        bool w = dp->bits32;
        if (!pm_push(dp, r, (r->eflags & ~EFLAGS_VM) | (vm->vif ? EFLAGS_IF : 0), w) ||
            !pm_push(dp, r, r->cs & 0xFFFF, w) || !pm_push(dp, r, r->eip, w)) {
            kill(vm, r, "stack fault calling an interrupt handler");
            return;
        }
        r->cs = dp->vec_sel[n];
        r->eip = dp->vec_off[n];
        r->eflags &= ~EFLAGS_TF;
        return;
    }
    if ((n == 0x21 && ((r->eax >> 8 & 0xFF) == 0x4C || (r->eax >> 8 & 0xFF) == 0x00)) || n == 0x20) {
        terminate(vm, r, n == 0x21 && (r->eax >> 8 & 0xFF) == 0x4C ? (uint8_t)r->eax : 0);
        return;
    }
    reflect_rm(vm, r, n, K_INT);
}

/* ---- breakpoints in real mode -------------------------------------------- */

/* The initial switch: far call with AX = 1 for a 32-bit client, ES = the
 * host's data area. */
static void rm_enter(struct vm *vm, struct regs *r, int n)
{
    (void)n;
    uint16_t ret_ip = v86_pop16(r), ret_cs = v86_pop16(r);
    r->cs = ret_cs;
    r->eip = ret_ip;
    if (vm->dpmi || !vm->set.dpmi_kb) {                 /* one client at a time */
        SET16(r->eax, 0x8011);
        r->eflags |= EFLAGS_CF;
        return;
    }
    struct dpmi *dp = kzalloc(sizeof *dp);
    uint32_t stub = vmx_alloc(vm, PAGE_SIZE, VMX_DPMI), hs = vmx_alloc(vm, HSTACK_SIZE, VMX_DPMI);
    if (!dp || !stub || !hs) {
        if (stub) vmx_free(vm, stub);
        if (hs) vmx_free(vm, hs);
        kfree(dp);
        SET16(r->eax, 0x8013);
        r->eflags |= EFLAGS_CF;
        return;
    }
    vm->dpmi = dp;
    dp->bits32 = r->eax & 1;
    dp->psp = dosmgr_current_psp(vm);
    dp->host_seg = (uint16_t)r->v86_es;
    dp->stub_lin = stub;
    dp->hstack_lin = hs;
    dp->hstack = HSTACK_SIZE;
    dp->rm_sp = RM_SP_TOP;
    uint8_t *code = (uint8_t *)(uintptr_t)stub;
    for (int i = 0; i < STUBS; i++) { code[2 * i] = 0xCD; code[2 * i + 1] = 0x30; }
    static const uint8_t tramp[] = { 0x2E, 0x0F, 0xB2, 0x25,           /* lss esp, cs:[TRAMP_TARGET] */
                                     TRAMP_TARGET & 0xFF, TRAMP_TARGET >> 8, 0, 0,
                                     0xCF };                            /* iretd */
    memcpy(code + TRAMP, tramp, sizeof tramp);
    rm_stubs(vm);

    dp->ldt[L_STUB32] = make_desc(stub, PAGE_SIZE - 1, 0xFA, 0x4);
    dp->ldt[L_STUB16] = make_desc(stub, PAGE_SIZE - 1, 0xFA, 0x0);
    dp->ldt[L_HSTACK] = make_desc(hs, HSTACK_SIZE - 1, 0xF2, dp->bits32 ? 0x4 : 0);
    dp->ldt[L_TRAMPSS] = make_desc(hs, HSTACK_SIZE - 1, 0xF2, 0x4);
    uint16_t cs = new_sel(dp, ret_cs << 4, 0xFFFF, 0xFA, 0);
    uint16_t ds = new_sel(dp, (uint32_t)(uint16_t)r->v86_ds << 4, 0xFFFF, 0xF2, 0);
    uint16_t ss = (uint16_t)r->user_ss == (uint16_t)r->v86_ds ? ds
                : new_sel(dp, (uint32_t)(uint16_t)r->user_ss << 4, 0xFFFF, 0xF2, 0);
    uint16_t es = new_sel(dp, (uint32_t)dp->psp << 4, 0xFF, 0xF2, 0);
    uint16_t *envp = (uint16_t *)(uintptr_t)(((uint32_t)dp->psp << 4) + 0x2C);
    dp->env_seg = *envp;
    if (dp->env_seg) {
        uint16_t env = new_sel(dp, (uint32_t)dp->env_seg << 4, 0xFFFF, 0xF2, 0);
        if (env) *envp = env;
        else dp->env_seg = 0;
    }
    dpmi_install_ldt(vm);
    to_pm(r, cs, ret_ip, ss, (uint16_t)r->user_esp, ds, es, 0, 0, r->eflags & ~EFLAGS_CF);
    klog("DPMI: VM %d: a %d-bit client (PSP %04X) entered protected mode\n",
         vm->id, dp->bits32 ? 32 : 16, dp->psp);
}

/* A real-mode INT or call made for the client is done. */
static void rm_returned(struct vm *vm, struct regs *r, int n)
{
    (void)n;
    struct dpmi *dp = vm->dpmi;
    if (!dp || !dp->depth || dp->nest[dp->depth - 1].kind > K_DOSRESIZE) {
        if (dp) kill(vm, r, "a real-mode return nobody waited for");
        return;
    }
    struct state s = dp->nest[--dp->depth];
    dp->rm_sp = s.rm_sp;
    uint32_t rmflags = r->eflags;
    switch (s.kind) {
    case K_INT: case K_STUB: {
        struct state t = s;                             /* general registers from real mode */
        t.eax = r->eax; t.ebx = r->ebx; t.ecx = r->ecx; t.edx = r->edx;
        t.esi = r->esi; t.edi = r->edi; t.ebp = r->ebp;
        t.eflags = (s.eflags & ~FLAGS_ARITH) | (rmflags & FLAGS_ARITH);
        restore_pm(r, &t);
        if (s.kind == K_STUB) {                         /* the default handler: IRET for it */
            uint32_t ip, cs, fl;
            bool w = dp->bits32;
            if (!pm_pop(dp, r, &ip, w) || !pm_pop(dp, r, &cs, w) || !pm_pop(dp, r, &fl, w)) {
                kill(vm, r, "stack fault returning from an interrupt");
                return;
            }
            r->eip = ip;
            r->cs = cs & 0xFFFF;
            r->eflags = (fl & EFLAGS_TF) | (rmflags & FLAGS_ARITH) | 0x202;
        }
        break;
    }
    case K_IRQ:
        restore_pm(r, &s);
        break;
    case K_CALL: {
        struct regs rr = *r;                            /* the real-mode registers ... */
        restore_pm(r, &s);
        struct rmregs *m = uptr(dp, s.arg2, s.arg, sizeof *m, true);
        if (m) frame_to_rmregs(m, &rr);                 /* ... into its structure */
        r->eflags &= ~EFLAGS_CF;
        break;
    }
    case K_DOSALLOC: case K_DOSFREE: case K_DOSRESIZE: {
        bool fail = rmflags & EFLAGS_CF;
        uint16_t rax = (uint16_t)r->eax, rbx = (uint16_t)r->ebx;
        restore_pm(r, &s);
        if (fail) {
            SET16(r->eax, rax);
            if (s.kind != K_DOSFREE) SET16(r->ebx, rbx);    /* the largest block there is */
            r->eflags |= EFLAGS_CF;
            break;
        }
        r->eflags &= ~EFLAGS_CF;
        if (s.kind == K_DOSALLOC) {
            uint16_t sel = new_sel(dp, (uint32_t)rax << 4, (uint32_t)(uint16_t)s.ebx * 16 - 1, 0xF2, 0);
            SET16(r->eax, rax);
            SET16(r->edx, sel);
        } else if (s.kind == K_DOSFREE) {
            uint64_t *d = client_desc(dp, (uint16_t)s.edx);
            if (d) *d = 0;
        } else {
            uint64_t *d = client_desc(dp, (uint16_t)s.edx);
            if (d) *d = make_desc(desc_base(*d), (uint32_t)(uint16_t)s.ebx * 16 - 1, (uint8_t)(*d >> 40), 0);
        }
        break;
    }
    }
    vm->vif = s.vif;
}

/* A real-mode callback was called: to its protected-mode procedure. */
static void rm_callback(struct vm *vm, struct regs *r, int n)
{
    struct dpmi *dp = vm->dpmi;
    int i = n - BRK_DPMI_CB;
    if (!dp || i < 0 || i >= CALLBACKS || !dp->cb[i].used) {
        r->eip = v86_pop16(r);                          /* nobody: just return */
        r->cs = v86_pop16(r);
        return;
    }
    struct callback *c = &dp->cb[i];
    struct rmregs *m = uptr(dp, c->regs_sel, c->regs_off, sizeof *m, true);
    if (!m) { kill(vm, r, "a callback's register structure is gone"); return; }
    frame_to_rmregs(m, r);
    m->cs = (uint16_t)r->cs; m->ip = (uint16_t)r->eip;
    m->ss = (uint16_t)r->user_ss; m->sp = (uint16_t)r->user_esp;
    struct state *s = push_state(vm, r, K_CB);
    if (!s) return;
    s->arg = (uint32_t)i;
    if ((uint16_t)r->user_ss == dp->host_seg)
        dp->rm_sp = (uint16_t)(r->user_esp - 0x20);
    dp->ldt[L_RMSS] = make_desc((uint32_t)(uint16_t)r->user_ss << 4, 0xFFFF, 0xF2, 0);
    uint32_t rsp = (uint16_t)r->user_esp;
    host_push(dp, 0x202, dp->bits32);                   /* its IRET -> S_CB_RET */
    host_push(dp, stub_cs(dp), dp->bits32);
    host_push(dp, stub_off(S_CB_RET), dp->bits32);
    to_pm(r, c->pm_sel, c->pm_off, SEL(L_HSTACK), dp->hstack, SEL(L_RMSS), c->regs_sel, 0, 0, 0x2);
    r->esi = rsp;
    r->edi = c->regs_off;
    vm->vif = false;
}

/* Raw switch, real -> protected: AX = DS, CX = ES, DX = SS, (E)BX = (E)SP,
 * SI = CS, (E)DI = (E)IP. */
static void rm_raw(struct vm *vm, struct regs *r, int n)
{
    (void)n;
    struct dpmi *dp = vm->dpmi;
    if (!dp) { r->eip = v86_pop16(r); r->cs = v86_pop16(r); return; }
    to_pm(r, (uint16_t)r->esi, dp->bits32 ? r->edi : (uint16_t)r->edi, (uint16_t)r->edx,
          dp->bits32 ? r->ebx : (uint16_t)r->ebx, (uint16_t)r->eax, (uint16_t)r->ecx, 0, 0, r->eflags);
}

static void rm_save(struct vm *vm, struct regs *r, int n)
{
    (void)vm; (void)n;
    r->eip = v86_pop16(r);                              /* RETF: nothing to save */
    r->cs = v86_pop16(r);
}

/* INT 2Fh AX=1687h in real mode: is there a host? */
bool dpmi_int2f(struct vm *vm, struct regs *r)
{
    if (AX(r) != 0x1687 || !vm->set.dpmi_kb || vm == system_vm)
        return false;
    SET16(r->eax, 0);
    SET16(r->ebx, 1);                                   /* 32-bit programs too */
    r->ecx = (r->ecx & ~0xFFu) | 4;
    SET16(r->edx, 0x005A);                              /* 0.90 */
    SET16(r->esi, HOST_PARAS);
    r->v86_es = v86_buf_seg();
    SET16(r->edi, v86_brk_off(BRK_DPMI_ENTER));
    return true;
}

/* A program ends from real mode (INT 21h AH=4Ch/00h, INT 20h) while it is
 * the client. */
void dpmi_rm_int(struct vm *vm, struct regs *r, uint8_t n)
{
    struct dpmi *dp = vm->dpmi;
    uint8_t ah = (uint8_t)(r->eax >> 8);
    if (dp && (n == 0x20 || (n == 0x21 && (ah == 0x4C || ah == 0x00))) &&
        dosmgr_current_psp(vm) == dp->psp)
        end_client(vm);
}

/* ---- traps from the client (ring 3) -------------------------------------- */

static void stub_service(struct vm *vm, struct regs *r, int s)
{
    struct dpmi *dp = vm->dpmi;
    bool w = dp->bits32;
    if (s < 0x100) {                                    /* a default interrupt handler */
        reflect_rm(vm, r, (uint8_t)s, K_STUB);
        return;
    }
    if (s >= 0x120 && s < 0x140) {                      /* a default exception handler */
        kill(vm, r, "an exception it didn't handle");
        return;
    }
    switch (s) {
    case S_EXC_RET: {                                   /* its exception handler RETFed */
        uint32_t e, ip, cs, fl, sp, ss;
        if (!dp->depth || dp->nest[dp->depth - 1].kind != 0xFF) { kill(vm, r, "a stray exception return"); return; }
        if (!pm_pop(dp, r, &e, w) || !pm_pop(dp, r, &ip, w) || !pm_pop(dp, r, &cs, w) ||
            !pm_pop(dp, r, &fl, w) || !pm_pop(dp, r, &sp, w) || !pm_pop(dp, r, &ss, w)) {
            kill(vm, r, "the exception frame is gone");
            return;
        }
        struct state st = dp->nest[--dp->depth];
        dp->hstack = st.hstack;
        to_pm(r, (uint16_t)cs, ip, (uint16_t)ss, sp, (uint16_t)st.ds, (uint16_t)st.es,
              (uint16_t)st.fs, (uint16_t)st.gs, fl);
        r->eax = st.eax; r->ebx = st.ebx; r->ecx = st.ecx; r->edx = st.edx;
        r->esi = st.esi; r->edi = st.edi; r->ebp = st.ebp;
        (void)e;
        break;
    }
    case S_IRQ_RET: {                                   /* its interrupt handler IRETed */
        if (!dp->depth || dp->nest[dp->depth - 1].kind != K_PMIRQ) { kill(vm, r, "a stray interrupt return"); return; }
        struct state st = dp->nest[--dp->depth];
        dp->hstack = st.hstack;
        dp->rm_sp = st.rm_sp;
        if (st.eflags & EFLAGS_VM) restore_rm(r, &st);
        else                       restore_pm(r, &st);
        vm->vif = st.vif;
        break;
    }
    case S_CB_RET: {                                    /* a callback procedure IRETed */
        if (!dp->depth || dp->nest[dp->depth - 1].kind != K_CB) { kill(vm, r, "a stray callback return"); return; }
        struct state st = dp->nest[--dp->depth];
        dp->hstack = st.hstack;
        dp->rm_sp = st.rm_sp;
        struct callback *c = &dp->cb[st.arg];
        struct rmregs *m = uptr(dp, c->regs_sel, c->regs_off, sizeof *m, false);
        if (!m) { kill(vm, r, "a callback's register structure is gone"); return; }
        rmregs_to_frame(r, m);
        to_rm(r, m->cs, m->ip, m->ss, m->sp, m->ds, m->es, m->flags);
        r->v86_fs = m->fs;
        r->v86_gs = m->gs;
        vm->vif = st.vif;
        break;
    }
    case S_RAW:                                         /* raw switch, protected -> real */
        to_rm(r, (uint16_t)r->esi, (uint16_t)r->edi, (uint16_t)r->edx, (uint16_t)r->ebx,
              (uint16_t)r->eax, (uint16_t)r->ecx, r->eflags);
        break;
    case S_SAVE: {                                      /* nothing to save: RETF */
        uint32_t ip, cs;
        if (!pm_pop(dp, r, &ip, w) || !pm_pop(dp, r, &cs, w)) { kill(vm, r, "stack fault"); return; }
        r->eip = ip;
        r->cs = cs & 0xFFFF;
        break;
    }
    default:
        kill(vm, r, "an unknown host entry");
    }
}

/* #GP from the client with no IDT error: an instruction to emulate. */
static bool emulate(struct vm *vm, struct regs *r)
{
    struct dpmi *dp = vm->dpmi;
    bool big = sel_big(dp, (uint16_t)r->cs);
    const uint8_t *c = uptr(dp, (uint16_t)r->cs, r->eip, 4, false);
    if (!c) return false;
    int i = 0, size = big ? 4 : 2;
    if (c[i] == 0x66) { size = big ? 2 : 4; i++; }
    uint16_t port;
    switch (c[i]) {
    case 0xFA: vm->vif = false; r->eip += (uint32_t)i + 1; return true;    /* CLI */
    case 0xFB: vm->vif = true; r->eip += (uint32_t)i + 1; return true;     /* STI */
    case 0xF4:                                                              /* HLT */
        r->eip += (uint32_t)i + 1;
        if (vm->vif) {
            uint32_t f = irq_save();
            while (!vpic_has_deliverable(&vm->pic)) { current->state = T_BLOCKED; schedule(); }
            irq_restore(f);
        }
        return true;
    case 0xE4: case 0xE5: case 0xE6: case 0xE7:
    case 0xEC: case 0xED: case 0xEE: case 0xEF: {
        bool imm = c[i] < 0xEC, out = c[i] & 2;
        int sz = (c[i] & 1) ? size : 1;
        port = imm ? c[i + 1] : (uint16_t)r->edx;
        uint32_t v = 0;
        for (int k = 0; k < sz; k++) {
            if (out) v86_port_out8(vm, (uint16_t)(port + k), (uint8_t)(r->eax >> (8 * k)));
            else     v |= (uint32_t)v86_port_in8(vm, (uint16_t)(port + k)) << (8 * k);
        }
        if (!out) {
            if (sz == 1) r->eax = (r->eax & ~0xFFu) | v;
            else if (sz == 2) r->eax = (r->eax & ~0xFFFFu) | v;
            else r->eax = v;
        }
        r->eip += (uint32_t)i + (imm ? 2u : 1u);
        return true;
    }
    }
    return false;
}

/* A trap or interrupt from the client's protected-mode code. Returns true
 * if dealt with (the frame then goes back to whichever mode it says). */
void dpmi_trap(struct regs *r)
{
    struct vm *vm = vm_current();
    struct dpmi *dp = vm->dpmi;
    if (r->int_no == SYSCALL_VECTOR) {                  /* INT 30h: a host stub, or the client's */
        uint16_t cs = (uint16_t)r->cs;
        if (cs == SEL(L_STUB32) || cs == SEL(L_STUB16))
            stub_service(vm, r, (int)((r->eip - 2) / 2));
        else
            soft_int(vm, r, 0x30);
        return;
    }
    if (r->int_no == 13 && (r->err_code & 2)) {         /* INT n through a ring-0 gate */
        uint8_t n = (uint8_t)(r->err_code >> 3);
        const uint8_t *c = uptr(dp, (uint16_t)r->cs, r->eip, 2, false);
        r->eip += c && c[0] == 0xCD ? 2 : 1;
        soft_int(vm, r, n);
        return;
    }
    if (r->int_no == 13 && !r->err_code && emulate(vm, r))
        return;
    if (r->int_no < 32 && r->int_no != 8 && dp->exc_sel[r->int_no]) {
        pm_exception(vm, r, (int)r->int_no, r->err_code);
        return;
    }
    char why[48];
    ksnprintf(why, sizeof why, "exception %02X%s", r->int_no,
              r->int_no == 14 ? (read_cr2() >= 0x80000000u ? " (the VMM's memory)" : " (page fault)") : "");
    kill(vm, r, why);
}

/* After an interrupt from the client (or from V86 code of a VM that has
 * one): a virtual IRQ to deliver? */
void dpmi_deliver(struct regs *r)
{
    struct vm *vm = vm_current();
    struct dpmi *dp = vm->dpmi;
    if (!dp || !vm->vif || (r->eflags & EFLAGS_VM))
        return;
    int vec = vpic_ack(&vm->pic);
    if (vec < 0)
        return;
    vm->delivered[vec]++;
    if (dp->vec_sel[vec])
        pm_irq(vm, r, (uint8_t)vec);
    else
        reflect_rm(vm, r, (uint8_t)vec, K_IRQ);
}

bool dpmi_client(struct vm *vm) { return vm && vm->dpmi; }

/*
 * Going back to ring 3 on a 16-bit stack segment, IRET loads only SP: the
 * top half of ESP would keep the VMM's (and code that uses ESP or an EBP
 * copied from it as a 32-bit address faults). So go back through a little
 * trampoline instead, on a 32-bit stack: the IRET frame goes on the
 * client's own stack, and the trampoline loads the whole of SS:ESP with
 * LSS and IRETs from there -- Linux's espfix, done the 1993 way.
 */
/* A selector the CPU will load without faulting (in the VMM's IRET or its
 * segment pops): code for CS, writable data for SS, data or readable code
 * for the rest; present, ring 3. */
static bool loadable(struct dpmi *dp, uint16_t sel, int what)
{
    if (!sel) return what == 0;
    uint64_t *d = desc(dp, sel);
    if (!d || (sel & 3) != 3) return false;
    uint8_t acc = (uint8_t)(*d >> 40);
    if ((acc & 0xF0) != 0xF0) return false;             /* present, DPL 3, code/data */
    bool code = acc & 0x08;
    if (what == 1) return code;
    if (what == 2) return !code && (acc & 0x02);
    return !code || (acc & 0x02);
}

void dpmi_espfix(struct regs *r)
{
    struct vm *vm = vm_current();
    struct dpmi *dp = vm->dpmi;
    if (!dp || (r->eflags & EFLAGS_VM) || (r->cs & 3) != 3)
        return;
    /* a selector freed or changed while loaded: null, as the CPU's cache
     * would have hidden it until the next load */
    if (!loadable(dp, (uint16_t)r->ds, 0)) r->ds = 0;
    if (!loadable(dp, (uint16_t)r->es, 0)) r->es = 0;
    if (!loadable(dp, (uint16_t)r->fs, 0)) r->fs = 0;
    if (!loadable(dp, (uint16_t)r->gs, 0)) r->gs = 0;
    if (!loadable(dp, (uint16_t)r->cs, 1) || !loadable(dp, (uint16_t)r->user_ss, 2)) {
        kill(vm, r, "its CS or SS is no longer usable");
        return;
    }
    if (sel_big(dp, (uint16_t)r->user_ss))
        return;
    uint16_t ss = (uint16_t)r->user_ss;
    uint32_t sp = (uint16_t)(r->user_esp - 12);
    uint32_t *f = uptr(dp, ss, sp, 12, true);
    if (!f)
        return;                                         /* (it will fault on its own) */
    f[0] = r->eip;
    f[1] = r->cs & 0xFFFF;
    f[2] = r->eflags;
    uint8_t *page = (uint8_t *)(uintptr_t)dp->stub_lin;
    *(uint32_t *)(page + TRAMP_TARGET) = (r->user_esp & 0xFFFF0000u) | sp;
    *(uint16_t *)(page + TRAMP_TARGET + 4) = ss;
    r->cs = SEL(L_STUB32);
    r->eip = TRAMP;
    r->user_ss = SEL(L_TRAMPSS);
    r->user_esp = (dp->hstack - 64) & ~3u;
}

void dpmi_vm_gone(struct vm *vm)
{
    if (vm->dpmi) {
        kfree(vm->dpmi);
        vm->dpmi = NULL;
    }
}

void dpmi_init(void)
{
    v86_set_brk(BRK_DPMI_ENTER, rm_enter);
    v86_set_brk(BRK_DPMI_RMRET, rm_returned);
    v86_set_brk(BRK_DPMI_RAW, rm_raw);
    v86_set_brk(BRK_DPMI_SAVE, rm_save);
    for (int i = 0; i < CALLBACKS; i++)
        v86_set_brk(BRK_DPMI_CB + i, rm_callback);
}
