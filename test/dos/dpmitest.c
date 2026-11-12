/*
 * dpmitest.c -- a DOS/4GW program (Open Watcom: wcl386 -bt=dos -l=dos4g):
 * a 32-bit flat-model program that DOS/4GW runs through the VMM's DPMI host.
 * Prints PASS/FAIL lines; tools/qemu-dosvm-test.py runs it in a DOS session.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dos.h>
#include <i86.h>

static int fails;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

int main(void)
{
    union REGS r;
    unsigned char *big, *p;
    unsigned long i, sum;
    FILE *f;
    char line[64];
    unsigned short far *screen;

    check(1, "DOS/4GW started: a 32-bit flat program");

    memset(&r, 0, sizeof r);
    r.w.ax = 0x0400;                            /* DPMI version, through DOS/4GW's INT 31h */
    int386(0x31, &r, &r);
    printf("DPMI %u.%02u, flags %04X, processor %u\n", r.h.ah, r.h.al, r.w.bx, r.h.cl);
    check(r.h.ah == 0 && r.h.al >= 90, "INT 31h 0400h: DPMI 0.9");

    big = malloc(2u * 1024 * 1024);             /* 2 MB: more than DOS has */
    check(big != NULL, "malloc of 2 MB");
    if (big) {
        for (i = 0; i < 2u * 1024 * 1024; i++)
            big[i] = (unsigned char)(i * 7 + (i >> 12));
        sum = 0;
        for (i = 0, p = big; i < 2u * 1024 * 1024; i++, p++)
            if (*p != (unsigned char)(i * 7 + (i >> 12)))
                sum++;
        check(sum == 0, "2 MB written and read back");
        free(big);
    }

    f = fopen("DPMIFILE.TXT", "w");             /* file calls: DOS/4GW translates them for DOS */
    check(f != NULL, "fopen for writing");
    if (f) {
        fprintf(f, "written from protected mode\n");
        fclose(f);
    }
    f = fopen("DPMIFILE.TXT", "r");
    line[0] = 0;
    if (f) {
        fgets(line, sizeof line, f);
        fclose(f);
    }
    check(strcmp(line, "written from protected mode\n") == 0, "the file reads back");
    remove("DPMIFILE.TXT");

    check(time(NULL) > 0, "time()");

    screen = (unsigned short far *)MK_FP(0, 0xB8000 + 158);   /* flat: the text screen */
    *(unsigned short *)(0xB8000 + 158) = 0x2F21;
    check(*(unsigned short *)(0xB8000 + 158) == 0x2F21, "the text screen at B8000h, flat");
    (void)screen;

    printf("DPMITEST %s\n", fails ? "failed" : "done");
    return fails ? 1 : 0;
}
