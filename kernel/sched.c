/*
 * sched.c -- kernel threads, round-robin with timer preemption.
 *
 * Preemption happens only at the end of an interrupt (after the PIC EOI),
 * so a thread is always switched out at a well-defined point: inside
 * schedule(), called from sched_yield(), sched_preempt() or a VM blocking
 * on HLT. Kernel code that must not be interleaved runs with interrupts off.
 */
#include "kernel.h"
#include "io.h"
#include "gdt.h"
#include "mm.h"
#include "sched.h"
#include "vm.h"
#include "user.h"

static struct thread threads[MAX_THREADS];
struct thread *current;
static volatile bool need_resched;
static volatile unsigned slice;
static bool idling;

void sched_init(struct vm *system_vm)
{
    current = &threads[0];
    current->state = T_RUNNABLE;
    current->name = "System VM";
    current->vm = system_vm;
    system_vm->thread = current;
}

static void thread_start(void)
{
    cpu_sti();
    current->entry(current->arg);
    thread_exit();
}

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg, struct vm *vm)
{
    struct thread *t = NULL;
    for (int i = 1; i < MAX_THREADS; i++)
        if (threads[i].state == T_FREE) { t = &threads[i]; break; }
    if (!t)
        return NULL;
    t->stack = kmalloc(THREAD_STACK);
    if (!t->stack)
        return NULL;

    /* Initial stack, as switch_context will pop it: EFLAGS, EDI, ESI, EBX,
     * EBP, return address. */
    uint32_t *sp = (uint32_t *)((uint8_t *)t->stack + THREAD_STACK);
    *--sp = (uint32_t)(uintptr_t)thread_start;
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
    *--sp = 0x00000002;                         /* IF off until thread_start */
    t->esp   = (uint32_t)(uintptr_t)sp;
    t->name  = name;
    t->entry = fn;
    t->arg   = arg;
    t->vm    = vm;
    t->state = T_BLOCKED;
    return t;
}

void thread_wake(struct thread *t)
{
    if (t && t->state == T_BLOCKED)
        t->state = T_RUNNABLE;
}

void thread_exit(void)
{
    cpu_cli();
    current->state = T_DEAD;
    schedule();
    panic("Dead thread %s was scheduled.", current->name);
}

bool thread_reap(struct thread *t)
{
    if (!t || t->state != T_DEAD)
        return false;
    kfree(t->stack);
    memset(t, 0, sizeof *t);
    return true;
}

static struct thread *pick_next(void)
{
    int cur = (int)(current - threads);
    for (int n = 1; n <= MAX_THREADS; n++) {
        struct thread *t = &threads[(cur + n) % MAX_THREADS];
        if (t->state == T_RUNNABLE)
            return t;
    }
    return NULL;
}

void schedule(void)
{
    struct thread *next = pick_next();
    while (!next) {                         /* everyone blocked: idle */
        idling = true;
        cpu_idle();                         /* an IRQ may wake a thread */
        cpu_cli();
        idling = false;
        next = pick_next();
    }
    need_resched = false;
    slice = 0;
    if (next == current)
        return;

    struct thread *prev = current;
    current = next;
    vm_switch_to(next->vm);                 /* PDE 0 and TSS.ESP0 */
    proc_switch(next->proc);                /* user PDEs (ring-3 processes) */
    if (next->user) {                       /* traps from ring 3 land here */
        kernel_tss.esp0 = (uint32_t)(uintptr_t)next->stack + THREAD_STACK;
        if (next->tib)
            gdt_set_tls(next->tib);         /* FS, when it returns to ring 3 */
    }
    switch_context(&prev->esp, next->esp);
}

void sched_yield(void)
{
    uint32_t f = irq_save();
    schedule();
    irq_restore(f);
}

void sched_tick(void)
{
    if (++slice >= SLICE_TICKS)
        need_resched = true;
}

void sched_preempt(void)
{
    if (need_resched && !idling && current)
        schedule();
}

bool sched_others_runnable(void)
{
    for (int i = 0; i < MAX_THREADS; i++)
        if (&threads[i] != current && threads[i].state == T_RUNNABLE)
            return true;
    return false;
}

void sched_wait(void)
{
    if (sched_others_runnable())
        sched_yield();
    else
        cpu_idle();
}
