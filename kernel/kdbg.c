/*
 * kdbg.c -- the kernel debugger, on COM1, in the spirit of WDEB386.
 *
 * Started with OS1993 /D. A terminal on COM1 (115200 8N1) gets the "#"
 * prompt when
 *
 *   - Ctrl+C arrives on the line (polled from the timer interrupt),
 *   - a breakpoint set here is hit (the 486's debug registers DR0-DR3:
 *     execution, write or read/write; nothing is patched into code),
 *   - a single step (T) completes, or an INT 3 executes in the kernel,
 *   - the VMM panics or double-faults: a last look before the blue screen.
 *
 * Interrupts stay off while the prompt is up; the machine is frozen.
 * Addresses are linear ("80001234"), real-mode in the current VM
 * ("B800:0000"), or physical ("%100000"). Memory that isn't mapped shows
 * as "??" instead of faulting.
 *
 *   R [reg=val]        registers (EAX..EDI, EIP, EFL, ESP of the frame)
 *   D[B|W|D] addr [n]  dump n bytes (128)       E addr b b ...  enter bytes
 *   BP addr            execution breakpoint     BW/BR addr [len] data watch
 *   BL / BC n|*        list / clear             G [addr]   go (to addr)
 *   T [n]              single step              K          stack (EBP chain)
 *   .T .VM .P .M       threads, VMs, processes, memory
 *   .PT addr           how addr is mapped       .CRASH     overflow a stack (test)
 *   .EXIT              back to DOS              Q          leave the debugger
 */
#include "kernel.h"
#include "gdt.h"
#include "interrupts.h"
#include "io.h"
#include "kdbg.h"
#include "mm.h"
#include "sched.h"
#include "serial.h"
#include "v86.h"
#include "vm.h"
#include "user.h"

#define EFLAGS_RF 0x00010000u

static struct {
    bool     on;
    bool     inside;
    int      steps;                 /* T n: steps still to go */
    uint32_t bp[4];
    uint8_t  bp_kind[4];            /* 0 free, 1 exec, 2 write, 3 read/write */
    uint8_t  bp_len[4];
    struct regs fault;              /* a double fault's state (no frame of its own) */
    bool     have_fault;
} k;

bool kdbg_enabled(void) { return k.on; }

void kdbg_init(const struct bootinfo *bi)
{
    k.on = (bi->flags & BI_FLAG_DEBUG) && serial_present();
    if (k.on)
        klog("KDBG: kernel debugger on COM1; Ctrl+C breaks in\n");
}

/* ---- the line ----------------------------------------------------------- */

static void out(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    serial_puts(buf);
}

static int getch(void)
{
    int c;
    while ((c = serial_getc()) < 0)
        ;
    return c;
}

static void read_line(char *buf, int size)
{
    int n = 0;
    for (;;) {
        int c = getch();
        if (c == '\r' || c == '\n') {
            if (c == '\r' || n) break;      /* a lone LF after CR: skip */
            continue;
        }
        if ((c == 8 || c == 127) && n) { n--; out("\b \b"); continue; }
        if (c == 3) { n = 0; out("^C\n#"); continue; }
        if (c >= ' ' && n < size - 1) { buf[n++] = (char)c; serial_putc((char)c); }
    }
    buf[n] = '\0';
    out("\n");
}

/* ---- parsing ------------------------------------------------------------ */

static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static const char *skip(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

static bool hex(const char **ps, uint32_t *v)
{
    const char *s = skip(*ps);
    uint32_t n = 0;
    int d = 0;
    for (;; s++, d++) {
        char c = upper(*s);
        if (c >= '0' && c <= '9') n = n << 4 | (uint32_t)(c - '0');
        else if (c >= 'A' && c <= 'F') n = n << 4 | (uint32_t)(c - 'A' + 10);
        else break;
    }
    if (!d) return false;
    *v = n;
    *ps = s;
    return true;
}

enum space { LINEAR, PHYS };

/* addr: "lin", "seg:off" (the current VM's real-mode memory), "%phys" */
static bool address(const char **ps, uint32_t *a, enum space *sp)
{
    const char *s = skip(*ps);
    *sp = LINEAR;
    if (*s == '%') { *sp = PHYS; s++; }
    uint32_t v, off;
    if (!hex(&s, &v)) return false;
    if (*s == ':') {
        s++;
        if (!hex(&s, &off)) return false;
        v = ((v & 0xFFFF) << 4) + (off & 0xFFFF);
    }
    *a = v;
    *ps = s;
    return true;
}

/* A pointer to readable memory, or NULL. */
static uint8_t *at(uint32_t a, enum space sp, bool write)
{
    if (sp == PHYS)
        return phys_to_virt(a);
    uint32_t pa;
    if (!vmm_translate(a, &pa))
        return NULL;
    if (write && !vmm_writable(a)) {        /* CR0.WP: go through the frame */
        uint8_t *p = phys_to_virt(pa);
        return p;
    }
    return (uint8_t *)(uintptr_t)a;
}

/* ---- commands ----------------------------------------------------------- */

static void show_regs(const struct regs *r)
{
    bool v86 = r->eflags & EFLAGS_VM, user = !v86 && (r->cs & 3);
    uint32_t esp = v86 || user || r == &k.fault ? r->user_esp : (uint32_t)(uintptr_t)&r->user_esp;
    out("EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X\n",
        r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi);
    out("EIP=%08X ESP=%08X EBP=%08X EFL=%08X CS=%04X DS=%04X ES=%04X SS=%04X\n",
        r->eip, esp, r->ebp, r->eflags, r->cs & 0xFFFF,
        (v86 ? r->v86_ds : r->ds) & 0xFFFF, (v86 ? r->v86_es : r->es) & 0xFFFF,
        v86 || user ? r->user_ss & 0xFFFF : KDATA_SEL);
    out("%s, thread %s, VM %d; CR2=%08X CR3=%08X DR6=%08X\n",
        v86 ? "V86 mode" : user ? "ring 3" : "ring 0",
        current ? current->name : "?", vm_current()->id, read_cr2(), read_cr3(),
        ({ uint32_t d; __asm__ volatile("mov %%dr6, %0" : "=r"(d)); d; }));
    uint32_t lin = v86 ? ((r->cs & 0xFFFF) << 4) + (r->eip & 0xFFFF) : r->eip;
    out("%04X:%08X ", r->cs & 0xFFFF, r->eip);
    for (uint32_t i = 0; i < 8; i++) {
        uint8_t *b = at(lin + i, LINEAR, false);
        if (b) out("%02X ", *b); else out("?? ");
    }
    out("\n");
}

static uint32_t *reg_slot(struct regs *r, const char *name)
{
    static const char *const names[] = { "EAX", "EBX", "ECX", "EDX", "ESI", "EDI", "EBP", "EIP", "EFL" };
    uint32_t *slots[] = { &r->eax, &r->ebx, &r->ecx, &r->edx, &r->esi, &r->edi, &r->ebp, &r->eip, &r->eflags };
    for (unsigned i = 0; i < ARRAY_SIZE(names); i++)
        if (upper(name[0]) == names[i][0] && upper(name[1]) == names[i][1] && upper(name[2]) == names[i][2])
            return slots[i];
    return NULL;
}

static uint32_t last_dump;
static enum space last_space;

static void dump(const char *s, int unit)
{
    uint32_t a = last_dump, n = 128;
    enum space sp = last_space;
    if (!address(&s, &a, &sp)) { a = last_dump; sp = last_space; }
    hex(&s, &n);
    if (n > 4096) n = 4096;
    for (uint32_t row = 0; row < n; row += 16) {
        out("%s%08X  ", sp == PHYS ? "%" : "", a + row);
        char text[17];
        for (uint32_t i = 0; i < 16; i += (uint32_t)unit) {
            uint8_t *p = at(a + row + i, sp, false);
            text[i] = '.';
            if (!p) { out(unit == 1 ? "?? " : unit == 2 ? "???? " : "???????? "); continue; }
            if (unit == 1) out("%02X%s", *p, i == 7 ? "-" : " ");
            else if (unit == 2) out("%04X ", *(uint16_t *)p);
            else out("%08X ", *(uint32_t *)p);
            for (int j = 0; j < unit; j++)
                text[i + (uint32_t)j] = p[j] >= ' ' && p[j] < 127 ? (char)p[j] : '.';
        }
        text[16] = '\0';
        out(" %s\n", text);
    }
    last_dump = a + n;
    last_space = sp;
}

static void enter_bytes(const char *s)
{
    uint32_t a, v;
    enum space sp;
    if (!address(&s, &a, &sp)) { out("E addr byte ...\n"); return; }
    while (hex(&s, &v)) {
        uint8_t *p = at(a, sp, true);
        if (!p) { out("%08X is not mapped\n", a); return; }
        *p = (uint8_t)v;
        a++;
    }
}

static void load_dr7(void)
{
    uint32_t dr7 = 0x700;                   /* GE, LE: exact data breakpoints */
    for (int i = 0; i < 4; i++) {
        if (!k.bp_kind[i]) continue;
        uint32_t rw = k.bp_kind[i] == 1 ? 0 : k.bp_kind[i] == 2 ? 1 : 3;
        uint32_t len = k.bp_kind[i] == 1 ? 0 : k.bp_len[i] == 4 ? 3 : k.bp_len[i] == 2 ? 1 : 0;
        dr7 |= 2u << (i * 2);               /* global enable */
        dr7 |= (rw | len << 2) << (16 + i * 4);
    }
    __asm__ volatile("mov %0, %%dr0" : : "r"(k.bp[0]));
    __asm__ volatile("mov %0, %%dr1" : : "r"(k.bp[1]));
    __asm__ volatile("mov %0, %%dr2" : : "r"(k.bp[2]));
    __asm__ volatile("mov %0, %%dr3" : : "r"(k.bp[3]));
    __asm__ volatile("mov %0, %%dr7" : : "r"(dr7));
}

static void set_bp(const char *s, int kind)
{
    uint32_t a, len = 1;
    enum space sp;
    if (!address(&s, &a, &sp) || sp == PHYS) { out("B%c addr%s\n", kind == 1 ? 'P' : kind == 2 ? 'W' : 'R',
                                                  kind == 1 ? "" : " [1|2|4]"); return; }
    hex(&s, &len);
    if (len != 2 && len != 4) len = 1;
    for (int i = 0; i < 4; i++)
        if (!k.bp_kind[i]) {
            k.bp[i] = len > 1 ? a & ~(len - 1) : a;
            k.bp_kind[i] = (uint8_t)kind;
            k.bp_len[i] = (uint8_t)len;
            load_dr7();
            out("breakpoint %d at %08X\n", i, k.bp[i]);
            return;
        }
    out("all four breakpoints are in use\n");
}

static void list_bp(void)
{
    static const char *const kinds[] = { "", "execute", "write", "read/write" };
    for (int i = 0; i < 4; i++)
        if (k.bp_kind[i])
            out("%d  %08X  %s%s\n", i, k.bp[i], kinds[k.bp_kind[i]],
                k.bp_kind[i] > 1 ? (k.bp_len[i] == 4 ? " 4" : k.bp_len[i] == 2 ? " 2" : " 1") : "");
}

static void clear_bp(const char *s)
{
    s = skip(s);
    uint32_t n;
    if (*s == '*') memset(k.bp_kind, 0, sizeof k.bp_kind);
    else if (hex(&s, &n) && n < 4) k.bp_kind[n] = 0;
    else { out("BC n|*\n"); return; }
    load_dr7();
}

static void backtrace(const struct regs *r)
{
    if (r->eflags & EFLAGS_VM || (r->cs & 3)) { out("not in the kernel\n"); return; }
    out("%08X\n", r->eip);
    uint32_t ebp = r->ebp;
    for (int i = 0; i < 32 && ebp >= 0x80000000u; i++) {
        uint32_t *f = (uint32_t *)(uintptr_t)ebp;
        if (!at(ebp, LINEAR, false) || !at(ebp + 4, LINEAR, false)) break;
        out("%08X  (frame %08X)\n", f[1], ebp);
        if (f[0] <= ebp) break;
        ebp = f[0];
    }
}

static void page_walk(const char *s)
{
    uint32_t a;
    enum space sp;
    if (!address(&s, &a, &sp) || sp == PHYS) { out(".PT addr\n"); return; }
    uint32_t pde = ((volatile uint32_t *)PD_WINDOW)[a >> 22];
    out("PDE %03X = %08X", a >> 22, pde);
    if (pde & PTE_P) {
        uint32_t pte = ((volatile uint32_t *)PT_WINDOW)[a >> 12];
        out("  PTE = %08X%s%s%s%s", pte, pte & PTE_P ? " P" : " not present",
            pte & PTE_W ? " W" : "", pte & PTE_U ? " U" : "", pte & PTE_D ? " D" : "");
        if (pte & PTE_P) out("  -> %%%08X", (pte & PAGE_MASK) | (a & ~PAGE_MASK));
    }
    out("\n");
}

static void list_threads(void)
{
    static const char *const st[] = { "free", "runnable", "blocked", "dead" };
    for (int i = 0; i < MAX_THREADS; i++) {
        struct thread *t = thread_get(i);
        if (!t) continue;
        out("%2d %c %-9s VM %d %s%s  stack %08X esp %08X\n", i, t == current ? '*' : ' ', st[t->state],
            t->vm ? t->vm->id : -1, t->name, t->user ? " (ring 3)" : "",
            (uint32_t)(uintptr_t)t->stack, t->esp);
    }
}

static void list_vms(void)
{
    static const char *const st[] = { "free", "ready", "running", "finished" };
    struct vm *f = vm_focus();
    for (int i = 0; i < VM_MAX; i++) {
        struct vm *vm = vm_get(i);
        if (!vm) continue;
        out("%d %c %-8s %-10s DOS depth %d, IRR %04X ISR %04X IMR %04X, %u keys%s\n",
            i, vm == vm_current() ? '*' : ' ', st[vm->state], vm->name, vm->dos_depth,
            vm->pic.irr, vm->pic.isr, vm->pic.imr, vm->keys, vm == f ? ", focus" : "");
    }
}

static void memory(void)
{
    struct heap_stats h;
    heap_get_stats(&h);
    out("frames: %u of %u free (%u KB); heap %u KB mapped, %u used, %u free in %u blocks\n",
        pmm_free_pages(), pmm_total_pages(), pmm_free_pages() * 4, h.mapped / 1024,
        h.used / 1024, h.free / 1024, h.free_blocks);
    user_dbg_memory(out);
}

static int crash_depth(int n)                       /* deep, and not a tail call */
{
    volatile char pad[256];
    pad[0] = (char)n;
    return n > 0 ? crash_depth(n + 1) + pad[0] : 0;
}

static void crash_thread(void *arg)
{
    (void)arg;
    klog("KDBG: overflowing this thread's kernel stack\n");
    crash_depth(1);
}

static const char help[] =
    "R [reg=val]  D[B|W|D] addr [n]  E addr b ...  BP addr  BW|BR addr [len]  BL  BC n|*\n"
    "G [addr]  T [n]  K  .T  .VM  .P  .M  .PT addr  .CRASH  .EXIT  Q\n"
    "addr: linear, seg:off (this VM), %phys\n";

/* The prompt. Returns when the machine should go on. */
static void command_loop(struct regs *r)
{
    char line[96];
    for (;;) {
        out("#");
        read_line(line, sizeof line);
        const char *s = skip(line);
        char c0 = upper(s[0]), c1 = upper(s[1]);
        if (!c0) continue;
        if (c0 == '?' || c0 == 'H') { out(help); continue; }
        if (c0 == 'Q') return;
        if (c0 == '.') {
            const char *w = s + 1;
            char n0 = upper(w[0]), n1 = upper(w[1]);
            if (n0 == 'T') list_threads();
            else if (n0 == 'V') list_vms();
            else if (n0 == 'P' && n1 == 'T') page_walk(w + 2);
            else if (n0 == 'P') user_dbg_list(out);
            else if (n0 == 'M') memory();
            else if (n0 == 'C') {
                struct thread *t = thread_create("crash test", crash_thread, NULL, NULL);
                if (t) { thread_wake(t); out("a thread will overflow its stack: G\n"); }
            } else if (n0 == 'E') kernel_shutdown(0);
            else out("?\n");
            continue;
        }
        if (!r) {                               /* panic: memory only */
            if (c0 == 'D') dump(s + (c1 == 'B' || c1 == 'W' || c1 == 'D' ? 2 : 1), c1 == 'W' ? 2 : c1 == 'D' ? 4 : 1);
            else if (c0 == 'G') return;
            else out("no frame here: D, .T, .VM, .M, .PT, Q\n");
            continue;
        }
        switch (c0) {
        case 'R': {
            const char *a = skip(s + 1);
            if (!*a) { show_regs(r); break; }
            uint32_t *slot = reg_slot(r, a), v;
            const char *eq = a;
            while (*eq && *eq != '=') eq++;
            if (!slot || !*eq || (eq++, !hex(&eq, &v))) { out("R reg=value\n"); break; }
            *slot = v;
            break;
        }
        case 'D':
            dump(s + (c1 == 'B' || c1 == 'W' || c1 == 'D' ? 2 : 1), c1 == 'W' ? 2 : c1 == 'D' ? 4 : 1);
            break;
        case 'E': enter_bytes(s + 1); break;
        case 'B':
            if (c1 == 'P') set_bp(s + 2, 1);
            else if (c1 == 'W') set_bp(s + 2, 2);
            else if (c1 == 'R') set_bp(s + 2, 3);
            else if (c1 == 'L') list_bp();
            else if (c1 == 'C') clear_bp(s + 2);
            else out("?\n");
            break;
        case 'K': backtrace(r); break;
        case 'T': {
            uint32_t n = 1;
            const char *a = s + 1;
            hex(&a, &n);
            k.steps = (int)(n ? n : 1);
            r->eflags |= EFLAGS_TF | EFLAGS_RF;
            return;
        }
        case 'G': {
            uint32_t a;
            enum space sp;
            const char *p = s + 1;
            if (address(&p, &a, &sp) && sp == LINEAR) {     /* a one-shot breakpoint in slot 3 */
                k.bp[3] = a;
                k.bp_kind[3] = 1;
                load_dr7();
            }
            r->eflags |= EFLAGS_RF;             /* don't stop again on this instruction */
            return;
        }
        default:
            out("? (H for help)\n");
        }
    }
}

void kdbg_enter(struct regs *r, const char *why)
{
    if (!k.on || k.inside)
        return;
    uint32_t f = irq_save();
    k.inside = true;
    out("\n*** KDBG: %s\n", why);
    if (r) show_regs(r);
    command_loop(r);
    k.inside = false;
    irq_restore(f);
}

void kdbg_poll(struct regs *r)
{
    if (k.on && !k.inside && serial_getc() == 3)
        kdbg_enter(r, "break (Ctrl+C)");
}

bool kdbg_trap(struct regs *r)
{
    if (!k.on || k.inside)
        return false;
    if (r->int_no == 3) {                       /* INT 3 compiled into the kernel */
        if (r->eflags & EFLAGS_VM || (r->cs & 3))
            return false;
        kdbg_enter(r, "INT 3");
        return true;
    }
    uint32_t dr6;
    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
    __asm__ volatile("mov %0, %%dr6" : : "r"(0u));
    if (dr6 & 0x4000) {                         /* BS: our single step */
        if (k.steps <= 0)
            return false;
        r->eflags &= ~EFLAGS_TF;
        if (--k.steps > 0) {
            r->eflags |= EFLAGS_TF;
            return true;
        }
        kdbg_enter(r, "step");
        return true;
    }
    for (int i = 0; i < 4; i++)
        if ((dr6 & (1u << i)) && k.bp_kind[i]) {
            char why[40];
            ksnprintf(why, sizeof why, "breakpoint %d at %08X", i, k.bp[i]);
            if (i == 3 && k.bp_kind[3] == 1 && k.steps == 0) {  /* G addr's one-shot */
                k.bp_kind[3] = 0;
                load_dr7();
            }
            kdbg_enter(r, why);
            return true;
        }
    return false;
}

void kdbg_set_fault_regs(const struct regs *r)
{
    k.fault = *r;
    k.have_fault = true;
}

void kdbg_panic(const char *msg)
{
    if (!k.on || k.inside)
        return;
    k.inside = true;
    out("\n*** KDBG: the VMM stopped: %s\n", msg);
    if (k.have_fault) {
        show_regs(&k.fault);
        out("(the frame of the fault; Q goes on to the blue screen)\n");
    } else {
        out("(no frame; Q goes on to the blue screen)\n");
    }
    command_loop(k.have_fault ? &k.fault : NULL);
    k.inside = false;
}
