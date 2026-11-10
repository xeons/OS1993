/*
 * crttest.c -- the C runtime's tests. Built twice: CRTTEST.BIN imports
 * CRT.DLL, CRTSTAT.BIN has libcrt.a linked in.
 *
 * Without arguments it runs the checks and prints one line per failure and
 * a total (stdout is the debug log). With one, it breaks a rule on purpose
 * and the runtime must end it (tools/qemu-crt-test.py checks the log):
 *
 *   smash       overrun a local array: the stack canary
 *   heap        write past a malloc'd block: its guard
 *   double      free a block twice
 *   fortify     memcpy into a smaller array (_FORTIFY_SOURCE)
 *   constraint  strcpy_s into too small a buffer (Annex K's handler)
 *   percentn    printf with %n
 *   assert      a failing assert
 *   canary      print this process's stack canary
 */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern unsigned long __stack_chk_guard;

static int failures, checks;

#define CHECK(cond, what) \
    do { checks++; if (!(cond)) { failures++; printf("CRT: FAIL %s (line %d)\n", what, __LINE__); } } while (0)

static int same(const char *a, const char *b) { return !strcmp(a, b); }

/* ---- the deliberate failures ------------------------------------------------ */

__attribute__((noinline)) static void smash(size_t n)
{
    char buf[16];
    volatile char *p = buf;
    for (size_t i = 0; i < n; i++)
        p[i] = 'A';
    printf("CRT: wrote %u bytes into 16\n", (unsigned)n);
}

__attribute__((noinline)) static void fortify(const char *src, size_t n)
{
    char d[8];
    memcpy(d, src, n);
    printf("CRT: copied %u bytes into 8 (%c)\n", (unsigned)n, d[0]);
}

static int deliberate(const char *what, size_t n)
{
    if (same(what, "smash")) {
        smash(n * 8);
    } else if (same(what, "heap")) {
        volatile char *p = malloc(16);
        for (size_t i = 0; i < 16 + n; i++)
            p[i] = 'B';
        free((void *)p);
    } else if (same(what, "double")) {
        char *p = malloc(32);
        free(p);
        free(p);
    } else if (same(what, "fortify")) {
        static const char src[64] = "fortified";
        fortify(src, n * 4);
    } else if (same(what, "constraint")) {
        char d[4];
        strcpy_s(d, sizeof d, "far too long");
    } else if (same(what, "percentn")) {
        int count;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
        printf("CRT: abc%n\n", &count);
#pragma GCC diagnostic pop
    } else if (same(what, "assert")) {
        assert(n == 0);
    } else if (same(what, "canary")) {
        printf("CRT: canary %08lx\n", __stack_chk_guard);
        return 0;
    } else {
        printf("CRT: unknown test %s\n", what);
        return 2;
    }
    printf("CRT: %s survived\n", what);
    return 1;
}

/* ---- the checks ------------------------------------------------------------ */

/* Truncation and a null %s on purpose */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wformat-overflow"

static void test_printf(void)
{
    char s[128];
    int n = snprintf(s, sizeof s, "%d|%5s|%-4d|%04x|%X|%o|%#x|%c|%%|%+d|% d", -42, "ab", 7, 0xbe, 0xCAFEu, 8u,
                     255u, 'z', 5, 3);
    CHECK(same(s, "-42|   ab|7   |00be|CAFE|10|0xff|z|%|+5| 3"), "printf: basic conversions");
    CHECK(n == (int)strlen(s), "printf: returns the length");
    snprintf(s, sizeof s, "%llu %lld %lld", ULLONG_MAX, LLONG_MIN, 1234567890123LL);
    CHECK(same(s, "18446744073709551615 -9223372036854775808 1234567890123"), "printf: 64-bit numbers");
    snprintf(s, sizeof s, "%.3s|%*d|%-*d|%.5d|%hhd|%hu|%zu", "abcdef", 5, 42, 4, 1, 42, 300, 70000, sizeof(int));
    CHECK(same(s, "abc|   42|1   |00042|44|4464|4"), "printf: precision, *, lengths");
    snprintf(s, sizeof s, "%p", (void *)0x1234);
    CHECK(same(s, "0x00001234"), "printf: %p");
    n = snprintf(s, 6, "hello world");
    CHECK(n == 11 && same(s, "hello"), "snprintf: truncates, terminates, returns the full length");
    n = snprintf(NULL, 0, "%d", 12345);
    CHECK(n == 5, "snprintf: measuring with a null buffer");

    constraint_handler_t old = set_constraint_handler_s(ignore_handler_s);
    n = sprintf_s(s, 4, "%s", "toolong");
    CHECK(n == 0 && s[0] == '\0', "sprintf_s: output that doesn't fit is a constraint");
    n = sprintf_s(s, sizeof s, "%s", (char *)NULL);
    CHECK(n < 0 && s[0] == '\0', "sprintf_s: %s of NULL is a constraint");
    n = snprintf_s(s, 4, "%s", "toolong");
    CHECK(n == 7 && same(s, "too"), "snprintf_s: truncates");
    set_constraint_handler_s(old);
}

static void test_strings(void)
{
    char d[8];
    constraint_handler_t old = set_constraint_handler_s(ignore_handler_s);
    CHECK(strcpy_s(d, sizeof d, "1234567") == 0 && same(d, "1234567"), "strcpy_s: fits exactly");
    CHECK(strcpy_s(d, sizeof d, "12345678") != 0 && d[0] == '\0', "strcpy_s: too long empties d");
    CHECK(strcpy_s(NULL, 8, "x") == EINVAL, "strcpy_s: null d");
    strcpy_s(d, sizeof d, "abc");
    CHECK(strcat_s(d, sizeof d, "defg") == 0 && same(d, "abcdefg"), "strcat_s: fits");
    CHECK(strcat_s(d, sizeof d, "h") != 0 && d[0] == '\0', "strcat_s: overflow empties d");
    CHECK(strncpy_s(d, sizeof d, "abcdefghij", 3) == 0 && same(d, "abc"), "strncpy_s: n limits");
    CHECK(strncat_s(d, sizeof d, "xyz123", 2) == 0 && same(d, "abcxy"), "strncat_s: n limits");
    char buf[16] = "abcdefgh";
    CHECK(memcpy_s(buf + 2, 8, buf, 4) != 0 && !memcmp(buf, "ab\0\0\0\0\0\0", 8),
          "memcpy_s: overlap is a constraint (and empties d)");
    memcpy(buf, "abcdefgh", 8);
    CHECK(memmove_s(buf + 2, 8, buf, 4) == 0 && !memcmp(buf, "ababcdgh", 8), "memmove_s: overlap is fine");
    CHECK(memset_s(buf, 4, 'z', 8) != 0 && !memcmp(buf, "zzzzcdgh", 8), "memset_s: n > dmax fills dmax");
    CHECK(strnlen_s(NULL, 5) == 0 && strnlen_s("abcdef", 4) == 4, "strnlen_s");
    char text[] = "a,b,,c", *save, *tok;
    rsize_t left = sizeof text;
    int count = 0;
    for (tok = strtok_s(text, &left, ",", &save); tok; tok = strtok_s(NULL, &left, ",", &save))
        count++;
    CHECK(count == 3, "strtok_s: three tokens");
    set_constraint_handler_s(old);

    CHECK(strlcpy(d, "truncated", sizeof d) == 9 && same(d, "truncat"), "strlcpy: truncates, returns the length");
    CHECK(strlcat(d, "x", sizeof d) == 8 && same(d, "truncat"), "strlcat: full stays full");
    char t2[] = "one two  three", *sv;
    CHECK(same(strtok_r(t2, " ", &sv), "one") && same(strtok_r(NULL, " ", &sv), "two") &&
          same(strtok_r(NULL, " ", &sv), "three") && !strtok_r(NULL, " ", &sv), "strtok_r");
    CHECK(strstr("haystack", "st") && same(strstr("haystack", "st"), "stack") && !strstr("abc", "d"), "strstr");
    CHECK(strcasecmp("Hello", "hELLO") == 0 && strncmp("abcd", "abce", 3) == 0, "string comparisons");
    CHECK(strchr("abc", 'c') && !strchr("abc", 'x') && strrchr("abcabc", 'b') - "abcabc" >= 0, "strchr, strrchr");
    CHECK(strspn("aabbc", "ab") == 4 && strcspn("hello", "l") == 2, "strspn, strcspn");
    CHECK(timingsafe_memcmp("abc", "abd", 3) < 0 && timingsafe_memcmp("abd", "abc", 3) > 0 &&
          timingsafe_memcmp("abc", "abc", 3) == 0, "timingsafe_memcmp");
    char *dup = strdup("copy");
    CHECK(dup && same(dup, "copy"), "strdup");
    free(dup);
    CHECK(same(strerror(ENOENT), "No such file or directory"), "strerror");
    CHECK(isalpha('q') && !isalpha('1') && isspace('\t') && toupper('a') == 'A' && !isprint(200), "ctype");
}

static void test_numbers(void)
{
    char *end;
    CHECK(strtol("0x1F", &end, 0) == 31 && !*end, "strtol: hex prefix");
    CHECK(strtol("  -123xyz", &end, 10) == -123 && same(end, "xyz"), "strtol: sign, stops at junk");
    CHECK(strtol("0755", NULL, 0) == 493, "strtol: octal");
    errno = 0;
    CHECK(strtol("99999999999", NULL, 10) == LONG_MAX && errno == ERANGE, "strtol: overflow saturates");
    errno = 0;
    CHECK(strtol("-2147483648", NULL, 10) == LONG_MIN && errno == 0, "strtol: LONG_MIN");
    errno = 0;
    CHECK(strtoull("18446744073709551615", NULL, 10) == ULLONG_MAX && errno == 0, "strtoull: the largest");
    CHECK(strtoull("18446744073709551616", NULL, 10) == ULLONG_MAX && errno == ERANGE, "strtoull: one more overflows");
    CHECK(strtol("zz", &end, 36) == 1295, "strtol: base 36");
    CHECK(strtol("abc", &end, 10) == 0 && same(end, "abc"), "strtol: no digits");
    CHECK(atoi("42") == 42 && abs(-5) == 5 && div(7, 2).quot == 3 && div(7, 2).rem == 1, "atoi, abs, div");
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static void test_memory(void)
{
    int *v = malloc(200 * sizeof *v);
    CHECK(v != NULL, "malloc");
    srand(7);
    for (int i = 0; i < 200; i++)
        v[i] = rand() % 1000;
    qsort(v, 200, sizeof *v, cmp_int);
    int sorted = 1;
    for (int i = 1; i < 200; i++)
        sorted &= v[i - 1] <= v[i];
    CHECK(sorted, "qsort: 200 numbers");
    int key = v[123];
    int *hit = bsearch(&key, v, 200, sizeof *v, cmp_int);
    CHECK(hit && *hit == key, "bsearch");
    v = realloc(v, 400 * sizeof *v);
    CHECK(v && v[0] <= v[199] && malloc_usable_size(v) == 400 * sizeof *v, "realloc keeps the contents");
    free(v);
    errno = 0;
    CHECK(calloc(0x10000, 0x10001) == NULL && errno == ENOMEM, "calloc: an overflowing size fails");
    char *z = calloc(64, 1);
    int zero = 1;
    for (int i = 0; z && i < 64; i++)
        zero &= z[i] == 0;
    CHECK(z && zero, "calloc: zeroed");
    free(z);
    free(NULL);
    void *blocks[50];
    for (int i = 0; i < 50; i++)
        blocks[i] = malloc((size_t)(i * 37 % 300 + 1));
    for (int i = 0; i < 50; i += 2)
        free(blocks[i]);
    for (int i = 1; i < 50; i += 2)
        free(blocks[i]);
    CHECK(1, "malloc/free: 50 blocks out of order");
    unsigned a = 0, b = 0;
    CHECK(rand_s(&a) == 0 && rand_s(&b) == 0 && a != b, "rand_s: the kernel's random numbers");
}

static jmp_buf jb;

__attribute__((noinline)) static void jump_back(int v) { longjmp(jb, v); }

static void test_setjmp(void)
{
    volatile int passes = 0;
    int r = setjmp(jb);
    passes++;
    if (r == 0)
        jump_back(7);
    CHECK(r == 7 && passes == 2, "setjmp/longjmp");
    r = setjmp(jb);
    if (r == 0)
        jump_back(0);
    CHECK(r == 1, "longjmp(0) returns 1");
}

static void test_files(void)
{
    FILE *f = fopen("CRTTEST.TMP", "w");
    CHECK(f != NULL, "fopen for writing");
    if (!f)
        return;
    for (int i = 0; i < 100; i++)
        fprintf(f, "line %d\n", i);
    CHECK(ftell(f) == 790, "ftell while writing");
    CHECK(fclose(f) == 0, "fclose");
    f = fopen("CRTTEST.TMP", "r");
    char line[32];
    int lines = 0, good = 1;
    while (f && fgets(line, sizeof line, f)) {
        char want[32];
        snprintf(want, sizeof want, "line %d\n", lines);
        good &= same(line, want);
        lines++;
    }
    CHECK(f && lines == 100 && good && feof(f), "fgets reads every line back");
    CHECK(f && fseek(f, 7, SEEK_SET) == 0 && fgetc(f) == 'l' && ftell(f) == 8, "fseek, fgetc, ftell");
    CHECK(f && ungetc('Q', f) == 'Q' && fgetc(f) == 'Q' && ftell(f) == 8, "ungetc");
    char big[600];
    CHECK(f && fseek(f, 0, SEEK_SET) == 0 && fread(big, 1, sizeof big, f) == sizeof big &&
          !memcmp(big, "line 0\nline 1\n", 14), "fread across buffer refills");
    if (f) fclose(f);
    f = fopen("CRTTEST.TMP", "a");
    fputs("appended\n", f);
    fclose(f);
    f = fopen("CRTTEST.TMP", "r");
    long size = -1;
    if (f && fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    CHECK(size == 799, "append mode writes at the end");
    if (f) fclose(f);
    f = fopen("CRTTEST.TMP", "wx");
    CHECK(!f && errno == EEXIST, "\"wx\" refuses an existing file");
    CHECK(remove("CRTTEST.TMP") == 0, "remove");
    errno = 0;
    CHECK(fopen("NOSUCH.TMP", "r") == NULL && errno == ENOENT, "fopen: a missing file is ENOENT");
    CHECK(fopen("X.TMP", "rw") == NULL && errno == EINVAL, "fopen: a bad mode");
}

static void test_time(void)
{
    struct tm tm;
    time_t zero = 0;
    gmtime_r(&zero, &tm);
    CHECK(tm.tm_year == 70 && tm.tm_mon == 0 && tm.tm_mday == 1 && tm.tm_wday == 4, "gmtime(0): 1970-01-01, a Thursday");
    struct tm leap = { 0, 0, 0, 29, 1, 100, 0, 0, 0 };
    CHECK(mktime(&leap) == 951782400 && leap.tm_wday == 2 && leap.tm_yday == 59, "mktime: 2000-02-29");
    char s[40];
    strftime(s, sizeof s, "%Y-%m-%d %H:%M:%S %a", &leap);
    CHECK(same(s, "2000-02-29 00:00:00 Tue"), "strftime");
    CHECK(time(NULL) > 946684800, "time: after 2000");
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return deliberate(argv[1], strlen(argv[1]));
    test_printf();
    test_strings();
    test_numbers();
    test_memory();
    test_setjmp();
    test_files();
    test_time();
    printf("CRT: %s: %d of %d checks passed\n", argv[0], checks - failures, checks);
    return failures;
}
