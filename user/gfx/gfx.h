/*
 * gfx.h -- 640x480x16 planar VGA drawing (the display driver).
 *
 * Self-contained on purpose: it needs only port I/O to the VGA (which the
 * TSS I/O bitmap leaves open to ring 3) and a pointer to A000:0000. It is
 * linked into the ring-3 GUI, like Windows' VGA.DRV.
 *
 * All drawing is clipped to the clip rectangle (the whole screen by
 * default). A gfx can draw to the screen, or to an off-screen copy of it in
 * memory (same 4-plane layout) that gfx_present() then copies to the screen
 * -- double buffering, so nothing half-drawn is ever visible.
 */
#ifndef OS1993_GFX_H
#define OS1993_GFX_H

#include <stdint.h>

#define GFX_TRANSPARENT 0xFF            /* text background: leave as is */

#define GFX_PLANE_BYTES (80 * 480)      /* one plane of the 640x480 screen */

struct gfx {
    volatile uint8_t *fb;               /* A000:0000 (screen target) */
    uint8_t *planes;                    /* or 4 x GFX_PLANE_BYTES in memory */
    int width, height, stride;
    const uint8_t *font;                /* 8x16, 256 glyphs */
    int cx0, cy0, cx1, cy1;             /* clip rectangle, [x0,x1) x [y0,y1) */
};

void gfx_init(struct gfx *g, volatile uint8_t *fb, const uint8_t *font8x16);
void gfx_init_mem(struct gfx *g, uint8_t *planes, const uint8_t *font8x16);
/* Copy a rectangle (widened to whole bytes) of a memory gfx to the screen. */
void gfx_present(const struct gfx *mem, volatile uint8_t *fb, int x, int y, int w, int h);
void gfx_clip(struct gfx *g, int x, int y, int w, int h);   /* within the screen */
void gfx_noclip(struct gfx *g);

/* 16 colours: the attribute controller is set to the identity map and the
 * DAC loaded with rgb[i][0..2] (6-bit values). */
void gfx_set_palette(const uint8_t rgb[16][3]);

void gfx_fill(struct gfx *g, int x, int y, int w, int h, uint8_t color);
void gfx_pixel(struct gfx *g, int x, int y, uint8_t color);
void gfx_hline(struct gfx *g, int x, int y, int w, uint8_t color);
void gfx_vline(struct gfx *g, int x, int y, int h, uint8_t color);
void gfx_rect(struct gfx *g, int x, int y, int w, int h, uint8_t color);
void gfx_text(struct gfx *g, int x, int y, const char *s, uint8_t fg, uint8_t bg);

/* 1-bit images, rows of up to 16 pixels, MSB = leftmost */
void gfx_bitmap(struct gfx *g, int x, int y, const uint16_t *rows, int h, uint8_t color);
void gfx_blit8(struct gfx *g, int x, int y, int w, int h, const uint8_t *pixels, int stride);
/* Invert the pixels under an image or rectangle; doing it twice undoes it. */
void gfx_xor_bitmap(struct gfx *g, int x, int y, const uint16_t *rows, int h);
void gfx_xor_fill(struct gfx *g, int x, int y, int w, int h);
void gfx_xor_frame(struct gfx *g, int x, int y, int w, int h, int thick);

#endif
