/*
 * random.h -- the kernel's random numbers (random.c): ChaCha20 keyed from
 * interrupt timing.
 */
#ifndef OS1993_RANDOM_H
#define OS1993_RANDOM_H

#include <stdint.h>

void     random_init(void);
void     random_mix(uint32_t v);                /* interrupts: a bit of timing */
void     random_bytes(void *buf, uint32_t n);
uint32_t random_u32(void);

#endif
