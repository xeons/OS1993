/*
 * dosshell.c -- run COMMAND.COM inside a VM.
 *
 * dos_exec_shell() EXECs %COMSPEC% through INT 21h AX=4B00h in the calling
 * thread's VM. A DOS session VM (key N in the VMM) is just a VM whose body
 * runs an interactive shell; EXIT ends the VM.
 */
#include "kernel.h"
#include "sched.h"
#include "v86.h"
#include "vm.h"

#define TEST_FILE "V86TEST.TXT"

static void dos_flush_buffers(void);

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
int dos_exec_shell(const char *args, uint8_t *exit_code)
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
    pb[3] = 0x5C; pb[4] = seg;                      /* FCB 1 (the PSP's) */
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

/* Body of a DOS session VM: a fresh text screen and an interactive shell. */
void dos_session_body(struct vm *vm)
{
    struct v86_regs r = { .eax = 0x0003 };          /* BIOS: set text mode */
    v86_int(0x10, &r);

    uint8_t code = 0;
    vm->result = dos_exec_shell("", &code);
    if (vm->result < 0)
        klog("VMM: DOS session in VM %d stopped: %s\n", vm->id, v86_last_error());
    else if (vm->result > 0)
        klog("VMM: EXEC of COMMAND.COM in VM %d failed, DOS error %d\n", vm->id, vm->result);
    else
        klog("VMM: DOS session in VM %d ended\n", vm->id);
    dos_flush_buffers();
}

/* Each VM has its own copy of DOS, including its disk buffers, so a VM
 * must flush them before another VM can see what it wrote (and the reader
 * must drop its stale ones). INT 21h AH=0Dh does both. A DOS critical
 * section shared by all VMs (as Win386's DOSMGR did) will replace this. */
static void dos_flush_buffers(void)
{
    struct v86_regs r = { .eax = 0x0D00 };
    v86_int(0x21, &r);
}

static void shell_test_body(struct vm *vm)
{
    uint8_t code = 0;
    vm->result = dos_exec_shell("/C ECHO V86 OK>" TEST_FILE, &code);
    dos_flush_buffers();
}

/* Automated check: COMMAND.COM in a new VM writes a file, and the System VM
 * reads it back through DOS. */
void dos_shell_selftest(void)
{
    struct vm *vm = vm_create("Shell test", shell_test_body, NULL);
    if (!vm)
        panic("Cannot create a VM for the COMMAND.COM test.");
    vm_start(vm);
    while (vm->state != VM_FINISHED)
        sched_wait();
    int rc = vm->result;
    char err[96];
    memcpy(err, vm->error, sizeof err);
    vm_reap();

    if (rc < 0)
        panic("COMMAND.COM crashed in its VM: %s", err);
    if (rc > 0)
        panic("EXEC of COMMAND.COM failed, DOS error %d.", rc);

    dos_flush_buffers();                            /* drop stale buffers */
    uint16_t seg = v86_buf_seg(), buf = v86_buf_off();
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
    kprintf("  COMMAND.COM ran in its own VM and wrote " TEST_FILE ".\n");
    klog("VMM: DOS shell test passed\n");
}
