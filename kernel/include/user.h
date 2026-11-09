/*
 * user.h -- ring-3 processes and the INT 30h system call interface.
 *
 * Shared with the ring-3 side (user/): the system call numbers, the image
 * header and the info block a program receives in EBX at entry.
 */
#ifndef OS1993_USER_H
#define OS1993_USER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Each process has its own address space below 2 GB; the shared region is
 * the same memory, at the same address, in all of them (like Win95's shared
 * arena, where USER kept its data). */
#define USER_BASE       0x00400000u     /* a program's image is linked here */
#define USER_LIMIT      0x80000000u     /* the kernel starts here */
#define USER_STACK_TOP  0x7FFF0000u
#define USER_STACK_SIZE 0x00010000u
#define USER_INFO       0x7FFF0000u     /* struct user_info + the font */
#define USER_SHARED     0x60000000u     /* shared region (one page table): */
#define USER_VGA        0x60000000u     /*   A000:0000-AFFF, 64 KB */
#define USER_ARENA      0x60010000u     /*   the shared arena */
#define USER_ARENA_SIZE 0x00080000u     /*   512 KB */
#define PROC_MAX        8               /* processes at once */
#define ARGS_MAX        128

/*
 * Module format, for programs and DLLs alike. Each module is linked at a
 * fixed base address (programs at USER_BASE, each DLL at its own), so the
 * loader never relocates. Programs import functions by name; for each one
 * an import library provides a JMP [slot] thunk, and the loader fills the
 * slot with the DLL's export.
 */
#define MOD_MAGIC       0x33444F4Du     /* "MOD3" */
#define MOD_PROGRAM     1
#define MOD_LIBRARY     2

struct mod_header {                     /* first bytes of every module */
    uint32_t magic;
    uint32_t kind;
    uint32_t base;                      /* linked (and loaded) at */
    uint32_t entry;                     /* programs: entry point */
    uint32_t file_size;                 /* bytes in the file */
    uint32_t mem_size;                  /* including .bss */
    uint32_t exports;                   /* -> struct mod_exports, or 0 */
    uint32_t imports;                   /* -> struct mod_import[], 0-terminated */
    char     name[16];                  /* "UI.DLL" */
};

struct mod_export { uint32_t name; uint32_t addr; };    /* name -> ASCIIZ */
struct mod_exports {
    uint32_t count;
    struct mod_export e[];
};

struct mod_import {                     /* one per imported DLL */
    uint32_t module;                    /* -> "UI.DLL" (0 ends the list) */
    uint32_t names;                     /* -> name pointers, 0-terminated */
    uint32_t slots;                     /* -> addresses the loader fills in */
};

#define DLL_BASE        0x10000000u     /* DLLs live between here ... */
#define DLL_LIMIT       0x20000000u     /* ... and here */
#define MOD_MAX         8

#define UINFO_TEST_CLI      0x01        /* self-test: execute CLI (expect #GP) */
#define UINFO_TEST_KERNEL   0x02        /* self-test: write kernel memory (#PF) */
#define UINFO_GRAPHICS      0x04        /* the screen is in 640x480x16 for us */

struct user_info {                      /* at USER_INFO, pointed to by EBX */
    uint32_t flags;
    uint32_t vga;                       /* user address of video memory */
    uint32_t font;                      /* user address of the 8x16 font */
    uint32_t width, height;
    uint32_t pid;
    uint32_t arena, arena_size;         /* the shared arena */
    char     name[16];                  /* the program's file name */
    char     args[ARGS_MAX];            /* command line arguments */
};

/* Local time from the real-time clock (SYS_TIME); fields as in SYSTEMTIME */
struct systime {
    uint16_t year, month, day_of_week, day;
    uint16_t hour, minute, second, ms;
};

/* An input event (SYS_GET_EVENT) */
#define EV_KEY      1                   /* key: scancode */
#define EV_MOUSE    2                   /* x, y: pointer position; buttons */
#define EV_FOCUS    3                   /* key = 1: the screen is ours again (repaint),
                                           key = 0: a DOS session took it (stop drawing) */
#define MB_LEFT     0x01
#define MB_RIGHT    0x02
#define MB_MIDDLE   0x04

struct user_event {
    uint8_t  type;
    uint8_t  buttons;
    uint16_t key;
    int16_t  x, y;
};

/* System calls: INT 30h, EAX = number, args in EBX, ECX, EDX; result in EAX */
#define SYS_EXIT        0               /* EBX = exit code */
#define SYS_PRINT       1               /* EBX = string, ECX = length */
#define SYS_TICKS       2               /* -> timer ticks (18.2 Hz) */
#define SYS_YIELD       3
#define SYS_WAIT_TICK   4               /* sleep until the next timer tick */
#define SYS_GET_KEY     5               /* -> scancode, or -1 */
#define SYS_NEW_SESSION 6               /* start a DOS session -> VM id, or -1 */
#define SYS_GET_EVENT   7               /* EBX = struct user_event * -> 1, or 0 */
#define SYS_MOUSE       8               /* -> 1 if there is a mouse */
#define SYS_EXEC        9               /* EBX = program, ECX = args -> pid, or -1 */
#define SYS_WAIT        10              /* EBX = timeout ms (~0: none), ECX = WAIT_INPUT
                                           -> 1 woken, 0 timed out or input */
#define SYS_WAKE        11              /* EBX = pid: end (or pre-empt) its SYS_WAIT */
#define SYS_GETPID      12
#define SYS_PROC_ALIVE  13              /* EBX = pid -> 1 if it is running */
#define SYS_KILL        14              /* EBX = pid -> 0, or -1 */
#define SYS_LOCK        15              /* EBX = lock 0-7: wait for it, then own it */
#define SYS_UNLOCK      16              /* EBX = lock */
#define SYS_TIME        17              /* EBX = struct systime * */
#define SYS_PROC_EXITS  18              /* -> how many processes have ended so far */
#define SYS_FILE        19              /* EBX = FOP_*, ECX = struct file_args * -> result, or -error */
#define WAIT_INFINITE   0xFFFFFFFFu
#define WAIT_INPUT      1               /* also end the wait when input is pending */
#define LOCK_MAX        8

/* File access (SYS_FILE). Files are DOS's, reached through the System VM;
 * values match Win32 where Win32 has them. */
enum { FOP_OPEN, FOP_READ, FOP_WRITE, FOP_CLOSE, FOP_SEEK, FOP_SIZE, FOP_DELETE,
       FOP_RENAME, FOP_MKDIR, FOP_RMDIR, FOP_ATTRIB, FOP_GETCWD, FOP_SETCWD,
       FOP_FIND_FIRST, FOP_FIND_NEXT, FOP_FIND_CLOSE };

struct file_args {
    const char *path, *path2;           /* OPEN, DELETE, RENAME, ..., FIND_FIRST */
    uint32_t    handle;                 /* READ, WRITE, CLOSE, SEEK, SIZE, FIND_* */
    void       *buf;                    /* READ, WRITE, GETCWD, FIND_* (struct find_data) */
    uint32_t    len;
    uint32_t    a, b;                   /* OPEN: access, creation; SEEK: offset, method */
};

struct find_data {                      /* a directory entry (FIND_FIRST/NEXT) */
    uint32_t attrib;                    /* FILE_ATTRIBUTE_* (DOS attributes) */
    uint32_t size;
    uint16_t time, date;                /* DOS format */
    char     name[13];                  /* 8.3 */
};

#define GENERIC_READ        0x80000000u
#define GENERIC_WRITE       0x40000000u
#define CREATE_NEW          1
#define CREATE_ALWAYS       2
#define OPEN_EXISTING       3
#define OPEN_ALWAYS         4
#define TRUNCATE_EXISTING   5
#define FILE_BEGIN          0
#define FILE_CURRENT        1
#define FILE_END            2
#define FILE_ATTRIBUTE_READONLY  0x01
#define FILE_ATTRIBUTE_HIDDEN    0x02
#define FILE_ATTRIBUTE_SYSTEM    0x04
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE   0x20
#define ERROR_FILE_NOT_FOUND     2
#define ERROR_PATH_NOT_FOUND     3
#define ERROR_TOO_MANY_OPEN_FILES 4
#define ERROR_ACCESS_DENIED      5
#define ERROR_INVALID_HANDLE     6
#define ERROR_NO_MORE_FILES      18
#define ERROR_FILE_EXISTS        80
#define ERROR_INVALID_PARAMETER  87
#define ERROR_INVALID_NAME       123
#define ERROR_NOACCESS           998     /* a bad user pointer */

#ifndef USER_SIDE                       /* kernel only */
struct regs;
void syscall_dispatch(struct regs *r);
void user_fault(struct regs *r);

void user_return(struct regs *r);       /* about to return to ring 3 */
bool user_range_ok(uint32_t p, uint32_t len);       /* mapped user memory? */
bool user_string(uint32_t p, char *out, size_t size);  /* copy in a string */
struct process;
void proc_switch(struct process *p);    /* scheduler: install its address space */

/* Start a ring-3 session with its first process (the shell), loaded from
 * the directory VMM.BIN came from. Returns 0, or -1 if it couldn't be
 * loaded. The session lasts until that process ends; the others go too. */
int  user_start(const char *name, uint32_t flags);
bool user_running(void);
void user_service(void);                /* console thread: loads, reaping */
void user_tick(void);                   /* timer: wake waiting processes */
void user_screen(bool ours);            /* the GUI gained/lost the screen */
/* After the session: the first process's exit code (>= 0) or
 * -(exception number + 1). */
int  user_result(void);
void user_selftest(void);
#endif

#endif
