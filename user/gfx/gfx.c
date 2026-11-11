/*
 * gfx.c -- drawing: 640x480x16 planar VGA, and linear 8/16/32-bit modes.
 *
 * Linear modes (the VBE adapter's) are plain memory: a row of pixels of
 * 1, 2 or 4 bytes each. Every drawing function handles that case first.
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
#include <string.h>

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
    g->bpp = 4;
    g->base = 0;
    g->pitch = 80;
    for (int i = 0; i < 16; i++)
        g->pal[i] = (uint32_t)i;
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

void gfx_init_linear(struct gfx *g, uint8_t *base, int w, int h, int bpp, uint32_t pitch,
                     const uint8_t *font8x16)
{
    gfx_init(g, 0, font8x16);
    g->bpp = bpp;
    g->base = base;
    g->pitch = pitch;
    g->width = w;
    g->height = h;
    gfx_noclip(g);
}

/* ---- linear modes ----------------------------------------------------------- */

static int clip_rect(const struct gfx *g, int *x, int *y, int *w, int *h);

static uint32_t xor_value(const struct gfx *g)
{
    return g->bpp == 32 ? 0x00FFFFFFu : g->bpp == 16 ? 0xFFFFu : 0x0Fu;
}

static void lin_fill(struct gfx *g, int x, int y, int w, int h, uint32_t v, bool xor)
{
    if (!clip_rect(g, &x, &y, &w, &h))
        return;
    for (int row = y; row < y + h; row++) {
        uint8_t *p = g->base + (uint32_t)row * g->pitch;
        if (g->bpp == 8) {
            uint8_t *q = p + x;
            for (int i = 0; i < w; i++)
                q[i] = xor ? (uint8_t)(q[i] ^ v) : (uint8_t)v;
        } else if (g->bpp == 16) {
            uint16_t *q = (uint16_t *)p + x;
            for (int i = 0; i < w; i++)
                q[i] = xor ? (uint16_t)(q[i] ^ v) : (uint16_t)v;
        } else {
            uint32_t *q = (uint32_t *)p + x;
            if (xor)
                for (int i = 0; i < w; i++) q[i] ^= v;
            else
                for (int i = 0; i < w; i++) q[i] = v;
        }
    }
}

static void lin_put(struct gfx *g, int x, int y, uint32_t v, bool xor)
{
    if (x < g->cx0 || y < g->cy0 || x >= g->cx1 || y >= g->cy1)
        return;
    uint8_t *p = g->base + (uint32_t)y * g->pitch;
    if (g->bpp == 8)       { uint8_t *q = p + x;              *q = xor ? (uint8_t)(*q ^ v) : (uint8_t)v; }
    else if (g->bpp == 16) { uint16_t *q = (uint16_t *)p + x; *q = xor ? (uint16_t)(*q ^ v) : (uint16_t)v; }
    else                   { uint32_t *q = (uint32_t *)p + x; *q = xor ? *q ^ v : v; }
}

static void lin_rows(struct gfx *g, int x, int y, const uint16_t *rows, int h, uint32_t v, bool xor)
{
    for (int r = 0; r < h; r++)
        for (int b = 0; rows[r] && b < 16; b++)
            if (rows[r] & (0x8000 >> b))
                lin_put(g, x + b, y + r, v, xor);
}

static uint8_t nearest(const struct gfx *g, uint8_t r, uint8_t gr, uint8_t b)
{
    int best = 0;
    int32_t best_d = 0x7FFFFFFF;
    for (int i = 0; i < 16; i++) {
        int32_t dr = r - g->theme[i][0] * 255 / 63, dg = gr - g->theme[i][1] * 255 / 63,
                db = b - g->theme[i][2] * 255 / 63;
        int32_t d = dr * dr + dg * dg + db * db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return (uint8_t)best;
}

static uint32_t rgb_value(const struct gfx *g, uint8_t r, uint8_t gr, uint8_t b)
{
    if (g->bpp <= 8)
        return g->pal[nearest(g, r, gr, b)];
    return ((uint32_t)(r >> (8 - g->rsize)) << g->rpos) | ((uint32_t)(gr >> (8 - g->gsize)) << g->gpos) |
           ((uint32_t)(b >> (8 - g->bsize)) << g->bpos);
}

void gfx_set_colors(struct gfx *g, const uint8_t rgb[16][3], int rpos, int rsize,
                    int gpos, int gsize, int bpos, int bsize)
{
    g->rpos = (uint8_t)rpos; g->rsize = (uint8_t)rsize;
    g->gpos = (uint8_t)gpos; g->gsize = (uint8_t)gsize;
    g->bpos = (uint8_t)bpos; g->bsize = (uint8_t)bsize;
    for (int i = 0; i < 16; i++) {
        for (int c = 0; c < 3; c++)
            g->theme[i][c] = rgb[i][c];
        g->pal[i] = (uint32_t)i;
    }
    if (g->bpp > 8)
        for (int i = 0; i < 16; i++)
            g->pal[i] = rgb_value(g, (uint8_t)(rgb[i][0] * 255 / 63), (uint8_t)(rgb[i][1] * 255 / 63),
                                  (uint8_t)(rgb[i][2] * 255 / 63));
}

void gfx_fill_rgb(struct gfx *g, int x, int y, int w, int h, uint8_t r, uint8_t gr, uint8_t b)
{
    if (g->bpp == 4)
        gfx_fill(g, x, y, w, h, nearest(g, r, gr, b));
    else
        lin_fill(g, x, y, w, h, rgb_value(g, r, gr, b), false);
}

/* ---- planar mode 12h ---------------------------------------------------------- */

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

void gfx_present(const struct gfx *m, struct gfx *screen, int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > m->width) w = m->width - x;
    if (y + h > m->height) h = m->height - y;
    if (w <= 0 || h <= 0)
        return;
    if (m->bpp != 4) {                  /* rows of whole pixels */
        uint32_t bytes = (uint32_t)w * (uint32_t)(m->bpp / 8), off = (uint32_t)x * (uint32_t)(m->bpp / 8);
        for (int row = y; row < y + h; row++) {
            const uint8_t *src = m->base + (uint32_t)row * m->pitch + off;
            uint8_t *dst = screen->base + (uint32_t)row * screen->pitch + off;
            uint32_t n = bytes;
            while (n >= 4 && !((uintptr_t)dst & 3)) {
                *(uint32_t *)dst = *(const uint32_t *)src;
                dst += 4; src += 4; n -= 4;
            }
            while (n--) *dst++ = *src++;
        }
        return;
    }
    volatile uint8_t *fb = screen->fb;
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
    if (g->bpp != 4) {
        lin_fill(g, x, y, w, h, fn ? xor_value(g) : g->pal[color & 15], fn != 0);
        return;
    }
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
    if (g->bpp != 4) {
        lin_put(g, x, y, g->pal[color & 15], false);
        return;
    }
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
    if (g->bpp != 4) {
        lin_rows(g, x, y, rows, h, fn ? xor_value(g) : g->pal[color & 15], fn != 0);
        return;
    }
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

static void sprite_rows(const struct gfx *g, int x, int y, int w, int h, uint8_t *save, bool restore)
{
    int bpp = g->bpp / 8;
    for (int r = 0; r < h; r++) {
        int row = y + r;
        if (row < 0 || row >= g->height)
            continue;
        for (int c = 0; c < w; c++) {
            int col = x + c;
            if (col < 0 || col >= g->width)
                continue;
            uint8_t *p = g->base + (uint32_t)row * g->pitch + (uint32_t)col * (uint32_t)bpp;
            uint8_t *s = save + ((uint32_t)r * (uint32_t)w + (uint32_t)c) * (uint32_t)bpp;
            for (int k = 0; k < bpp; k++)
                if (restore) p[k] = s[k]; else s[k] = p[k];
        }
    }
}

void gfx_sprite_save(const struct gfx *src, int x, int y, int w, int h, uint8_t *save)
{
    if (src->bpp != 4)
        sprite_rows(src, x, y, w, h, save, false);
}

void gfx_sprite_restore(struct gfx *g, int x, int y, int w, int h, const uint8_t *save)
{
    if (g->bpp != 4)
        sprite_rows(g, x, y, w, h, (uint8_t *)save, true);
}

void gfx_sprite_draw(struct gfx *g, int x, int y, const char *const *art, int w, int h)
{
    if (g->bpp == 4)
        return;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w && art[r][c]; c++) {
            int row = y + r, col = x + c;
            char a = art[r][c];
            if ((a != 'k' && a != 'w') || row < 0 || row >= g->height || col < 0 || col >= g->width)
                continue;
            uint32_t v = g->pal[a == 'k' ? 0 : 8];      /* the theme's black and white */
            uint8_t *p = g->base + (uint32_t)row * g->pitch;
            if (g->bpp == 8)       p[col] = (uint8_t)v;
            else if (g->bpp == 16) ((uint16_t *)p)[col] = (uint16_t)v;
            else                   ((uint32_t *)p)[col] = v;
        }
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

/* The proportional font: each 8x16 glyph trimmed to its inked columns,
 * one blank column after it (space: 4 pixels). The widths are worked out
 * once from whatever font the gfx has. */
static const uint8_t *prop_font;
static uint8_t prop_lead[256], prop_adv[256];

static void prop_init(const uint8_t *font)
{
    if (prop_font == font)
        return;
    for (int c = 0; c < 256; c++) {
        uint8_t ink = 0;
        for (int r = 0; r < 16; r++)
            ink |= font[c * 16 + r];
        if (!ink || c == ' ') {
            prop_lead[c] = 0;
            prop_adv[c] = c == ' ' || !ink ? 4 : 8;
            continue;
        }
        int lead = 0, last = 7;
        while (!(ink & (0x80 >> lead))) lead++;
        while (!(ink & (0x80 >> last))) last--;
        prop_lead[c] = (uint8_t)lead;
        prop_adv[c] = (uint8_t)(last - lead + 2);
    }
    prop_font = font;
}

int gfx_text_width(const struct gfx *g, int font, const char *s, int n)
{
    if (n < 0)
        n = (int)strlen(s);
    if (font == GFX_FONT_FIXED)
        return n * 8;
    prop_init(g->font);
    int w = 0;
    for (int i = 0; i < n; i++)
        w += prop_adv[(uint8_t)s[i]];
    return w;
}

/* n characters of s (n < 0: all) in a font; -> the x after them. */
int gfx_text_font(struct gfx *g, int font, int x, int y, const char *s, int n, uint8_t fg, uint8_t bg)
{
    if (n < 0)
        n = (int)strlen(s);
    if (font == GFX_FONT_PROP)
        prop_init(g->font);
    for (int i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        int adv = font == GFX_FONT_PROP ? prop_adv[c] : 8, lead = font == GFX_FONT_PROP ? prop_lead[c] : 0;
        if (x >= g->cx1)
            break;
        if (x + adv > g->cx0) {
            if (bg != GFX_TRANSPARENT)
                gfx_fill(g, x, y, adv, 16, bg);
            const uint8_t *glyph = g->font + c * 16;
            uint16_t rows[16];
            for (int r = 0; r < 16; r++)
                rows[r] = (uint16_t)((glyph[r] << lead) << 8);
            blit_rows(g, x, y, rows, 16, fg, 0x00);
        }
        x += adv;
    }
    return x;
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
        if (g->bpp != 4) {
            for (int px = x0; px < x1; px++)
                if (src[px] != 0xFF)
                    lin_put(g, px, row, g->pal[src[px] & 15], false);
            continue;
        }
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
