/*
 * decomp.c -- decompressors for btrfs: zlib (inflate, RFC 1950/1951),
 * LZO1X and zstd (RFC 8878). Each takes a whole compressed buffer and
 * writes at most `max` bytes; -> the bytes produced, or -1 if the data is
 * damaged. Nothing here allocates; zstd's tables live in a static
 * workspace (callers hold the file system lock).
 *
 * Plain C with memcpy/memset only, so it builds on the host too
 * (tools/decomp-test.c checks it against zlib, lzop-style and zstd data).
 */
#ifdef DECOMP_HOST
#include <stdint.h>
#include <string.h>
#else
#include "kernel.h"
#endif
#include "decomp.h"

/* ---- inflate ------------------------------------------------------------------------ */

struct inflate {
    const uint8_t *in;
    uint32_t inlen, pos, bitbuf, bitcnt;
    uint8_t *out;
    uint32_t outlen, max;
    int err;
};

static uint32_t bits(struct inflate *s, int need)
{
    while (s->bitcnt < (uint32_t)need) {
        if (s->pos >= s->inlen) {
            s->err = 1;
            return 0;
        }
        s->bitbuf |= (uint32_t)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    uint32_t v = s->bitbuf & ((1u << need) - 1);
    s->bitbuf >>= need;
    s->bitcnt -= (uint32_t)need;
    return v;
}

struct huff { uint16_t count[16], symbol[320]; };

static int huff_build(struct huff *h, const uint8_t *lengths, int n)
{
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++)
        h->count[lengths[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++)
        offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    return 0;
}

static int huff_decode(struct inflate *s, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)bits(s, 1);
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->err)
            return -1;
    }
    s->err = 1;
    return -1;
}

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                       67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                       4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                        9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(struct inflate *s, const struct huff *lit, const struct huff *dist)
{
    for (;;) {
        int sym = huff_decode(s, lit);
        if (s->err || sym < 0)
            return -1;
        if (sym < 256) {
            if (s->outlen >= s->max)
                return -1;
            s->out[s->outlen++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29)
                return -1;
            uint32_t len = len_base[sym] + bits(s, len_extra[sym]);
            int d = huff_decode(s, dist);
            if (s->err || d < 0 || d >= 30)
                return -1;
            uint32_t back = dist_base[d] + bits(s, dist_extra[d]);
            if (s->err || back > s->outlen || s->outlen + len > s->max)
                return -1;
            for (uint32_t i = 0; i < len; i++, s->outlen++)
                s->out[s->outlen] = s->out[s->outlen - back];
        }
    }
}

static int dynamic(struct inflate *s)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t lengths[320];
    int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30)
        return -1;
    memset(lengths, 0, sizeof lengths);
    for (int i = 0; i < ncode; i++)
        lengths[order[i]] = (uint8_t)bits(s, 3);
    struct huff lc, lit, dist;
    huff_build(&lc, lengths, 19);
    for (int i = 0; i < nlen + ndist;) {
        int sym = huff_decode(s, &lc);
        if (s->err || sym < 0)
            return -1;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        int rep, val = 0;
        if (sym == 16) {
            if (!i)
                return -1;
            val = lengths[i - 1];
            rep = 3 + (int)bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + (int)bits(s, 3);
        } else {
            rep = 11 + (int)bits(s, 7);
        }
        if (i + rep > nlen + ndist)
            return -1;
        while (rep--)
            lengths[i++] = (uint8_t)val;
    }
    huff_build(&lit, lengths, nlen);
    huff_build(&dist, lengths + nlen, ndist);
    return codes(s, &lit, &dist);
}

static int fixed(struct inflate *s)
{
    static struct huff lit, dist;
    static int built;
    if (!built) {
        uint8_t l[320];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        huff_build(&lit, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        huff_build(&dist, l, 30);
        built = 1;
    }
    return codes(s, &lit, &dist);
}

int inflate_raw(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t max)
{
    struct inflate s = { in, inlen, 0, 0, 0, out, 0, max, 0 };
    int last;
    do {
        last = (int)bits(&s, 1);
        int type = (int)bits(&s, 2);
        if (s.err)
            return -1;
        if (type == 0) {                                /* stored */
            s.bitbuf = s.bitcnt = 0;
            if (s.pos + 4 > inlen)
                return -1;
            uint32_t len = in[s.pos] | in[s.pos + 1] << 8;
            s.pos += 4;
            if (s.pos + len > inlen || s.outlen + len > max)
                return -1;
            memcpy(out + s.outlen, in + s.pos, len);
            s.pos += len;
            s.outlen += len;
        } else if (type == 1) {
            if (fixed(&s) < 0)
                return -1;
        } else if (type == 2) {
            if (dynamic(&s) < 0)
                return -1;
        } else {
            return -1;
        }
    } while (!last);
    return (int)s.outlen;
}

int inflate_zlib(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t max)
{
    if (inlen < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20))
        return -1;                                      /* deflate, no preset dictionary */
    return inflate_raw(in + 2, inlen - 2, out, max);
}

/* ---- LZO1X -------------------------------------------------------------------------- */

int lzo1x_decompress(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t max)
{
    const uint8_t *ip = in, *ie = in + inlen;
    uint8_t *op = out, *oe = out + max;
    const uint8_t *m;
    uint32_t t;
#define NEED_IP(n) do { if ((uint32_t)(ie - ip) < (uint32_t)(n)) return -1; } while (0)
#define NEED_OP(n) do { if ((uint32_t)(oe - op) < (uint32_t)(n)) return -1; } while (0)
#define CHECK_M()  do { if (m < out) return -1; } while (0)
    NEED_IP(1);
    if (*ip > 17) {
        t = *ip++ - 17u;
        if (t < 4)
            goto match_next;
        NEED_OP(t);
        NEED_IP(t + 1);
        while (t--) *op++ = *ip++;
        goto first_literal_run;
    }
    for (;;) {
        NEED_IP(1);
        t = *ip++;
        if (t >= 16)
            goto match;
        if (t == 0) {
            for (;;) {
                NEED_IP(1);
                if (*ip)
                    break;
                t += 255;
                ip++;
            }
            t += 15u + *ip++;
        }
        NEED_OP(t + 3);
        NEED_IP(t + 4);
        for (t += 3; t; t--) *op++ = *ip++;
first_literal_run:
        t = *ip++;
        if (t >= 16)
            goto match;
        NEED_IP(1);
        m = op - (1 + 0x0800);
        m -= t >> 2;
        m -= *ip++ << 2;
        CHECK_M();
        NEED_OP(3);
        *op++ = *m++; *op++ = *m++; *op++ = *m;
        goto match_done;
        for (;;) {
match:
            if (t >= 64) {
                NEED_IP(1);
                m = op - 1;
                m -= (t >> 2) & 7;
                m -= *ip++ << 3;
                t = (t >> 5) - 1;
            } else if (t >= 32) {
                t &= 31;
                if (t == 0) {
                    for (;;) {
                        NEED_IP(1);
                        if (*ip)
                            break;
                        t += 255;
                        ip++;
                    }
                    t += 31u + *ip++;
                }
                NEED_IP(2);
                m = op - 1;
                m -= (ip[0] >> 2) + (ip[1] << 6);
                ip += 2;
            } else if (t >= 16) {
                m = op;
                m -= (t & 8) << 11;
                t &= 7;
                if (t == 0) {
                    for (;;) {
                        NEED_IP(1);
                        if (*ip)
                            break;
                        t += 255;
                        ip++;
                    }
                    t += 7u + *ip++;
                }
                NEED_IP(2);
                m -= (ip[0] >> 2) + (ip[1] << 6);
                ip += 2;
                if (m == op)
                    return (int)(op - out);             /* the end marker */
                m -= 0x4000;
            } else {
                NEED_IP(1);
                m = op - 1;
                m -= t >> 2;
                m -= *ip++ << 2;
                CHECK_M();
                NEED_OP(2);
                *op++ = *m++;
                *op++ = *m;
                goto match_done;
            }
            CHECK_M();
            NEED_OP(t + 2);
            for (t += 2; t; t--) *op++ = *m++;          /* overlapping: byte by byte */
match_done:
            t = ip[-2] & 3u;
            if (t == 0)
                break;
match_next:
            NEED_OP(t);
            NEED_IP(t + 1);
            while (t--) *op++ = *ip++;
            t = *ip++;
        }
    }
#undef NEED_IP
#undef NEED_OP
#undef CHECK_M
}

/* ---- zstd ---------------------------------------------------------------------------- */

/* A bit stream read backwards (from its last byte, past the padding bit). */
struct bitr {
    const uint8_t *start;
    int64_t bitpos;                     /* bits still unread, counting from the start */
};

static int bitr_init(struct bitr *b, const uint8_t *p, uint32_t len)
{
    if (!len || !p[len - 1])
        return -1;
    int hi = 7;
    while (!(p[len - 1] & (1 << hi)))
        hi--;
    b->start = p;
    b->bitpos = (int64_t)(len - 1) * 8 + hi;            /* the padding 1 isn't data */
    return 0;
}

/* n bits (<= 32), MSB first going backwards; past the start: zeros. */
static uint32_t bitr_read(struct bitr *b, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        b->bitpos--;
        uint32_t bit = 0;
        if (b->bitpos >= 0)
            bit = (b->start[b->bitpos >> 3] >> (b->bitpos & 7)) & 1;
        v = v << 1 | bit;
    }
    return v;
}

static uint32_t bitr_peek(struct bitr *b, int n)
{
    int64_t keep = b->bitpos;
    uint32_t v = bitr_read(b, n);
    b->bitpos = keep;
    return v;
}

static int highbit(uint32_t v) { int n = -1; while (v) { v >>= 1; n++; } return n; }

/* FSE decoding tables */
struct fse_entry { uint16_t base; uint8_t sym, bits; };
struct fse { int al; struct fse_entry t[512]; };

static int fse_build(struct fse *f, const int16_t *norm, int maxsym, int al)
{
    int size = 1 << al, high = size - 1;
    uint16_t next[256];
    uint8_t syms[512];
    if (al > 9 || maxsym > 255)
        return -1;
    for (int s = 0; s <= maxsym; s++) {
        if (norm[s] == -1) {
            syms[high--] = (uint8_t)s;
            next[s] = 1;
        } else {
            next[s] = (uint16_t)(norm[s] > 0 ? norm[s] : 0);
        }
    }
    int step = (size >> 1) + (size >> 3) + 3, pos = 0;
    for (int s = 0; s <= maxsym; s++)
        for (int i = 0; i < norm[s]; i++) {
            syms[pos] = (uint8_t)s;
            do
                pos = (pos + step) & (size - 1);
            while (pos > high);
        }
    if (pos)
        return -1;
    for (int i = 0; i < size; i++) {
        int s = syms[i];
        uint32_t n = next[s]++;
        int nb = al - highbit(n);
        f->t[i].sym = (uint8_t)s;
        f->t[i].bits = (uint8_t)nb;
        f->t[i].base = (uint16_t)((n << nb) - (uint32_t)size);
    }
    f->al = al;
    return 0;
}

/* A table description (normalized counts), bits read forwards from the
 * low end. -> bytes used. */
static int fse_read(struct fse *f, const uint8_t *p, uint32_t len, int maxsym_allowed, int max_al)
{
    int16_t norm[256];
    memset(norm, 0, sizeof norm);
    uint32_t bitpos = 0;
    uint32_t total = len * 8;
#define FPEEK(n) ({ uint32_t _v = 0; for (int _i = 0; _i < (n); _i++) { uint32_t _b = bitpos + (uint32_t)_i; \
        if (_b < total) _v |= (uint32_t)((p[_b >> 3] >> (_b & 7)) & 1) << _i; } _v; })
    if (total < 4)
        return -1;
    int al = (int)FPEEK(4) + 5;
    bitpos += 4;
    if (al > max_al)
        return -1;
    int remaining = (1 << al) + 1, threshold = 1 << al, nbits = al + 1, sym = 0;
    bool previous0 = false;
    while (remaining > 1 && sym <= maxsym_allowed) {
        if (previous0) {                                /* zeros: 2-bit repeat counts, 3 = more */
            for (;;) {
                uint32_t rep = FPEEK(2);
                bitpos += 2;
                for (uint32_t i = 0; i < rep && sym <= maxsym_allowed; i++)
                    norm[sym++] = 0;
                if (rep != 3)
                    break;
            }
            if (sym > maxsym_allowed)
                break;
            previous0 = false;
        }
        int max = 2 * threshold - 1 - remaining, count;
        uint32_t low = FPEEK(nbits - 1);
        if ((int)low < max) {
            count = (int)low;
            bitpos += (uint32_t)(nbits - 1);
        } else {
            count = (int)FPEEK(nbits);
            if (count >= threshold)
                count -= max;
            bitpos += (uint32_t)nbits;
        }
        if (bitpos > total)
            return -1;
        count--;                                        /* -1: "less than one" */
        remaining -= count < 0 ? -count : count;
        norm[sym++] = (int16_t)count;
        previous0 = count == 0;
        while (remaining < threshold) {
            nbits--;
            threshold >>= 1;
        }
    }
#undef FPEEK
    if (remaining != 1 || sym > maxsym_allowed + 1)
        return -1;
    if (fse_build(f, norm, sym - 1, al) < 0)
        return -1;
    return (int)((bitpos + 7) >> 3);
}

/* Huffman */
struct hufd { int maxbits; uint8_t sym[2048], len[2048]; };

static int huf_from_weights(struct hufd *h, const uint8_t *w, int n)
{
    uint32_t total = 0;
    for (int i = 0; i < n; i++) {
        if (w[i] > 11)
            return -1;
        if (w[i])
            total += 1u << (w[i] - 1);
    }
    if (!total)
        return -1;
    int maxbits = highbit(total) + 1;
    uint32_t rest = (1u << maxbits) - total;
    if (rest & (rest - 1))
        return -1;                                      /* the last weight makes it a power of two */
    uint8_t weights[256];
    memcpy(weights, w, (size_t)n);
    weights[n++] = (uint8_t)(highbit(rest) + 1);
    if (maxbits > 11)
        return -1;
    h->maxbits = maxbits;
    uint32_t pos = 0;                                   /* weight 1 (the longest codes) first */
    for (int wt = 1; wt <= maxbits; wt++)
        for (int s = 0; s < n; s++)
            if (weights[s] == wt)
                for (uint32_t k = 0; k < 1u << (wt - 1); k++) {
                    if (pos >= 2048)
                        return -1;
                    h->sym[pos] = (uint8_t)s;
                    h->len[pos] = (uint8_t)(maxbits + 1 - wt);
                    pos++;
                }
    return pos == (1u << maxbits) ? 0 : -1;
}

static int huf_read_table(struct hufd *h, const uint8_t *p, uint32_t len)
{
    if (!len)
        return -1;
    uint8_t w[256];
    int n = 0, hb = p[0];
    if (hb >= 128) {                                    /* direct: 4 bits each */
        n = hb - 127;
        uint32_t bytes = ((uint32_t)n + 1) / 2;
        if (1 + bytes > len)
            return -1;
        for (int i = 0; i < n; i++)
            w[i] = (uint8_t)(i & 1 ? p[1 + i / 2] & 15 : p[1 + i / 2] >> 4);
        if (huf_from_weights(h, w, n) < 0)
            return -1;
        return (int)(1 + bytes);
    }
    if (1u + (uint32_t)hb > len)                         /* FSE-compressed weights */
        return -1;
    static struct fse f;
    int used = fse_read(&f, p + 1, (uint32_t)hb, 255, 6);
    if (used < 0)
        return -1;
    struct bitr b;
    if (bitr_init(&b, p + 1 + used, (uint32_t)(hb - used)) < 0)
        return -1;
    uint32_t s1 = bitr_read(&b, f.al), s2 = bitr_read(&b, f.al);
    for (;;) {
        if (n >= 255)
            return -1;
        w[n++] = f.t[s1].sym;
        s1 = f.t[s1].base + bitr_read(&b, f.t[s1].bits);
        if (b.bitpos < 0) {
            w[n++] = f.t[s2].sym;
            break;
        }
        if (n >= 255)
            return -1;
        w[n++] = f.t[s2].sym;
        s2 = f.t[s2].base + bitr_read(&b, f.t[s2].bits);
        if (b.bitpos < 0) {
            w[n++] = f.t[s1].sym;
            break;
        }
    }
    if (huf_from_weights(h, w, n) < 0)
        return -1;
    return 1 + hb;
}

static int huf_stream(const struct hufd *h, const uint8_t *p, uint32_t len, uint8_t *out, uint32_t n)
{
    struct bitr b;
    if (bitr_init(&b, p, len) < 0)
        return -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = bitr_peek(&b, h->maxbits);
        out[i] = h->sym[v];
        b.bitpos -= h->len[v];
        if (b.bitpos < 0 && i + 1 < n)
            return -1;
    }
    return b.bitpos == 0 ? 0 : -1;
}

static const uint32_t ll_base[36] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40,
                                      48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
static const uint8_t ll_bits[36] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3,
                                     4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const uint32_t ml_base[53] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
                                      26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131,
                                      259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539 };
static const uint8_t ml_bits[53] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                     0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const int16_t ll_def[36] = { 4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1,
                                    1, 1, 1, 1, -1, -1, -1, -1 };
static const int16_t ml_def[53] = { 1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1 };
static const int16_t of_def[29] = { 1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1,
                                    -1, -1 };

struct zstd {
    struct hufd huf;
    bool have_huf;
    struct fse ll, of, ml;
    uint32_t rep[3];
    uint8_t lit[131072 + 64];
};

static struct zstd zs;

static int seq_table(struct fse *f, int mode, const uint8_t *p, uint32_t len, const int16_t *def, int defmax,
                     int defal, int maxsym, int maxal)
{
    if (mode == 0)
        return fse_build(f, def, defmax, defal) < 0 ? -1 : 0;
    if (mode == 1) {                                    /* RLE: one symbol */
        if (!len || p[0] > maxsym)
            return -1;
        f->al = 0;
        f->t[0] = (struct fse_entry){ 0, p[0], 0 };
        return 1;
    }
    if (mode == 2)
        return fse_read(f, p, len, maxsym, maxal);
    return 0;                                           /* repeat: the last block's */
}

static int zstd_block(const uint8_t *p, uint32_t len, uint8_t *out, uint32_t *pos, uint32_t max)
{
    /* literals */
    if (!len)
        return -1;
    uint32_t ltype = p[0] & 3, sf = (p[0] >> 2) & 3, regen, csize = 0, hdr;
    if (ltype < 2) {
        if (sf == 0 || sf == 2) { regen = p[0] >> 3; hdr = 1; }
        else if (sf == 1) { if (len < 2) return -1; regen = (p[0] >> 4) + ((uint32_t)p[1] << 4); hdr = 2; }
        else { if (len < 3) return -1; regen = (p[0] >> 4) + ((uint32_t)p[1] << 4) + ((uint32_t)p[2] << 12); hdr = 3; }
    } else {
        if (sf <= 1) {
            if (len < 3) return -1;
            regen = (p[0] >> 4) | ((uint32_t)(p[1] & 0x3F) << 4);
            csize = (p[1] >> 6) | ((uint32_t)p[2] << 2);
            hdr = 3;
        } else if (sf == 2) {
            if (len < 4) return -1;
            regen = (p[0] >> 4) | ((uint32_t)p[1] << 4) | ((uint32_t)(p[2] & 3) << 12);
            csize = (p[2] >> 2) | ((uint32_t)p[3] << 6);
            hdr = 4;
        } else {
            if (len < 5) return -1;
            regen = (p[0] >> 4) | ((uint32_t)p[1] << 4) | ((uint32_t)(p[2] & 0x3F) << 12);
            csize = (p[2] >> 6) | ((uint32_t)p[3] << 2) | ((uint32_t)p[4] << 10);
            hdr = 5;
        }
    }
    if (regen > 131072)
        return -1;
    uint32_t at = hdr;
    if (ltype == 0) {
        if (at + regen > len) return -1;
        memcpy(zs.lit, p + at, regen);
        at += regen;
    } else if (ltype == 1) {
        if (at + 1 > len) return -1;
        memset(zs.lit, p[at], regen);
        at += 1;
    } else {
        if (at + csize > len) return -1;
        const uint8_t *q = p + at;
        uint32_t qlen = csize;
        if (ltype == 2) {
            int used = huf_read_table(&zs.huf, q, qlen);
            if (used < 0) return -1;
            q += used;
            qlen -= (uint32_t)used;
            zs.have_huf = true;
        } else if (!zs.have_huf) {
            return -1;
        }
        if (sf == 0) {
            if (huf_stream(&zs.huf, q, qlen, zs.lit, regen) < 0) return -1;
        } else {
            if (qlen < 6) return -1;
            uint32_t s1 = q[0] | q[1] << 8, s2 = q[2] | q[3] << 8, s3 = q[4] | q[5] << 8;
            if (6 + s1 + s2 + s3 > qlen) return -1;
            uint32_t s4 = qlen - 6 - s1 - s2 - s3, seg = (regen + 3) / 4;
            if (seg * 3 > regen) return -1;
            const uint8_t *d = q + 6;
            if (huf_stream(&zs.huf, d, s1, zs.lit, seg) < 0 ||
                huf_stream(&zs.huf, d + s1, s2, zs.lit + seg, seg) < 0 ||
                huf_stream(&zs.huf, d + s1 + s2, s3, zs.lit + 2 * seg, seg) < 0 ||
                huf_stream(&zs.huf, d + s1 + s2 + s3, s4, zs.lit + 3 * seg, regen - 3 * seg) < 0)
                return -1;
        }
        at += csize;
    }
    /* sequences */
    if (at >= len) {
        if (*pos + regen > max) return -1;
        memcpy(out + *pos, zs.lit, regen);
        *pos += regen;
        return 0;
    }
    uint32_t nseq = p[at++];
    if (nseq >= 128) {
        if (nseq < 255) {
            if (at >= len) return -1;
            nseq = ((nseq - 128) << 8) + p[at++];
        } else {
            if (at + 2 > len) return -1;
            nseq = p[at] + ((uint32_t)p[at + 1] << 8) + 0x7F00;
            at += 2;
        }
    }
    uint32_t litpos = 0;
    if (nseq) {
        if (at >= len) return -1;
        int modes = p[at++], r;
        if ((r = seq_table(&zs.ll, (modes >> 6) & 3, p + at, len - at, ll_def, 35, 6, 35, 9)) < 0) return -1;
        at += (uint32_t)r;
        if ((r = seq_table(&zs.of, (modes >> 4) & 3, p + at, len - at, of_def, 28, 5, 31, 8)) < 0) return -1;
        at += (uint32_t)r;
        if ((r = seq_table(&zs.ml, (modes >> 2) & 3, p + at, len - at, ml_def, 52, 6, 52, 9)) < 0) return -1;
        at += (uint32_t)r;
        struct bitr b;
        if (at >= len || bitr_init(&b, p + at, len - at) < 0) return -1;
        uint32_t sl = bitr_read(&b, zs.ll.al), so = bitr_read(&b, zs.of.al), sm = bitr_read(&b, zs.ml.al);
        for (uint32_t i = 0; i < nseq; i++) {
            uint32_t ofc = zs.of.t[so].sym, mlc = zs.ml.t[sm].sym, llc = zs.ll.t[sl].sym;
            if (ofc > 31 || mlc > 52 || llc > 35) return -1;
            uint32_t ofv = (1u << ofc) + bitr_read(&b, (int)ofc);
            uint32_t ml = ml_base[mlc] + bitr_read(&b, ml_bits[mlc]);
            uint32_t ll = ll_base[llc] + bitr_read(&b, ll_bits[llc]);
            uint32_t off;
            if (ofv > 3) {
                off = ofv - 3;
                zs.rep[2] = zs.rep[1];
                zs.rep[1] = zs.rep[0];
                zs.rep[0] = off;
            } else {
                uint32_t idx = ofv - 1 + (ll == 0);     /* with no literals, the repeats shift by one */
                if (idx == 0) {
                    off = zs.rep[0];
                } else {
                    off = idx == 3 ? zs.rep[0] - 1 : zs.rep[idx];
                    if (idx > 1)
                        zs.rep[2] = zs.rep[1];
                    zs.rep[1] = zs.rep[0];
                    zs.rep[0] = off;
                }
            }
            if (i + 1 < nseq) {                         /* the states: LL, ML, OF */
                sl = zs.ll.t[sl].base + bitr_read(&b, zs.ll.t[sl].bits);
                sm = zs.ml.t[sm].base + bitr_read(&b, zs.ml.t[sm].bits);
                so = zs.of.t[so].base + bitr_read(&b, zs.of.t[so].bits);
            }
            if (litpos + ll > regen || *pos + ll + ml > max) return -1;
            memcpy(out + *pos, zs.lit + litpos, ll);
            *pos += ll;
            litpos += ll;
            if (!off || off > *pos) return -1;
            for (uint32_t k = 0; k < ml; k++, (*pos)++)
                out[*pos] = out[*pos - off];
        }
        if (b.bitpos != 0) return -1;
    }
    if (*pos + regen - litpos > max) return -1;
    memcpy(out + *pos, zs.lit + litpos, regen - litpos);
    *pos += regen - litpos;
    return 0;
}

int zstd_decompress(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t max)
{
    uint32_t at = 0, pos = 0;
    while (at + 4 <= inlen) {
        uint32_t magic = in[at] | in[at + 1] << 8 | in[at + 2] << 16 | (uint32_t)in[at + 3] << 24;
        if (!magic && pos)
            break;                                      /* zeros after a frame: padding (btrfs pads to a sector) */
        at += 4;
        if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {     /* a skippable frame */
            if (at + 4 > inlen) return -1;
            uint32_t n = in[at] | in[at + 1] << 8 | in[at + 2] << 16 | (uint32_t)in[at + 3] << 24;
            at += 4 + n;
            continue;
        }
        if (magic != 0xFD2FB528u || at >= inlen)
            return -1;
        uint8_t fhd = in[at++];
        bool single = fhd & 0x20, checksum = fhd & 0x04;
        uint32_t dict = fhd & 3, fcs = fhd >> 6;
        if (fhd & 0x08) return -1;                      /* reserved */
        if (!single) at++;                              /* the window descriptor */
        static const uint8_t dsz[4] = { 0, 1, 2, 4 };
        if (dict && dsz[dict]) {
            uint32_t id = 0;
            for (uint32_t i = 0; i < dsz[dict]; i++) id |= (uint32_t)in[at + i] << (8 * i);
            at += dsz[dict];
            if (id) return -1;                          /* no dictionaries here */
        }
        static const uint8_t fsz[4] = { 0, 2, 4, 8 };
        at += fcs == 0 ? (single ? 1 : 0) : fsz[fcs];
        zs.rep[0] = 1;
        zs.rep[1] = 4;
        zs.rep[2] = 8;
        zs.have_huf = false;
        for (;;) {
            if (at + 3 > inlen) return -1;
            uint32_t bh = in[at] | in[at + 1] << 8 | (uint32_t)in[at + 2] << 16;
            at += 3;
            uint32_t last = bh & 1, type = (bh >> 1) & 3, size = bh >> 3;
            if (type == 0) {
                if (at + size > inlen || pos + size > max) return -1;
                memcpy(out + pos, in + at, size);
                pos += size;
                at += size;
            } else if (type == 1) {
                if (at + 1 > inlen || pos + size > max) return -1;
                memset(out + pos, in[at], size);
                pos += size;
                at += 1;
            } else if (type == 2) {
                if (at + size > inlen || size > 131072) return -1;
                if (zstd_block(in + at, size, out, &pos, max) < 0) return -1;
                at += size;
            } else {
                return -1;
            }
            if (last)
                break;
        }
        if (checksum)
            at += 4;                                    /* (its xxHash isn't checked: btrfs checks the sectors) */
    }
    return (int)pos;
}
