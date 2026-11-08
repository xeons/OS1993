/*
 * gfx.h -- 640x480x16 planar VGA drawing (the display driver).
 *
 * Self-contained on purpose: it needs only port I/O to the VGA (3C4h/3CEh,
 * which the TSS I/O bitmap leaves open) and a pointer to A000:0000, so it
 * can move into the ring-3 GUI as its display driver, like Windows' VGA.DRV.
 */
#ifndef OS1993_GFX_H
#define OS1993_GFX_H

#include <stdint.h>

enum {                                  /* default VGA palette */
    GFX_BLACK, GFX_BLUE, GFX_GREEN, GFX_CYAN, GFX_RED, GFX_MAGENTA, GFX_BROWN,
    GFX_LGRAY, GFX_DGRAY, GFX_LBLUE, GFX_LGREEN, GFX_LCYAN, GFX_LRED,
    GFX_LMAGENTA, GFX_YELLOW, GFX_WHITE
};
#define GFX_TRANSPARENT 0xFF            /* text background: leave as is */

struct gfx {
    volatile uint8_t *fb;               /* A000:0000 */
    int width, height, stride;
    const uint8_t *font;                /* 8x16, 256 glyphs */
};

void gfx_init(struct gfx *g, volatile uint8_t *fb, const uint8_t *font8x16);
void gfx_fill(struct gfx *g, int x, int y, int w, int h, uint8_t color);
void gfx_pixel(struct gfx *g, int x, int y, uint8_t color);
void gfx_hline(struct gfx *g, int x, int y, int w, uint8_t color);
void gfx_vline(struct gfx *g, int x, int y, int h, uint8_t color);
void gfx_rect(struct gfx *g, int x, int y, int w, int h, uint8_t color);
void gfx_text(struct gfx *g, int x, int y, const char *s, uint8_t fg, uint8_t bg);
/* XOR a 1-bit image (rows of up to 16 pixels, MSB = leftmost) onto the
 * screen, inverting the pixels under it; drawing it again erases it. */
void gfx_xor_bitmap(struct gfx *g, int x, int y, const uint16_t *rows, int h);

#endif
