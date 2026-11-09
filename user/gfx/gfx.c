/*
 * gfx.c -- 640x480x16 planar VGA drawing.
 *
 * Mode 12h keeps each of the 4 colour bits in its own plane, 8 pixels per
 * byte. We draw with the graphics controller's Set/Reset registers: the
 * colour goes in Set/Reset (enabled for all planes), the Bit Mask selects
 * which pixels of a byte change, and a dummy read first loads the latches
 * so the other pixels keep their colours. One CPU write then updates all
 * four planes at once. Function Select = XOR turns the same path into an
 * inverter (cursor, drag outlines).
 */
#include "gfx.h"
#include "io.h"

#include <stdbool.h>

static inline void gc(uint8_t i, uint8_t v) { outb(0x3CE, i); outb(0x3CF, v); }

static void begin(uint8_t color, uint8_t function)
{
    outb(0x3C4, 2); outb(0x3C5, 0x0F);  /* write all planes */
    gc(5, 0x00);                        /* write mode 0 */
    gc(3, function);                    /* 0x00 replace, 0x18 XOR */
    gc(1, 0x0F);                        /* Set/Reset drives all planes */
    gc(0, color);
}

static void end(void)
{
    gc(1, 0x00);
    gc(3, 0x00);
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
    g->planes = 0;
    g->width = 640;
    g->height = 480;
    g->stride = 80;
    g->font = font8x16;
    gfx_noclip(g);
}

void gfx_init_mem(struct gfx *g, uint8_t *planes, const uint8_t *font8x16)
{
    gfx_init(g, 0, font8x16);
    g->planes = planes;
}

/* Memory target: apply what one Set/Reset write would do to all 4 planes. */
static inline void mem_put(struct gfx *g, int row, int col, uint8_t mask,
                           uint8_t color, uint8_t fn)
{
    uint8_t *q = g->planes + row * g->stride + col;
    for (int p = 0; p < 4; p++, q += GFX_PLANE_BYTES) {
        bool on = (color >> p) & 1;
        if (fn)
            { if (on) *q ^= mask; }
        else
            *q = on ? (uint8_t)(*q | mask) : (uint8_t)(*q & ~mask);
    }
}

void gfx_present(const struct gfx *m, volatile uint8_t *fb, int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 640) w = 640 - x;
    if (y + h > 480) h = 480 - y;
    if (w <= 0 || h <= 0)
        return;
    int b1 = x >> 3, b2 = (x + w - 1) >> 3, n = b2 - b1 + 1;
    gc(5, 0x00);                        /* write mode 0, plain copies */
    gc(3, 0x00);
    gc(1, 0x00);
    gc(8, 0xFF);
    for (int p = 0; p < 4; p++) {
        outb(0x3C4, 2); outb(0x3C5, (uint8_t)(1 << p));
        const uint8_t *src = m->planes + p * GFX_PLANE_BYTES + y * 80 + b1;
        volatile uint8_t *dst = fb + y * 80 + b1;
        for (int row = 0; row < h; row++, src += 80, dst += 80)
            for (int i = 0; i < n; i++)
                dst[i] = src[i];
    }
    outb(0x3C4, 2); outb(0x3C5, 0x0F);
}

void gfx_noclip(struct gfx *g)
{
    g->cx0 = 0;
    g->cy0 = 0;
    g->cx1 = g->width;
    g->cy1 = g->height;
}

void gfx_clip(struct gfx *g, int x, int y, int w, int h)
{
    g->cx0 = x < 0 ? 0 : x;
    g->cy0 = y < 0 ? 0 : y;
    g->cx1 = x + w > g->width ? g->width : x + w;
    g->cy1 = y + h > g->height ? g->height : y + h;
}

void gfx_set_palette(const uint8_t rgb[16][3])
{
    for (uint8_t i = 0; i < 16; i++) {  /* attribute palette: identity */
        (void)inb(0x3DA);
        outb(0x3C0, i);
        outb(0x3C0, i);
    }
    (void)inb(0x3DA);
    outb(0x3C0, 0x20);                  /* display on */
    outb(0x3C8, 0);
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++)
            outb(0x3C9, rgb[i][c]);
}

/* Bits of byte column `col` that lie inside the clip rectangle. */
static uint8_t col_mask(const struct gfx *g, int col)
{
    int lo = g->cx0 - col * 8, hi = g->cx1 - col * 8;
    if (lo < 0) lo = 0;
    if (hi > 8) hi = 8;
    if (lo >= hi)
        return 0;
    return (uint8_t)((0xFF >> lo) & (0xFF << (8 - hi)));
}

static int clip_rect(const struct gfx *g, int *x, int *y, int *w, int *h)
{
    int x1 = *x + *w, y1 = *y + *h;
    if (*x < g->cx0) *x = g->cx0;
    if (*y < g->cy0) *y = g->cy0;
    if (x1 > g->cx1) x1 = g->cx1;
    if (y1 > g->cy1) y1 = g->cy1;
    *w = x1 - *x;
    *h = y1 - *y;
    return *w > 0 && *h > 0;
}

static void fill_fn(struct gfx *g, int x, int y, int w, int h, uint8_t color, uint8_t fn)
{
    if (!clip_rect(g, &x, &y, &w, &h))
        return;
    int b1 = x >> 3, b2 = (x + w - 1) >> 3;
    uint8_t m1 = (uint8_t)(0xFF >> (x & 7));
    uint8_t m2 = (uint8_t)(0xFF << (7 - ((x + w - 1) & 7)));

    if (g->planes) {
        for (int row = y; row < y + h; row++)
            for (int b = b1; b <= b2; b++)
                mem_put(g, row, b, (uint8_t)((b == b1 ? m1 : 0xFF) & (b == b2 ? m2 : 0xFF)), color, fn);
        return;
    }
    begin(color, fn);
    for (int row = y; row < y + h; row++) {
        volatile uint8_t *line = g->fb + row * g->stride;
        if (b1 == b2) {
            put_masked(line + b1, m1 & m2);
            continue;
        }
        put_masked(line + b1, m1);
        gc(8, 0xFF);
        for (int b = b1 + 1; b < b2; b++) {
            if (fn)
                (void)line[b];          /* XOR needs the old pixels latched */
            line[b] = 0xFF;
        }
        put_masked(line + b2, m2);
    }
    end();
}

void gfx_fill(struct gfx *g, int x, int y, int w, int h, uint8_t color)
{
    fill_fn(g, x, y, w, h, color, 0x00);
}

void gfx_xor_fill(struct gfx *g, int x, int y, int w, int h)
{
    fill_fn(g, x, y, w, h, 0x0F, 0x18);
}

void gfx_xor_frame(struct gfx *g, int x, int y, int w, int h, int t)
{
    gfx_xor_fill(g, x, y, w, t);                        /* top */
    gfx_xor_fill(g, x, y + h - t, w, t);                /* bottom */
    gfx_xor_fill(g, x, y + t, t, h - 2 * t);            /* left, between them */
    gfx_xor_fill(g, x + w - t, y + t, t, h - 2 * t);    /* right */
}

void gfx_pixel(struct gfx *g, int x, int y, uint8_t color)
{
    if (x < g->cx0 || y < g->cy0 || x >= g->cx1 || y >= g->cy1)
        return;
    if (g->planes) {
        mem_put(g, y, x >> 3, (uint8_t)(0x80 >> (x & 7)), color, 0x00);
        return;
    }
    begin(color, 0x00);
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

/* Draw rows of up to 16 pixels (MSB left) at x,y through the clip. */
static void blit_rows(struct gfx *g, int x, int y, const uint16_t *rows, int h,
                      uint8_t color, uint8_t fn)
{
    if (x >= g->cx1 || y >= g->cy1 || x + 16 <= g->cx0 || y + h <= g->cy0)
        return;
    int shift = x & 7, col = x >> 3;
    uint8_t cm[3];
    for (int b = 0; b < 3; b++)
        cm[b] = (col + b >= 0 && col + b < g->stride) ? col_mask(g, col + b) : 0;

    if (!g->planes)
        begin(color, fn);
    for (int r = 0; r < h; r++) {
        int row = y + r;
        if (row < g->cy0 || row >= g->cy1 || !rows[r])
            continue;
        uint32_t bits = (uint32_t)rows[r] << (8 - shift);    /* 24 bits */
        for (int b = 0; b < 3; b++) {
            uint8_t m = (uint8_t)(bits >> (16 - 8 * b)) & cm[b];
            if (!m)
                continue;
            if (g->planes)
                mem_put(g, row, col + b, m, color, fn);
            else
                put_masked(g->fb + row * g->stride + col + b, m);
        }
    }
    if (!g->planes)
        end();
}

void gfx_bitmap(struct gfx *g, int x, int y, const uint16_t *rows, int h, uint8_t color)
{
    blit_rows(g, x, y, rows, h, color, 0x00);
}

void gfx_xor_bitmap(struct gfx *g, int x, int y, const uint16_t *rows, int h)
{
    blit_rows(g, x, y, rows, h, 0x0F, 0x18);
}

void gfx_text(struct gfx *g, int x, int y, const char *s, uint8_t fg, uint8_t bg)
{
    for (; *s; s++, x += 8) {
        if (x >= g->cx1)
            break;
        if (x + 8 <= g->cx0)
            continue;
        if (bg != GFX_TRANSPARENT)
            gfx_fill(g, x, y, 8, 16, bg);
        const uint8_t *glyph = g->font + (uint8_t)*s * 16;
        uint16_t rows[16];
        for (int r = 0; r < 16; r++)
            rows[r] = (uint16_t)(glyph[r] << 8);
        blit_rows(g, x, y, rows, 16, fg, 0x00);
    }
}

/* Copy w x h pixels (one byte each, a colour index) to x,y through the
 * clip. On a memory target whole bytes of 8 pixels go at once. */
void gfx_blit8(struct gfx *g, int x, int y, int w, int h, const uint8_t *pixels, int stride)
{
    int x0 = x > g->cx0 ? x : g->cx0, x1 = x + w < g->cx1 ? x + w : g->cx1;
    int y0 = y > g->cy0 ? y : g->cy0, y1 = y + h < g->cy1 ? y + h : g->cy1;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (int row = y0; row < y1; row++) {
        const uint8_t *src = pixels + (row - y) * stride - x;
        if (!g->planes) {
            for (int px = x0; px < x1; px++)
                if (src[px] != 0xFF)
                    gfx_pixel(g, px, row, src[px]);
            continue;
        }
        for (int col = x0 >> 3; col <= (x1 - 1) >> 3; col++) {
            uint8_t bits[4] = { 0, 0, 0, 0 }, mask = 0;
            for (int b = 0; b < 8; b++) {
                int px = col * 8 + b;
                if (px < x0 || px >= x1)
                    continue;
                uint8_t bit = (uint8_t)(0x80 >> b), c = src[px];
                if (c == 0xFF)                      /* transparent */
                    continue;
                mask |= bit;
                for (int p = 0; p < 4; p++)
                    if (c & (1 << p))
                        bits[p] |= bit;
            }
            uint8_t *q = g->planes + row * g->stride + col;
            for (int p = 0; p < 4; p++, q += GFX_PLANE_BYTES)
                *q = (uint8_t)((*q & ~mask) | bits[p]);
        }
    }
}
