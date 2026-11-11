/*
 * decomp.h -- decompressors (decomp.c): -> bytes produced, or -1.
 */
#ifndef OS1993_DECOMP_H
#define OS1993_DECOMP_H

#include <stdbool.h>
#include <stdint.h>

int inflate_raw(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t max);     /* deflate */
int inflate_zlib(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t max);    /* zlib's wrapper */
int lzo1x_decompress(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t max);
int zstd_decompress(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t max);

#endif
