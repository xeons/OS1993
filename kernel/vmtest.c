/*
 * vmtest.c -- boot-time checks of multiple VMs: preemptive scheduling,
 * private address spaces, virtualized video and keyboard focus.
 */
#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "v86.h"
#include "vm.h"

#define CHECK(cond, ...) do { if (!(cond)) panic(__VA_ARGS__); } while (0)

#define MARK_OFF    200             /* in the V86 buffer: per-VM marker word */
#define BUSY_TICKS  10

/* Real-mode routine: put a letter at the top-left of the screen, then spin
 * (no HLT) until the BIOS tick count has changed BUSY_TICKS times. Two of
 * these can only overlap in time if the scheduler preempts. */
static const uint8_t busy_code[] = {
    0x1E,                               /*     push ds                  */
    0x31, 0xC0,                         /*     xor  ax, ax              */
    0x8E, 0xD8,                         /*     mov  ds, ax              */
    0xB8, 0x00, 0xB8,                   /*     mov  ax, 0B800h          */
    0x8E, 0xC0,                         /*     mov  es, ax              */
    0x26, 0xC6, 0x06, 0x00, 0x00, 0x41, /*     mov  byte [es:0], 'A'    */
    0xFB,                               /*     sti                      */
    0x8B, 0x1E, 0x6C, 0x04,             /*     mov  bx, [046Ch]         */
    0xB9, BUSY_TICKS, 0x00,             /*     mov  cx, BUSY_TICKS      */
    0x3B, 0x1E, 0x6C, 0x04,             /* w:  cmp  bx, [046Ch]         */
    0x74, 0xFA,                         /*     je   w                   */
    0x8B, 0x1E, 0x6C, 0x04,             /*     mov  bx, [046Ch]         */
    0xE2, 0xF4,                         /*     loop w                   */
    0x1F,                               /*     pop  ds                  */
    0xCB,                               /*     retf                     */
};
#define BUSY_LETTER_OFF 15

/* Real-mode routine: make the 8042 emit the scancode for 'A' (command D2h),
 * then wait for a key with INT 16h. Returns AX = 1E61h only if the key went
 * IRQ 1 -> kernel -> focused VM's virtual keyboard -> its BIOS INT 9. */
static const uint8_t inject_key_code[] = {
    0xE4, 0x64,             /* w1:    in   al, 64h         */
    0xA8, 0x02,             /*        test al, 2           */
    0x75, 0xFA,             /*        jnz  w1              */
    0xB0, 0xD2,             /*        mov  al, 0D2h        */
    0xE6, 0x64,             /*        out  64h, al         */
    0xB0, 0x1E,             /*        mov  al, 1Eh  ('A')  */
    0xE6, 0x60,             /*        out  60h, al         */
    0xFB,                   /*        sti                  */
    0x30, 0xE4,             /*        xor  ah, ah          */
    0xCD, 0x16,             /*        int  16h             */
    0xCB,                   /*        retf                 */
};

static void busy_body(struct vm *vm)
{
    uint16_t seg = v86_buf_seg(), buf = v86_buf_off();
    uint8_t *code = v86_ptr(seg, buf);              /* this VM's own memory */
    memcpy(code, busy_code, sizeof busy_code);
    code[BUSY_LETTER_OFF] = (uint8_t)('A' + vm->id);
    *(uint16_t *)v86_ptr(seg, buf + MARK_OFF) = (uint16_t)(0x5A00 + vm->id);

    struct v86_regs r = { 0 };
    vm->result = v86_call_far(seg, buf, &r);
}

static void key_body(struct vm *vm)
{
    uint16_t seg = v86_buf_seg(), buf = v86_buf_off();
    memcpy(v86_ptr(seg, buf), inject_key_code, sizeof inject_key_code);
    struct v86_regs r = { 0 };
    vm->result = v86_call_far(seg, buf, &r) == 0 ? (int)(r.eax & 0xFFFF) : -1;
}

static void wait_finished(struct vm *vm)
{
    while (vm->state != VM_FINISHED)
        sched_wait();
}

void vm_selftest(void)
{
    uint32_t mark_lin = ((uint32_t)v86_buf_seg() << 4) + v86_buf_off() + MARK_OFF;

    /* 1. Two busy VMs at once */
    struct vm *a = vm_create("Busy A", busy_body, NULL);
    struct vm *b = vm_create("Busy B", busy_body, NULL);
    CHECK(a && b, "Cannot create the test VMs.");
    vm_start(a);
    vm_start(b);
    wait_finished(a);
    wait_finished(b);
    CHECK(a->result == 0 && b->result == 0, "A busy VM crashed: %s %s", a->error, b->error);
    CHECK(a->start_tick < b->end_tick && b->start_tick < a->end_tick,
          "VMs did not overlap in time (A %u-%u, B %u-%u): no preemption.",
          a->start_tick, a->end_tick, b->start_tick, b->end_tick);

    /* Same linear address, different memory */
    uint16_t ma = *(uint16_t *)vm_ptr(a, mark_lin);
    uint16_t mb = *(uint16_t *)vm_ptr(b, mark_lin);
    uint16_t ms = *(uint16_t *)vm_ptr(system_vm, mark_lin);
    CHECK(ma == 0x5A00 + a->id && mb == 0x5A00 + b->id && ms != ma && ms != mb,
          "VM memory is not private (%04X %04X %04X).", ma, mb, ms);

    /* Background VMs drew into their own video buffers, not the screen */
    uint8_t va = *(uint8_t *)vm_ptr(a, 0xB8000);
    uint8_t vb = *(uint8_t *)vm_ptr(b, 0xB8000);
    uint8_t real = *(uint8_t *)KLOW(0xB8000);
    CHECK(va == 'A' + a->id && vb == 'A' + b->id && real != va && real != vb,
          "Video is not virtualized (%02X %02X, screen %02X).", va, vb, real);
    uint32_t a_ticks = a->end_tick - a->start_tick, b_ticks = b->end_tick - b->start_tick;
    vm_reap();

    /* 2. Keyboard focus: a key typed goes to the focused VM only */
    struct vm *k = vm_create("Keyboard", key_body, NULL);
    CHECK(k, "Cannot create the keyboard test VM.");
    vm_set_focus(k);
    vm_start(k);
    wait_finished(k);
    CHECK(vm_focus() == NULL, "Focus did not return to the console.");
    CHECK(k->result == 0x1E61, "Focused VM read %04X from INT 16h instead of 1E61.", k->result);
    vm_reap();

    kprintf("  Two VMs ran concurrently (%u and %u ticks), memory and video private;\n",
            a_ticks, b_ticks);
    kprintf("  keyboard input reached only the focused VM.\n");
    klog("VMM: multi-VM self-test passed\n");
}
