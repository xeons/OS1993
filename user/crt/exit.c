/*
 * exit.c -- starting a program (argc/argv), and ending it: exit, atexit,
 * abort, assertions.
 */
#include "crt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os1993.h"

#define ATEXIT_MAX 32
#define ARGV_MAX   32

static void (*handlers[ATEXIT_MAX])(void);
static int nhandlers;

int atexit(void (*fn)(void))
{
    __crt_lock(CRT_LOCK_EXIT);
    int r = -1;
    if (fn && nhandlers < ATEXIT_MAX) {
        handlers[nhandlers++] = fn;
        r = 0;
    }
    __crt_unlock(CRT_LOCK_EXIT);
    return r;
}

void exit(int code)
{
    while (nhandlers > 0)                           /* in reverse order */
        handlers[--nhandlers]();
    fflush(NULL);
    ExitProcess(code);
    for (;;)
        ;
}

void _Exit(int code)
{
    ExitProcess(code);
    for (;;)
        ;
}

void abort(void)
{
    __crt_fail(FAIL_ABORT, NULL);
}

void __assert_fail(const char *expr, const char *file, int line, const char *func)
{
    char msg[120];
    snprintf(msg, sizeof msg, "%s, %s line %d: %s", func ? func : "?", file, line, expr);
    __crt_fail(FAIL_ASSERT, msg);
}

/* crt0 calls this with main and the startup info: argv[0] is the program,
 * the rest its arguments, split at spaces ("quotes" keep spaces in one). */
static char args[ARGS_MAX];
static char *argv[ARGV_MAX + 1];

int __crt_start(int (*main_fn)(int, char **), const struct user_info *info)
{
    strlcpy(args, info->args, sizeof args);
    int argc = 0;
    argv[argc++] = (char *)info->name;
    for (char *p = args; *p && argc < ARGV_MAX; ) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        char *out = p;
        argv[argc++] = out;
        bool quoted = false;
        for (; *p && (quoted || (*p != ' ' && *p != '\t')); p++) {
            if (*p == '"')
                quoted = !quoted;
            else
                *out++ = *p;
        }
        if (*p)
            p++;
        *out = '\0';
    }
    argv[argc] = NULL;
    exit(main_fn(argc, argv));
}
