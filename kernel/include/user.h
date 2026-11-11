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
#define USER_HEAP       0x20000000u     /* the heap (SYS_SBRK) grows up from here ... */
#define USER_HEAP_LIMIT 0x60000000u     /* ... to here */
#define USER_SHARED     0x60000000u     /* shared region (page tables of their own): */
#define USER_VGA        0x60000000u     /*   A000:0000-AFFF, 64 KB */
#define USER_ARENA      0x60010000u     /*   the shared arena */
#define USER_ARENA_SIZE 0x00080000u     /*   512 KB */
#define USER_BACK       0x60400000u     /*   the off-screen copy of the screen (up to 8 MB) */
#define USER_BACK_MAX   0x00800000u
#define USER_FB         0x60C00000u     /*   a linear framebuffer (up to 16 MB) */
#define USER_FB_MAX     0x01000000u
#define USER_SHARED_END 0x61C00000u
#define USER_VM         0x62000000u     /* VirtualAlloc, thread stacks and TIBs: from here ... */
#define USER_VM_LIMIT   0x7F000000u     /* ... to here, in 64 KB-aligned regions */
#define VM_GRANULE      0x00010000u
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
    uint32_t data;                      /* writable from here (page aligned): each process
                                           gets its own copy; the pages before are shared */
    uint32_t guard;                     /* -> the module's stack canary (__stack_chk_guard),
                                           or 0: the loader sets a random one per process */
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
#define UINFO_TEST_CODE     0x08        /* self-test: write its own (shared) code (#PF) */
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
#define EV_CAD      4                   /* Ctrl+Alt+Del: show the task list (SYS_TASKLIST_ACK) */
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
#define SYS_SBRK        20              /* EBX = bytes to add -> the old end of the heap, or 0 */
#define SYS_TASKLIST_ACK 21             /* the shell shows its task list for EV_CAD */
#define SYS_PROC_LIST   22              /* EBX = struct proc_entry *, ECX = most -> count */
#define SYS_DISPLAY_INFO 23             /* EBX = struct display_info * */
#define SYS_DISPLAY_MODES 24            /* EBX = struct display_mode *, ECX = most -> count */
#define SYS_DISPLAY_SET 25              /* EBX = width, ECX = height, EDX = bits -> 0, or -1 */
#define SYS_RANDOM      26              /* EBX = buffer, ECX = bytes (at most 256) -> 0, or -1 */
#define SYS_FAIL        27              /* EBX = FAIL_*, ECX = message or 0: the process
                                           detected its own corruption and ends at once */

/* SYS_FAIL reasons (as Win32's __fastfail codes); the exit code is FAIL_EXIT */
#define FAIL_STACK_OVERRUN  1           /* a stack canary was overwritten */
#define FAIL_HEAP_CORRUPT   2           /* the heap's checks failed (overrun, double free) */
#define FAIL_BUFFER_OVERRUN 3           /* a _FORTIFY_SOURCE check failed */
#define FAIL_CONSTRAINT     4           /* a bounds-checked (_s) function's constraint */
#define FAIL_ABORT          5           /* abort() */
#define FAIL_ASSERT         6           /* assert() */
#define FAIL_EXIT           (-0x409)    /* cf. STATUS_STACK_BUFFER_OVERRUN */

#define SYS_THREAD_CREATE 28            /* EBX = struct thread_args * -> tid, or -error */
#define SYS_THREAD_EXIT 29              /* EBX = exit code (the last thread: the process's) */
#define SYS_THREAD_WAIT 30              /* EBX = tid, ECX = ms, EDX = int *code (or 0)
                                           -> 0 it has ended, 1 timed out, -1 no such thread */
#define SYS_THREAD_RESUME 31            /* EBX = tid: start one created THREAD_SUSPENDED */
#define SYS_THREAD_KILL 32              /* EBX = tid, ECX = its exit code */
#define SYS_FUTEX_WAIT  33              /* EBX = address, ECX = expected value, EDX = ms
                                           -> 0 woken, 1 the value differed, 2 timed out */
#define SYS_FUTEX_WAKE  34              /* EBX = address, ECX = how many -> how many woke */
#define SYS_VMEM        35              /* EBX = VM_*, ECX = struct vmem_args * -> 0, or -error */
#define SYS_PROC_WAIT   36              /* EBX = pid, ECX = ms, EDX = int *code (or 0)
                                           -> 0 it has ended, 1 timed out, -1 unknown pid */
#define SYS_MEMINFO     37              /* EBX = struct mem_info * */


/* Each thread's information block, at FS:0 (as Win32's TEB): its own
 * errno, last error, thread-local storage. The kernel fills it in. */
#define TLS_SLOTS       64
#define TIB_MAIN        0x01            /* the process's first thread */
struct user_tib {
    uint32_t self;                      /* its own address */
    uint32_t tid, pid;
    uint32_t flags;
    int      err;                       /* the C runtime's errno */
    uint32_t last_error;                /* GetLastError */
    uint32_t stack_base, stack_limit;   /* the stack: [limit, base) */
    volatile uint32_t sleep_word;       /* Sleep waits on it */
    uint32_t reserved[7];
    uint32_t tls[TLS_SLOTS];            /* TlsGetValue / TlsSetValue */
};

#define THREADS_PER_PROC 16
#define THREAD_STACK_DEFAULT 0x10000u   /* 64 KB, and a guard page below */
#define THREAD_STACK_MAX     0x100000u
#define THREAD_SUSPENDED 0x04           /* created stopped (SYS_THREAD_RESUME) */
struct thread_args {
    uint32_t start;                     /* entered with [ESP] = fn, [ESP+4] = arg */
    uint32_t fn, arg;
    uint32_t stack_size;                /* 0: THREAD_STACK_DEFAULT */
    uint32_t flags;
};

/* SYS_VMEM: regions of USER_VM, reserved, then committed page by page */
#define VM_RESERVE      1               /* addr (0: anywhere), size -> base */
#define VM_COMMIT       2               /* addr, size, prot (reserves too if addr is 0) -> base */
#define VM_DECOMMIT     3               /* addr, size */
#define VM_RELEASE      4               /* addr: the region's base */
#define VM_PROTECT      5               /* addr, size, prot -> old prot */
#define VM_QUERY        6               /* addr -> base, size, state, prot */
#define VM_PROT_READ    1
#define VM_PROT_WRITE   2
#define VM_STATE_FREE    0x10000        /* as Win32's MEM_FREE, MEM_RESERVE, MEM_COMMIT */
#define VM_STATE_RESERVE 0x2000
#define VM_STATE_COMMIT  0x1000
struct vmem_args {
    uint32_t addr, size, prot;          /* in */
    uint32_t base, region_size;         /* out: the region (or the page run, VM_QUERY) */
    uint32_t state, old_prot;           /* out */
};

struct mem_info {
    uint32_t total_pages, free_pages;   /* physical memory */
    uint32_t process_pages;             /* the caller's private pages */
};
/* ---- sockets (SYS_SOCKET; WSOCK.DLL wraps them as Winsock) ---------------- */
#define SYS_SOCKET      38              /* EBX = SOP_*, ECX = struct sock_args * -> result, or -errno */
#define SYS_TIMEZONE    39              /* EBX = 1 set / 0 get, ECX = struct fs_timezone * -> 0 unknown, 1 standard, 2 daylight */
struct fs_tzrule { uint8_t month, week, day_of_week, hour, minute; };   /* month 0: none; week 5: the last */
struct fs_timezone {                    /* UTC = local + bias + (std_bias or dst_bias) */
    int32_t bias, std_bias, dst_bias;
    struct fs_tzrule std, dst;          /* when standard time starts again; when daylight time starts */
};

enum { SOP_SOCKET, SOP_BIND, SOP_CONNECT, SOP_LISTEN, SOP_ACCEPT, SOP_SEND, SOP_RECV,
       SOP_SHUTDOWN, SOP_CLOSE, SOP_SETOPT, SOP_GETOPT, SOP_NAME, SOP_POLL, SOP_CONFIG };

struct sock_args {
    int32_t  s;                         /* the socket */
    int32_t  a, b, c;                   /* SOCKET: type, protocol; LISTEN: backlog;
                                           SEND/RECV: flags; SHUTDOWN: how; SETOPT/GETOPT:
                                           level, name, value; NAME: 0 ours, 1 the peer's;
                                           POLL: count, timeout ms (-1 none) */
    void    *buf;                       /* SEND/RECV: data; POLL: struct sock_poll[]; CONFIG */
    uint32_t len;
    uint32_t ip;                        /* BIND/CONNECT/SEND to; ACCEPT/RECV/NAME from: */
    uint16_t port, has_addr;            /*   host byte order; has_addr: SEND to ip:port */
};

struct sock_poll { int32_t s; int16_t events, revents; };

struct net_config {                     /* SOP_CONFIG */
    uint8_t  mac[6];
    uint8_t  up;                        /* a NIC was found */
    uint8_t  dhcp;                      /* 0 none, 1 asking, 2 leased */
    uint32_t ip, mask, gateway, dns;    /* host order */
    uint32_t dhcp_server, lease_secs;
    uint32_t rx_packets, tx_packets, rx_dropped, tx_dropped;
}; 

#define NET_SOCK_STREAM 1               /* Winsock's numbers */
#define NET_SOCK_DGRAM  2
#define NET_SOCK_RAW    3
#define NET_POLLIN      0x0001
#define NET_POLLOUT     0x0004
#define NET_POLLERR     0x0008
#define NET_POLLHUP     0x0010
#define NET_MSG_PEEK    0x2


/* The display (SYS_DISPLAY_*): VGA's 640x480 in 16 colours (4 bits,
 * planar), or a linear mode in 8 (palette), 16 (5:6:5) or 32 (8:8:8) bits. */
struct display_mode { uint16_t width, height; uint8_t bpp, pad[3]; };
struct display_info {
    uint16_t width, height;
    uint8_t  bpp;                       /* 4: planar VGA */
    uint8_t  red_pos, red_size, green_pos, green_size, blue_pos, blue_size, pad;
    uint32_t pitch;                     /* bytes per row on screen (planar: 80) */
    uint32_t fb;                        /* the screen, in user space */
    uint32_t back;                      /* the off-screen copy (same format, rows of back_pitch) */
    uint32_t back_pitch;
    uint32_t gen;                       /* counts mode changes */
};

struct proc_entry {                     /* a running process (SYS_PROC_LIST) */
    uint32_t pid;
    uint32_t flags;                     /* PROC_* */
    char     name[16];
};
#define PROC_LEADER     0x01            /* the session's first process (the shell) */
#define PROC_UI_LOCK    0x02            /* it holds lock 0 (the window manager's) */
#define WAIT_INFINITE   0xFFFFFFFFu
#define WAIT_FOREVER    WAIT_INFINITE   /* the ms of SYS_FUTEX_WAIT, SYS_THREAD_WAIT ... */
#define WAIT_INPUT      1               /* also end the wait when input is pending */
#define LOCK_MAX        8

/* File access (SYS_FILE). Files are DOS's, reached through the System VM;
 * values match Win32 where Win32 has them. */
enum { FOP_OPEN, FOP_READ, FOP_WRITE, FOP_CLOSE, FOP_SEEK, FOP_SIZE, FOP_DELETE,
       FOP_RENAME, FOP_MKDIR, FOP_RMDIR, FOP_ATTRIB, FOP_GETCWD, FOP_SETCWD,
       FOP_FIND_FIRST, FOP_FIND_NEXT, FOP_FIND_CLOSE,
       FOP_TRUNCATE,                    /* handle: the file ends at its pointer */
       FOP_FLUSH,                       /* handle: to the disk */
       FOP_SETATTR,                     /* path, a = FILE_ATTRIBUTE_* */
       FOP_FULLPATH,                    /* path -> buf (len): the full path; -> its length */
       FOP_DISKFREE,                    /* path (a drive, or 0: the current one) -> buf: struct disk_free */
       FOP_GETTIME,                     /* handle -> buf (uint32_t): DOS time << 16 | DOS date */
       FOP_SETTIME,                     /* handle, a: DOS time << 16 | DOS date */
       FOP_LOCK, FOP_UNLOCK,            /* handle, a|c<<32: offset, b|d<<32: length (LockFile/UnlockFile) */
       FOP_SEEK64,                      /* handle, a|c<<32: offset, b: method -> buf (uint64_t): the position */
       FOP_SIZE64,                      /* handle -> buf (uint64_t) */
       FOP_LINK,                        /* path2: a new name for the file at path (a hard link) */
       FOP_SYMLINK };                   /* path: a symbolic link, path2: what it points to (as given) */

struct disk_free {
    uint32_t sectors_per_cluster, bytes_per_sector, free_clusters, total_clusters;
};

struct file_args {
    const char *path, *path2;           /* OPEN, DELETE, RENAME, ..., FIND_FIRST */
    uint32_t    handle;                 /* READ, WRITE, CLOSE, SEEK, SIZE, FIND_* */
    void       *buf;                    /* READ, WRITE, GETCWD, FIND_* (struct find_data) */
    uint32_t    len;
    uint32_t    a, b;                   /* OPEN: access, creation; SEEK: offset, method */
    uint32_t    c, d;                   /* SEEK64: offset high; LOCK: offset high, length high */
};

struct find_data {                      /* a directory entry (FIND_FIRST/NEXT) */
    uint32_t attrib;                    /* FILE_ATTRIBUTE_* (DOS attributes) */
    uint32_t size;
    uint16_t time, date;                /* DOS format */
    char     name[260];                 /* the long name (on DOS's drives, the 8.3 one) */
    char     short_name[14];            /* the 8.3 alias, "" if the name is one */
    uint32_t size_high;                 /* files of 4 GB and more (exFAT, ext2) */
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
#define ERROR_CURRENT_DIRECTORY   16
#define ERROR_NOT_SAME_DEVICE     17
#define ERROR_WRITE_PROTECT       19
#define ERROR_GEN_FAILURE         31
#define ERROR_SHARING_VIOLATION   32
#define ERROR_LOCK_VIOLATION      33
#define ERROR_SHARING_BUFFER_EXCEEDED 36
#define ERROR_NOT_SUPPORTED       50
#define ERROR_NEGATIVE_SEEK       131
#define ERROR_FILE_TOO_LARGE      223
#define ERROR_TOO_MANY_LINKS      1142
#define ERROR_FILE_CORRUPT        1392
#define ERROR_ALREADY_EXISTS      183
#define ERROR_HANDLE_DISK_FULL    39
#define ERROR_CANNOT_MAKE         82
#define ERROR_DISK_FULL           112
#define ERROR_DIR_NOT_EMPTY       145
#define ERROR_FILENAME_EXCED_RANGE 206
#define MAX_PATH                 260
#define ERROR_NOACCESS           998     /* a bad user pointer */
#define ERROR_NOT_ENOUGH_MEMORY  8
#define ERROR_INVALID_ADDRESS    487
#define ERROR_TOO_MANY_THREADS   164     /* ERROR_MAX_THRDS_REACHED */

#ifndef USER_SIDE                       /* kernel only */
struct regs;
void syscall_dispatch(struct regs *r);
void user_fault(struct regs *r);

void user_return(struct regs *r);       /* about to return to ring 3 */
bool user_range_ok(uint32_t p, uint32_t len);       /* mapped user memory? */
bool user_writable(uint32_t p, uint32_t len);       /* ... that the kernel may write */
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
bool user_cad(void);                    /* keyboard IRQ: Ctrl+Alt+Del; false if no GUI */
/* After the session: the first process's exit code (>= 0) or
 * -(exception number + 1). */
int  user_result(void);
void user_selftest(void);
#endif

#endif
