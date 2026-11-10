/*
 * vga.h -- the VGA hardware owner (a small VDD).
 *
 * The VMM console can show either its 80x25 text screen or a 640x480x16
 * graphics desktop; DOS sessions are text mode. Switching the screen
 * between them saves and restores the complete VGA state: registers, DAC
 * palette, the four video planes (graphics) or the font plane (text).
 */
#ifndef OS1993_VGA_H
#define OS1993_VGA_H

#include <stdbool.h>
#include <stdint.h>

#define VGA_GFX_W       640
#define VGA_GFX_H       480
#define VGA_PLANE_BYTES (VGA_GFX_W / 8 * VGA_GFX_H)     /* 38400 */

struct vga_regs {
    uint8_t misc;
    uint8_t seq[5];
    uint8_t crtc[25];
    uint8_t gc[9];
    uint8_t ac[21];
    uint8_t dac_mask;
    uint8_t dac[768];
};

void vga_init(void);                /* boot: capture text mode state + fonts */
const uint8_t *vga_font8x16(void);  /* the VGA BIOS 8x16 font, 256 glyphs */

/* The console's graphics desktop (System VM thread only). */
bool vga_console_gfx(void);
bool vga_gfx_enter(void);           /* BIOS mode 12h; console goes to its shadow */
void vga_gfx_leave(void);           /* BIOS mode 03h; console back on screen */

/* Focus switching (vm.c): the console's graphics screen vs. text VMs. */
void vga_console_suspend(void);     /* save graphics, switch hardware to text */
void vga_console_resume(void);      /* restore graphics exactly */

void vga_force_text(void);          /* panic / exit: text mode, no BIOS */
void vga_planar_restore(void);      /* mode 12h again (leaving the VBE adapter's modes) */

/* Low-level state access (also used by the tests). */
void vga_save_regs(struct vga_regs *s);
void vga_load_regs(const struct vga_regs *s);

#endif
