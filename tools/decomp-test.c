/* decomp-test.c -- the kernel's decompressors (kernel/fs/decomp.c), on the
 * host: decomp-test zlib|zstd|lzo COMPRESSED ORIGINAL */
#define DECOMP_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/fs/decomp.c"

static unsigned char *slurp(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n + 1);
    if (fread(b, 1, *n, f) != (size_t)*n) exit(2);
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    long cn, on;
    unsigned char *c = slurp(argv[2], &cn), *o = slurp(argv[3], &on);
    static unsigned char out[1 << 24];
    int r = !strcmp(argv[1], "zlib") ? inflate_zlib(c, cn, out, sizeof out)
          : !strcmp(argv[1], "zstd") ? zstd_decompress(c, cn, out, sizeof out)
          : lzo1x_decompress(c, cn, out, sizeof out);
    int ok = r == on && !memcmp(out, o, on);
    printf("%s %s: %d of %ld bytes, %s\n", argv[1], argv[3], r, on, ok ? "same" : "DIFFERENT");
    return !ok;
}
