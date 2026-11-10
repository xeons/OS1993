/*
 * random.c -- the kernel's random numbers (SYS_RANDOM; stack canaries, and
 * later TCP sequence numbers and the like).
 *
 * A 1993 PC has no hardware random source, so entropy comes from timing:
 * every interrupt mixes in where it interrupted (EIP, ESP, EAX) and the tick
 * count, and the boot adds the real-time clock and the PIT's counter. The
 * pool keys a ChaCha20 generator; each request first folds the pool into
 * the key, and the key is replaced after every request ("fast key
 * erasure"), so earlier output can't be recovered from the state.
 */
#include "kernel.h"
#include "devices.h"
#include "io.h"
#include "random.h"

static uint32_t pool[16];
static unsigned pool_pos;
static uint32_t key[8];
static uint32_t counter;

static inline uint32_t rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void random_mix(uint32_t v)
{
    unsigned i = pool_pos;
    pool[i] = rotl(pool[i] ^ v, 7) + pool[(i + 5) & 15] * 0x9E3779B1u;
    pool_pos = (i + 1) & 15;
}

#define QR(a, b, c, d) \
    a += b; d = rotl(d ^ a, 16); c += d; b = rotl(b ^ c, 12); \
    a += b; d = rotl(d ^ a, 8);  c += d; b = rotl(b ^ c, 7)

static void chacha_block(uint32_t out[16])
{
    uint32_t s[16] = { 0x61707865, 0x3320646E, 0x79622D32, 0x6B206574,
                       key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
                       counter++, 0, 0x4F533139, 0x39335247 };    /* nonce: "OS1993RG" */
    uint32_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = s[i];
    for (int r = 0; r < 10; r++) {
        QR(x[0], x[4], x[8],  x[12]); QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]); QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++)
        out[i] = x[i] + s[i];
}

void random_bytes(void *buf, uint32_t n)
{
    uint32_t f = irq_save();
    random_mix(timer_ticks());
    for (int i = 0; i < 8; i++)                     /* fold the pool into the key */
        key[i] ^= pool[i] + rotl(pool[i + 8], 13);
    uint32_t block[16];
    uint8_t *out = buf;
    while (n) {
        chacha_block(block);
        uint32_t take = n < 64 ? n : 64;
        memcpy(out, block, take);
        out += take;
        n -= take;
    }
    chacha_block(block);                            /* a new key: forget this one */
    for (int i = 0; i < 8; i++)
        key[i] = block[i];
    memset(block, 0, sizeof block);
    irq_restore(f);
}

uint32_t random_u32(void)
{
    uint32_t v;
    random_bytes(&v, sizeof v);
    return v;
}

static uint8_t cmos(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

void random_init(void)
{
    for (uint8_t reg = 0; reg <= 9; reg++)          /* the RTC: seconds .. year */
        random_mix(cmos(reg) | (uint32_t)reg << 8);
    for (int i = 0; i < 32; i++) {                  /* the PIT's counter (no VM runs yet) */
        outb(0x43, 0x00);
        uint32_t lo = inb(0x40), hi = inb(0x40);
        random_mix(lo | hi << 8 | (uint32_t)i << 16);
    }
    random_mix(timer_ticks());
    klog("RANDOM: seeded from the RTC and the PIT; interrupts add timing\n");
}
