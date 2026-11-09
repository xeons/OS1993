/*
 * vga.c -- the VGA hardware owner (a small VDD).
 *
 * Everything here is plain VGA register programming, as a 1993 VDD did it.
 * All VGA registers are readable (unlike the EGA's), so a mode's complete
 * state can be captured after the BIOS sets it and replayed later.
 */
#include "kernel.h"
#include "console.h"
#include "io.h"
#include "mm.h"
#include "v86.h"
#include "vga.h"

#define MISC_W   0x3C2
#define MISC_R   0x3CC
#define SEQ_I    0x3C4
#define SEQ_D    0x3C5
#define CRTC_I   0x3D4
#define CRTC_D   0x3D5
#define GC_I     0x3CE
#define GC_D     0x3CF
#define AC_IW    0x3C0              /* index and data writes (flip-flop) */
#define AC_R     0x3C1
#define STATUS1  0x3DA              /* reading resets the AC flip-flop */
#define DAC_MASK 0x3C6
#define DAC_RI   0x3C7
#define DAC_WI   0x3C8
#define DAC_D    0x3C9

#define FONT_BYTES 8192             /* text font block 0 in plane 2: 256 x 32 */

static struct vga_regs text_regs;   /* mode 03h as the BIOS left it at boot */
static struct vga_regs gfx_regs;    /* mode 12h as the BIOS set it */
static uint8_t text_font[FONT_BYTES];
static uint8_t font8x16[256 * 16];  /* for drawing text in graphics */
static uint8_t *gfx_planes;         /* saved desktop while a VM has the screen */
static bool console_gfx;            /* the console shows the desktop */
static bool gfx_on_screen;          /* the hardware is in the desktop's mode */

static volatile uint8_t *const vram = (volatile uint8_t *)KLOW(0xA0000);

static void wr(uint16_t ip, uint8_t i, uint8_t v) { outb(ip, i); outb((uint16_t)(ip + 1), v); }
static uint8_t rd(uint16_t ip, uint8_t i) { outb(ip, i); return inb((uint16_t)(ip + 1)); }

void vga_save_regs(struct vga_regs *s)
{
    s->misc = inb(MISC_R);
    for (uint8_t i = 0; i < 5; i++)  s->seq[i] = rd(SEQ_I, i);
    for (uint8_t i = 0; i < 25; i++) s->crtc[i] = rd(CRTC_I, i);
    for (uint8_t i = 0; i < 9; i++)  s->gc[i] = rd(GC_I, i);
    for (uint8_t i = 0; i < 21; i++) {
        (void)inb(STATUS1);
        outb(AC_IW, i);
        s->ac[i] = inb(AC_R);
    }
    (void)inb(STATUS1);
    outb(AC_IW, 0x20);              /* palette address source: display on */
    s->dac_mask = inb(DAC_MASK);
    outb(DAC_RI, 0);
    for (int i = 0; i < 768; i++)
        s->dac[i] = inb(DAC_D);
}

void vga_load_regs(const struct vga_regs *s)
{
    wr(SEQ_I, 0, 0x01);             /* synchronous reset while clocks change */
    outb(MISC_W, s->misc);
    for (uint8_t i = 1; i < 5; i++)
        wr(SEQ_I, i, s->seq[i]);
    wr(SEQ_I, 0, 0x03);

    wr(CRTC_I, 0x11, s->crtc[0x11] & 0x7F);    /* unlock CRTC 0-7 */
    for (uint8_t i = 0; i < 25; i++)
        if (i != 0x11)
            wr(CRTC_I, i, s->crtc[i]);
    wr(CRTC_I, 0x11, s->crtc[0x11]);

    for (uint8_t i = 0; i < 9; i++)
        wr(GC_I, i, s->gc[i]);

    (void)inb(STATUS1);
    for (uint8_t i = 0; i < 21; i++) {
        outb(AC_IW, i);
        outb(AC_IW, s->ac[i]);
    }
    outb(DAC_MASK, s->dac_mask);
    outb(DAC_WI, 0);
    for (int i = 0; i < 768; i++)
        outb(DAC_D, s->dac[i]);
    (void)inb(STATUS1);
    outb(AC_IW, 0x20);
}

/* Map the planes one at a time, linearly, at A000:0000. Callers reload a
 * complete register set afterwards. */
static void plane_access(void)
{
    wr(SEQ_I, 4, 0x06);             /* extended memory, no odd/even, no chain-4 */
    wr(GC_I, 1, 0x00);              /* no set/reset */
    wr(GC_I, 3, 0x00);              /* no rotate, replace */
    wr(GC_I, 5, 0x00);              /* write mode 0, read mode 0 */
    wr(GC_I, 6, 0x05);              /* graphics, A0000-AFFFF */
    wr(GC_I, 8, 0xFF);              /* all bits */
}

static void plane_read(int p, uint8_t *dst, uint32_t n)
{
    wr(GC_I, 4, (uint8_t)p);
    for (uint32_t i = 0; i < n; i++)
        dst[i] = vram[i];
}

static void plane_write(int p, const uint8_t *src, uint32_t n)
{
    wr(SEQ_I, 2, (uint8_t)(1 << p));
    for (uint32_t i = 0; i < n; i++)
        vram[i] = src[i];
}

void vga_init(void)
{
    vga_save_regs(&text_regs);
    plane_access();
    plane_read(2, text_font, FONT_BYTES);
    vga_load_regs(&text_regs);

    /* INT 10h AX=1130h BH=06h: ES:BP -> the VGA BIOS 8x16 font */
    struct v86_regs r = { .eax = 0x1130, .ebx = 0x0600 };
    if (v86_int(0x10, &r) == 0) {
        const uint8_t *f = KLOW(((uint32_t)r.es << 4) + (uint16_t)r.ebp);
        memcpy(font8x16, f, sizeof font8x16);
    }
    gfx_planes = kmalloc(4 * VGA_PLANE_BYTES);
    if (!gfx_planes)
        panic("VGA: no memory for the graphics save buffer.");
    klog("VGA: text state saved (misc %02X), BIOS font at %04X:%04X\n",
         text_regs.misc, r.es, r.ebp & 0xFFFF);
}

const uint8_t *vga_font8x16(void) { return font8x16; }
bool vga_console_gfx(void)        { return console_gfx; }

static void restore_text_hw(void)
{
    plane_access();                 /* graphics modes overwrite the font */
    plane_write(2, text_font, FONT_BYTES);
    vga_load_regs(&text_regs);
}

bool vga_gfx_enter(void)
{
    if (console_gfx)
        return true;
    con_detach();                   /* text output continues in the shadow */
    struct v86_regs r = { .eax = 0x0012 };          /* 640x480x16 */
    if (v86_int(0x10, &r) != 0) {
        con_attach();
        return false;
    }
    vga_save_regs(&gfx_regs);
    console_gfx = gfx_on_screen = true;
    return true;
}

void vga_gfx_leave(void)
{
    if (!console_gfx)
        return;
    struct v86_regs r = { .eax = 0x0003 };          /* BIOS text mode */
    v86_int(0x10, &r);
    console_gfx = gfx_on_screen = false;
    con_attach();
}

void vga_console_suspend(void)
{
    if (!console_gfx || !gfx_on_screen)
        return;
    vga_save_regs(&gfx_regs);       /* as the GUI left them: its palette too */
    plane_access();
    for (int p = 0; p < 4; p++)
        plane_read(p, gfx_planes + p * VGA_PLANE_BYTES, VGA_PLANE_BYTES);
    restore_text_hw();
    gfx_on_screen = false;
}

void vga_console_resume(void)
{
    if (!console_gfx || gfx_on_screen)
        return;
    plane_access();
    for (int p = 0; p < 4; p++)
        plane_write(p, gfx_planes + p * VGA_PLANE_BYTES, VGA_PLANE_BYTES);
    vga_load_regs(&gfx_regs);
    gfx_on_screen = true;
}

void vga_force_text(void)
{
    if (!gfx_on_screen)
        return;
    restore_text_hw();
    console_gfx = gfx_on_screen = false;
}
