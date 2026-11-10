/*
 * vbe.h -- VESA BIOS Extensions (vbe.c): linear framebuffer modes.
 */
#ifndef OS1993_VBE_H
#define OS1993_VBE_H

#include <stdbool.h>
#include <stdint.h>

#define VBE_MODE_MAX 32

struct vbe_mode {
    uint16_t number;
    uint16_t width, height;
    uint8_t  bpp;                       /* 8, 15, 16, 24 or 32 */
    uint16_t pitch;                     /* bytes per scan line */
    uint32_t lfb;                       /* physical address of the framebuffer */
    uint8_t  red_size, red_pos, green_size, green_pos, blue_size, blue_pos;
};

void vbe_init(void);                    /* console thread, at boot */
int  vbe_mode_count(void);
const struct vbe_mode *vbe_mode(int i);
const struct vbe_mode *vbe_find(int w, int h, int bpp);
bool vbe_set(const struct vbe_mode *m); /* console thread: 4F02h, linear */

#endif
