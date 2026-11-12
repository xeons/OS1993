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

static uint8_t last_term_type;

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
    last_term_type = (uint8_t)(r.eax >> 8);         /* 0 normal, 1 ^C, 2 crit. error, 3 TSR */
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
        klog("VMM: DOS session in VM %d ended (exit code %u, termination type %u, %u keys, idled %u times)\n",
             vm->id, code, last_term_type, vm->keys, vm->idled);
    if (vm->result != 0 || code != 0)
        dosmgr_dump_trace(vm);              /* what DOS saw before it went wrong */
}

static void shell_test_body(struct vm *vm)
{
    uint8_t code = 0;
    vm->result = dos_exec_shell("/C ECHO V86 OK>" TEST_FILE, &code);
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

/* ---- concurrent DOS use ------------------------------------------------- */

#define CONC_LINES 60

/* Small DOS file helpers for the System VM; data goes through the V86
 * transfer buffer. */
static bool dos_write_file(const char *name, const char *data, uint16_t len)
{
    uint16_t seg = v86_buf_seg(), buf = v86_buf_off();
    memcpy(v86_ptr(seg, buf), name, strlen(name) + 1);
    memcpy(v86_ptr(seg, buf + 64), data, len);
    struct v86_regs r = { .eax = 0x3C00, .ecx = 0, .edx = buf, .ds = seg };
    if (v86_int(0x21, &r) != 0 || (r.flags & EFLAGS_CF))
        return false;
    uint32_t h = r.eax & 0xFFFF;
    r = (struct v86_regs){ .eax = 0x4000, .ebx = h, .ecx = len, .edx = (uint16_t)(buf + 64), .ds = seg };
    bool ok = v86_int(0x21, &r) == 0 && !(r.flags & EFLAGS_CF) && (r.eax & 0xFFFF) == len;
    r = (struct v86_regs){ .eax = 0x3E00, .ebx = h };
    v86_int(0x21, &r);
    return ok;
}

static int dos_read_file(const char *name, char *out, uint16_t max)
{
    uint16_t seg = v86_buf_seg(), buf = v86_buf_off();
    memcpy(v86_ptr(seg, buf), name, strlen(name) + 1);
    struct v86_regs r = { .eax = 0x3D00, .edx = buf, .ds = seg };
    if (v86_int(0x21, &r) != 0 || (r.flags & EFLAGS_CF))
        return -1;
    uint32_t h = r.eax & 0xFFFF;
    r = (struct v86_regs){ .eax = 0x3F00, .ebx = h, .ecx = max, .edx = (uint16_t)(buf + 64), .ds = seg };
    int n = (v86_int(0x21, &r) == 0 && !(r.flags & EFLAGS_CF)) ? (int)(r.eax & 0xFFFF) : -1;
    if (n > 0)
        memcpy(out, v86_ptr(seg, buf + 64), (size_t)n);
    r = (struct v86_regs){ .eax = 0x3E00, .ebx = h };
    v86_int(0x21, &r);
    return n;
}

static void dos_delete(const char *name)
{
    uint16_t seg = v86_buf_seg(), buf = v86_buf_off();
    memcpy(v86_ptr(seg, buf), name, strlen(name) + 1);
    struct v86_regs r = { .eax = 0x4100, .edx = buf, .ds = seg };
    v86_int(0x21, &r);
}

static void batch_body(struct vm *vm)
{
    uint8_t code = 0;
    vm->result = dos_exec_shell((const char *)vm->arg, &code);
}

/* Two VMs run batch files at the same time, each appending CONC_LINES lines
 * to its own file through the one shared DOS. Both files must come out
 * exactly right. */
void dos_concurrency_selftest(void)
{
    static char text[CONC_LINES * 24], expect[CONC_LINES * 8], got[CONC_LINES * 8 + 16];
    static const char tag[2] = { 'A', 'B' };
    static const char *const bat[2] = { "CONCA.BAT", "CONCB.BAT" };
    static const char *const out[2] = { "CONCA.TXT", "CONCB.TXT" };
    static const char *const cmd[2] = { "/C CONCA.BAT", "/C CONCB.BAT" };

    for (int v = 0; v < 2; v++) {
        int n = ksnprintf(text, sizeof text, "@ECHO OFF\r\n");
        for (int i = 1; i <= CONC_LINES; i++)
            n += ksnprintf(text + n, sizeof text - (size_t)n, "ECHO %c%02d>>%s\r\n", tag[v], i, out[v]);
        dos_delete(out[v]);
        if (!dos_write_file(bat[v], text, (uint16_t)n))
            panic("Cannot write %s.", bat[v]);
    }

    uint32_t waits0 = dosmgr_waits();
    struct vm *vm[2];
    for (int v = 0; v < 2; v++) {
        vm[v] = vm_create(bat[v], batch_body, (void *)cmd[v]);
        if (!vm[v])
            panic("Cannot create a VM for the concurrency test.");
    }
    vm_start(vm[0]);
    vm_start(vm[1]);
    while (vm[0]->state != VM_FINISHED || vm[1]->state != VM_FINISHED)
        sched_wait();
    int rc0 = vm[0]->result, rc1 = vm[1]->result;
    bool overlap = vm[0]->start_tick < vm[1]->end_tick && vm[1]->start_tick < vm[0]->end_tick;
    uint32_t waits = dosmgr_waits() - waits0;
    vm_reap();
    if (rc0 != 0 || rc1 != 0)
        panic("A batch VM failed (%d, %d).", rc0, rc1);

    for (int v = 0; v < 2; v++) {
        int n = 0;
        for (int i = 1; i <= CONC_LINES; i++)
            n += ksnprintf(expect + n, sizeof expect - (size_t)n, "%c%02d\r\n", tag[v], i);
        int len = dos_read_file(out[v], got, sizeof got);
        if (len != n || memcmp(got, expect, (size_t)n) != 0)
            panic("%s is wrong after concurrent DOS use (%d bytes, expected %d).", out[v], len, n);
        dos_delete(out[v]);
        dos_delete(bat[v]);
    }

    kprintf("  Two VMs ran batch files through the shared DOS: %d lines each, intact;\n", CONC_LINES);
    kprintf("  %s in time, %u waits for the DOS critical section.\n",
            overlap ? "overlapping" : "not overlapping", waits);
    klog("VMM: DOS concurrency test passed (overlap %d, waits %u)\n", overlap, waits);
}
