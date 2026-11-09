/*
 * user.h -- ring-3 programs (the GUI) and the INT 30h system call interface.
 *
 * Shared with the ring-3 side (user/): the system call numbers, the image
 * header and the info block a program receives in EBX at entry.
 */
#ifndef OS1993_USER_H
#define OS1993_USER_H

#include <stdbool.h>
#include <stdint.h>

/* Ring-3 address space (global for now: there is one ring-3 program) */
#define USER_BASE       0x00400000u     /* image is linked here */
#define USER_LIMIT      0x80000000u     /* the kernel starts here */
#define USER_STACK_TOP  0x7FFF0000u
#define USER_STACK_SIZE 0x00010000u
#define USER_VGA        0x7FF00000u     /* A000:0000-AFFF, 64 KB */
#define USER_INFO       0x7FFF0000u     /* struct user_info + the font */

#define UIMG_MAGIC      0x33495547u     /* "GUI3" */

struct user_image_header {              /* first bytes of GUI.BIN */
    uint32_t magic;
    uint32_t entry;
    uint32_t file_size;                 /* bytes in the file */
    uint32_t mem_size;                  /* including .bss */
};

#define UINFO_TEST_CLI      0x01        /* self-test: execute CLI (expect #GP) */
#define UINFO_TEST_KERNEL   0x02        /* self-test: write kernel memory (#PF) */
#define UINFO_GRAPHICS      0x04        /* the screen is in 640x480x16 for us */

struct user_info {                      /* at USER_INFO, pointed to by EBX */
    uint32_t flags;
    uint32_t vga;                       /* user address of video memory */
    uint32_t font;                      /* user address of the 8x16 font */
    uint32_t width, height;
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

#ifndef USER_SIDE                       /* kernel only */
struct regs;
void syscall_dispatch(struct regs *r);
void user_fault(struct regs *r);

/* Load and start a ring-3 program from the directory VMM.BIN came from.
 * Returns 0, or -1 if it couldn't be loaded. */
int  user_start(const char *name, uint32_t flags);
bool user_running(void);
void user_tick(void);                   /* timer: wake a waiting program */
void user_screen(bool ours);            /* the GUI gained/lost the screen */
/* After it ended: exit code (>= 0) or -(exception number + 1). */
int  user_result(void);
void user_selftest(void);
#endif

#endif
