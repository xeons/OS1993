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

static inline void cpu_cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void cpu_sti(void) { __asm__ volatile("sti" ::: "memory"); }

/* Enable interrupts and halt until the next one, atomically. */
static inline void cpu_idle(void) { __asm__ volatile("sti; hlt" ::: "memory"); }

static inline uint32_t read_cr0(void) { uint32_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint32_t read_cr2(void) { uint32_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint32_t read_cr3(void) { uint32_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }

/* Physical/linear low memory (identity mapped by the loader). */
#define LOWMEM8(addr)   (*(volatile uint8_t  *)(uintptr_t)(addr))
#define LOWMEM16(addr)  (*(volatile uint16_t *)(uintptr_t)(addr))
#define LOWMEM32(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))

#endif
