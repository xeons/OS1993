#ifndef OS1993_IO_H
#define OS1993_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ~1us delay for slow ISA devices (POST code port). */
static inline void io_wait(void) { outb(0x80, 0); }

static inline void outw(uint16_t port, uint16_t v)
{
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void cpu_cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void cpu_sti(void) { __asm__ volatile("sti" ::: "memory"); }

/* Disable interrupts, returning the previous EFLAGS for irq_restore(). */
static inline uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ volatile("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    if (flags & 0x200)                   /* EFLAGS.IF */
        cpu_sti();
}

/* Enable interrupts and halt until the next one, atomically. */
static inline void cpu_idle(void) { __asm__ volatile("sti; hlt" ::: "memory"); }

static inline uint32_t read_cr0(void) { uint32_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint32_t read_cr2(void) { uint32_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint32_t read_cr3(void) { uint32_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint32_t v) { __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline void invlpg(uint32_t va)   { __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory"); }

#define CR0_WP 0x00010000u      /* 486: ring 0 honours read-only pages too */

/* Physical/linear low memory (identity mapped by the loader). */
#define LOWMEM8(addr)   (*(volatile uint8_t  *)(uintptr_t)(addr))
#define LOWMEM16(addr)  (*(volatile uint16_t *)(uintptr_t)(addr))
#define LOWMEM32(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))

#endif
