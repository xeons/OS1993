/*
 * v86test.c -- boot-time check that the V86 monitor can run DOS and BIOS
 * code on the kernel's behalf. Any failure panics with a reason.
 */
#include "kernel.h"
#include "devices.h"
#include "io.h"
#include "v86.h"

#define CHECK(cond, ...) do { if (!(cond)) panic(__VA_ARGS__); } while (0)

static void call_int(uint8_t vec, struct v86_regs *r)
{
    if (v86_int(vec, r) != 0)
        panic("V86 INT %02Xh failed: %s", vec, v86_last_error());
}

/* Real-mode routine: STI, then HLT until the BIOS tick count at 0040:006C
 * has changed three times; RETF. Only finishes promptly if timer IRQs are
 * reflected into the VM and its EOIs reach the virtual PIC. */
static const uint8_t wait_ticks_code[] = {
    0x1E,                   /*        push ds              */
    0x31, 0xC0,             /*        xor  ax, ax          */
    0x8E, 0xD8,             /*        mov  ds, ax          */
    0xFB,                   /*        sti                  */
    0x8B, 0x1E, 0x6C, 0x04, /*        mov  bx, [046Ch]     */
    0xB9, 0x03, 0x00,       /*        mov  cx, 3           */
    0xF4,                   /* wait:  hlt                  */
    0x3B, 0x1E, 0x6C, 0x04, /*        cmp  bx, [046Ch]     */
    0x74, 0xF9,             /*        je   wait            */
    0x8B, 0x1E, 0x6C, 0x04, /*        mov  bx, [046Ch]     */
    0xE2, 0xF3,             /*        loop wait            */
    0x1F,                   /*        pop  ds              */
    0xCB,                   /*        retf                 */
};

/* Real-mode routine: make the 8042 emit the scancode for 'A' (command D2h
 * puts a byte in its output buffer as if it came from the keyboard, raising
 * IRQ 1), then wait for a key with INT 16h. Returns AX = 1E61h only if IRQ 1
 * went through the virtual PIC to the BIOS INT 9 handler. */
static const uint8_t inject_key_code[] = {
    0xE4, 0x64,             /* w1:    in   al, 64h         */
    0xA8, 0x02,             /*        test al, 2           */
    0x75, 0xFA,             /*        jnz  w1              */
    0xB0, 0xD2,             /*        mov  al, 0D2h        */
    0xE6, 0x64,             /*        out  64h, al         */
    0xE4, 0x64,             /* w2:    in   al, 64h         */
    0xA8, 0x02,             /*        test al, 2           */
    0x75, 0xFA,             /*        jnz  w2              */
    0xB0, 0x1E,             /*        mov  al, 1Eh  ('A')  */
    0xE6, 0x60,             /*        out  60h, al         */
    0xFB,                   /*        sti                  */
    0x30, 0xE4,             /*        xor  ah, ah          */
    0xCD, 0x16,             /*        int  16h             */
    0xCB,                   /*        retf                 */
};

void v86_selftest(const struct bootinfo *bi)
{
    uint16_t seg = v86_buf_seg(), buf = v86_buf_off();
    struct v86_regs r;

    /* BIOS: conventional memory size */
    r = (struct v86_regs){ 0 };
    call_int(0x12, &r);
    uint32_t conv_kb = r.eax & 0xFFFF;

    /* DOS: version */
    r = (struct v86_regs){ .eax = 0x3000 };
    call_int(0x21, &r);
    uint32_t major = r.eax & 0xFF, minor = (r.eax >> 8) & 0xFF;

    /* DOS: read the first 512 bytes of our own VMM.BIN, compare with RAM */
    const char *path = v86_ptr(seg, bi->kpath_off);
    r = (struct v86_regs){ .eax = 0x3D00, .edx = bi->kpath_off, .ds = seg };
    call_int(0x21, &r);
    CHECK(!(r.flags & EFLAGS_CF), "DOS could not open %s (error %u).", path, r.eax & 0xFFFF);
    uint32_t handle = r.eax & 0xFFFF;

    r = (struct v86_regs){ .eax = 0x3F00, .ebx = handle, .ecx = 512, .edx = buf, .ds = seg };
    call_int(0x21, &r);
    CHECK(!(r.flags & EFLAGS_CF) && (r.eax & 0xFFFF) == 512,
          "DOS read of %s failed (AX=%04X).", path, r.eax & 0xFFFF);
    CHECK(memcmp(v86_ptr(seg, buf), (void *)(uintptr_t)bi->arena_virt, 512) == 0,
          "VMM.BIN read through DOS differs from the kernel in memory.");

    r = (struct v86_regs){ .eax = 0x3E00, .ebx = handle };
    call_int(0x21, &r);

    /* Interrupt reflection: run real-mode code that sleeps on the timer */
    memcpy(v86_ptr(seg, buf), wait_ticks_code, sizeof wait_ticks_code);
    uint32_t before = v86_delivered(0x08);
    r = (struct v86_regs){ 0 };
    if (v86_call_far(seg, buf, &r) != 0)
        panic("V86 timer wait failed: %s", v86_last_error());
    uint32_t ticks = v86_delivered(0x08) - before;
    CHECK(ticks >= 3, "Only %u timer interrupts reached the VM's INT 8 handler.", ticks);

    /* Keyboard reflection: IRQ 1 -> BIOS INT 9 -> INT 16h */
    memcpy(v86_ptr(seg, buf), inject_key_code, sizeof inject_key_code);
    keyboard_release();
    r = (struct v86_regs){ 0 };
    int krc = v86_call_far(seg, buf, &r);
    keyboard_claim();
    if (krc != 0)
        panic("V86 keyboard test failed: %s", v86_last_error());
    CHECK((r.eax & 0xFFFF) == 0x1E61,
          "BIOS INT 16h returned %04X instead of 1E61 ('a').", r.eax & 0xFFFF);

    kprintf("  DOS %u.%02u in V86 mode: %u KB conventional memory,\n", major, minor, conv_kb);
    kprintf("  read %s through INT 21h, %u timer IRQs reflected,\n", path, ticks);
    kprintf("  keyboard IRQ reached the BIOS through the virtual PIC.\n");
    klog("VMM: V86 self-test passed\n");
}
