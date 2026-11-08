/*
 * sched.h -- kernel threads and the preemptive round-robin scheduler.
 *
 * Every VM is run by one kernel thread. Thread 0 is the boot thread: the
 * VMM console, which also owns the System VM (as Windows ran in the System
 * VM). The timer preempts the running thread every SLICE_TICKS ticks.
 */
#ifndef OS1993_SCHED_H
#define OS1993_SCHED_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_THREADS   16
#define THREAD_STACK  8192
#define SLICE_TICKS   2         /* ~110 ms at 18.2 Hz */

struct vm;

enum thread_state { T_FREE, T_RUNNABLE, T_BLOCKED, T_DEAD };

struct thread {
    enum thread_state state;
    uint32_t    esp;            /* saved by switch_context */
    void       *stack;
    struct vm  *vm;
    const char *name;
    void      (*entry)(void *);
    void       *arg;
};

extern struct thread *current;

void sched_init(struct vm *system_vm);
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg,
                             struct vm *vm);   /* created blocked */
void thread_wake(struct thread *t);
__attribute__((noreturn)) void thread_exit(void);
bool thread_reap(struct thread *t);            /* free a dead thread */

void schedule(void);            /* interrupts must be off */
void sched_yield(void);
void sched_tick(void);          /* timer IRQ */
void sched_preempt(void);       /* end of every interrupt */
bool sched_others_runnable(void);
void sched_wait(void);          /* let others run, or idle until an IRQ */

void switch_context(uint32_t *save_esp, uint32_t next_esp);   /* switch.asm */

#endif
