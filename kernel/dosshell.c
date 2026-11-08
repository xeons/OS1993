/*
 * dosshell.c -- run COMMAND.COM inside the DOS VM.
 *
 * The kernel EXECs %COMSPEC% through INT 21h AX=4B00h from V86 mode. While
 * the shell runs, the keyboard IRQ belongs to the VM (reflected to the BIOS
 * INT 9 handler) and the screen is the shell's; EXIT returns to the kernel.
 */
#include "kernel.h"
#include "console.h"
#include "devices.h"
#include "v86.h"

#define TEST_FILE "V86TEST.TXT"

static bool find_comspec(char *out, size_t size)
{
    uint16_t psp = v86_buf_seg();
    uint16_t env = *(uint16_t *)v86_ptr(psp, 0x2C);
    const char *e = v86_ptr(env, 0);

    for (unsigned i = 0; i < 32768 && e[i];) {
        const char *s = e + i;
        size_t len = strlen(s);
        if (len > 8 && memcmp(s, "COMSPEC=", 8) == 0 && len - 8 < size) {
            memcpy(out, s + 8, len - 8 + 1);
            return true;
        }
        i += (unsigned)len + 1;
    }
    return false;
}

/* EXEC %COMSPEC% with the given command tail (e.g. "/C DIR"). Returns 0 and
 * the shell's exit code, a DOS error code (>0), or -1 if the VM crashed. */
static int exec_shell(const char *args, uint8_t *exit_code)
{
    uint16_t seg = v86_buf_seg(), buf = v86_buf_off();
    char *path = v86_ptr(seg, buf);                 /* buf+0:   program path */
    uint8_t *tail = v86_ptr(seg, buf + 128);        /* buf+128: command tail */
    uint16_t *pb = v86_ptr(seg, buf + 256);         /* buf+256: EXEC block   */

    if (!find_comspec(path, 128))
        memcpy(path, "COMMAND.COM", 12);

    size_t n = strlen(args);
    if (n > 125)
        n = 125;
    if (n) {                                        /* " args\r", length n+1 */
        tail[0] = (uint8_t)(n + 1);
        tail[1] = ' ';
        memcpy(tail + 2, args, n);
        tail[n + 2] = '\r';
    } else {
        tail[0] = 0;
        tail[1] = '\r';
    }

    pb[0] = 0;                                      /* inherit environment */
    pb[1] = (uint16_t)(buf + 128); pb[2] = seg;     /* command tail */
    pb[3] = 0x5C; pb[4] = seg;                      /* FCB 1 (our PSP's) */
    pb[5] = 0x6C; pb[6] = seg;                      /* FCB 2 */

    struct v86_regs r = { .eax = 0x4B00, .edx = buf, .ds = seg,
                          .ebx = (uint16_t)(buf + 256), .es = seg };
    if (v86_int(0x21, &r) != 0)
        return -1;
    if (r.flags & EFLAGS_CF)
        return (int)(r.eax & 0xFFFF);

    r = (struct v86_regs){ .eax = 0x4D00 };         /* child's return code */
    if (v86_int(0x21, &r) != 0)
        return -1;
    *exit_code = (uint8_t)r.eax;
    return 0;
}

/* Interactive DOS prompt in the VM. */
void dos_shell(void)
{
    struct v86_regs r = { .eax = 0x0003 };          /* BIOS: reset text mode */
    v86_int(0x10, &r);
    con_show_cursor(1);

    keyboard_release();
    uint8_t code = 0;
    int rc = exec_shell("", &code);
    keyboard_claim();

    con_show_cursor(0);
    if (rc < 0)
        klog("VMM: DOS shell VM stopped: %s\n", v86_last_error());
    else if (rc > 0)
        klog("VMM: EXEC of COMMAND.COM failed, DOS error %d\n", rc);
    else
        klog("VMM: DOS shell exited (code %u)\n", code);
}

/* Automated check: have COMMAND.COM write a file, then read it back. */
void dos_shell_selftest(void)
{
    uint16_t seg = v86_buf_seg(), buf = v86_buf_off();
    uint8_t code = 0;

    keyboard_release();
    int rc = exec_shell("/C ECHO V86 OK>" TEST_FILE, &code);
    keyboard_claim();
    if (rc < 0)
        panic("COMMAND.COM crashed in the VM: %s", v86_last_error());
    if (rc > 0)
        panic("EXEC of COMMAND.COM failed, DOS error %d.", rc);

    memcpy(v86_ptr(seg, buf), TEST_FILE, sizeof TEST_FILE);
    struct v86_regs r = { .eax = 0x3D00, .edx = buf, .ds = seg };
    if (v86_int(0x21, &r) != 0 || (r.flags & EFLAGS_CF))
        panic("The shell in the VM did not create " TEST_FILE ".");
    uint32_t handle = r.eax & 0xFFFF;

    r = (struct v86_regs){ .eax = 0x3F00, .ebx = handle, .ecx = 16,
                           .edx = (uint16_t)(buf + 32), .ds = seg };
    v86_int(0x21, &r);
    bool good = !(r.flags & EFLAGS_CF) && (r.eax & 0xFFFF) >= 6 &&
                memcmp(v86_ptr(seg, buf + 32), "V86 OK", 6) == 0;

    r = (struct v86_regs){ .eax = 0x3E00, .ebx = handle };
    v86_int(0x21, &r);
    r = (struct v86_regs){ .eax = 0x4100, .edx = buf, .ds = seg };   /* delete */
    v86_int(0x21, &r);

    if (!good)
        panic(TEST_FILE " written by the shell has the wrong contents.");
    kprintf("  COMMAND.COM ran in the VM and wrote " TEST_FILE ".\n");
    klog("VMM: DOS shell test passed\n");
}
