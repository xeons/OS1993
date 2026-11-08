/*
 * gfx.c -- 640x480x16 planar VGA drawing.
 *
 * Mode 12h keeps each of the 4 colour bits in its own plane, 8 pixels per
 * byte. We draw with the graphics controller's Set/Reset registers: the
 * colour goes in Set/Reset (enabled for all planes), the Bit Mask selects
 * which pixels of a byte change, and a dummy read first loads the latches
 * so the other pixels keep their colours. One CPU write then updates all
 * four planes at once.
 */
#include "gfx.h"
#include "io.h"

static inline void gc(uint8_t i, uint8_t v) { outb(0x3CE, i); outb(0x3CF, v); }

/* Put the hardware into our drawing setup; undone by end(). */
static void begin(uint8_t color)
{
    outb(0x3C4, 2); outb(0x3C5, 0x0F);  /* write all planes */
    gc(5, 0x00);                        /* write mode 0 */
    gc(3, 0x00);                        /* replace, no rotate */
    gc(1, 0x0F);                        /* Set/Reset drives all planes */
    gc(0, color);
}

static void end(void)
{
    gc(1, 0x00);
    gc(8, 0xFF);
}

static inline void put_masked(volatile uint8_t *p, uint8_t mask)
{
    gc(8, mask);
    (void)*p;                           /* load latches */
    *p = 0xFF;                          /* data ignored: Set/Reset supplies it */
}

void gfx_init(struct gfx *g, volatile uint8_t *fb, const uint8_t *font8x16)
{
    g->fb = fb;
    g->width = 640;
    g->height = 480;
    g->stride = 80;
    g->font = font8x16;
}

static int clip(struct gfx *g, int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > g->width)  *w = g->width - *x;
    if (*y + *h > g->height) *h = g->height - *y;
    return *w > 0 && *h > 0;
}

void gfx_fill(struct gfx *g, int x, int y, int w, int h, uint8_t color)
{
    if (!clip(g, &x, &y, &w, &h))
        return;
    int b1 = x >> 3, b2 = (x + w - 1) >> 3;
    uint8_t m1 = (uint8_t)(0xFF >> (x & 7));
    uint8_t m2 = (uint8_t)(0xFF << (7 - ((x + w - 1) & 7)));

    begin(color);
    for (int row = y; row < y + h; row++) {
        volatile uint8_t *line = g->fb + row * g->stride;
        if (b1 == b2) {
            put_masked(line + b1, m1 & m2);
            continue;
        }
        put_masked(line + b1, m1);
        gc(8, 0xFF);
        for (int b = b1 + 1; b < b2; b++)
            line[b] = 0xFF;             /* whole bytes: no latch read needed */
        put_masked(line + b2, m2);
    }
    end();
}

void gfx_pixel(struct gfx *g, int x, int y, uint8_t color)
{
    if (x < 0 || y < 0 || x >= g->width || y >= g->height)
        return;
    begin(color);
    put_masked(g->fb + y * g->stride + (x >> 3), (uint8_t)(0x80 >> (x & 7)));
    end();
}

void gfx_hline(struct gfx *g, int x, int y, int w, uint8_t color) { gfx_fill(g, x, y, w, 1, color); }
void gfx_vline(struct gfx *g, int x, int y, int h, uint8_t color) { gfx_fill(g, x, y, 1, h, color); }

void gfx_rect(struct gfx *g, int x, int y, int w, int h, uint8_t color)
{
    gfx_hline(g, x, y, w, color);
    gfx_hline(g, x, y + h - 1, w, color);
    gfx_vline(g, x, y, h, color);
    gfx_vline(g, x + w - 1, y, h, color);
}

void gfx_text(struct gfx *g, int x, int y, const char *s, uint8_t fg, uint8_t bg)
{
    for (; *s; s++, x += 8) {
        if (x + 8 <= 0 || y + 16 <= 0 || x >= g->width || y >= g->height)
            continue;
        if (bg != GFX_TRANSPARENT)
            gfx_fill(g, x, y, 8, 16, bg);
        const uint8_t *glyph = g->font + (uint8_t)*s * 16;
        int shift = x & 7, col = x >> 3;
        begin(fg);
        for (int r = 0; r < 16; r++) {
            int row = y + r;
            uint8_t bits = glyph[r];
            if (row < 0 || row >= g->height || !bits)
                continue;
            volatile uint8_t *p = g->fb + row * g->stride + col;
            if (x >= 0 && (bits >> shift))
                put_masked(p, (uint8_t)(bits >> shift));
            if (shift && col + 1 < g->stride && (uint8_t)(bits << (8 - shift)))
                put_masked(p + 1, (uint8_t)(bits << (8 - shift)));
        }
        end();
    }
}

void gfx_xor_bitmap(struct gfx *g, int x, int y, const uint16_t *rows, int h)
{
    begin(0x0F);                        /* XOR with all-ones inverts */
    gc(3, 0x18);                        /* function select: XOR */
    int shift = x & 7, col = x >> 3;
    for (int r = 0; r < h; r++) {
        int row = y + r;
        if (row < 0 || row >= g->height)
            continue;
        /* 16 pixels starting at a byte boundary - shift: up to 3 bytes */
        uint32_t bits = (uint32_t)rows[r] << (8 - shift);
        volatile uint8_t *p = g->fb + row * g->stride + col;
        for (int b = 0; b < 3; b++) {
            uint8_t m = (uint8_t)(bits >> (16 - 8 * b));
            if (m && col + b >= 0 && col + b < g->stride)
                put_masked(p + b, m);
        }
    }
    gc(3, 0x00);
    end();
}
