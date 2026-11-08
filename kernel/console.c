/*
 * console.c -- 80x25 text-mode console, written straight to video RAM.
 * Works on colour (B800h) and monochrome (B000h) adapters.
 */
#include "console.h"
#include "io.h"
#include "kernel.h"

static volatile uint16_t *vram;
static uint16_t crtc;
static int mono;
static int cur_row, cur_col;
static int win_top, win_bottom = CON_ROWS - 1;
static uint8_t cur_attr = ATTR(C_LGRAY, C_BLACK);

static uint8_t fix_attr(uint8_t a)
{
    if (!mono)
        return a;
    return (a & 0x70) ? 0x70 : (a & 0x08) ? 0x0F : 0x07;   /* map to MDA */
}

static void update_cursor(void)
{
    uint16_t pos = (uint16_t)(cur_row * CON_COLS + cur_col);
    outb(crtc, 0x0E); outb(crtc + 1, pos >> 8);
    outb(crtc, 0x0F); outb(crtc + 1, pos & 0xFF);
}

void con_init(void)
{
    mono = LOWMEM8(0x449) == 7;          /* BIOS data area: video mode */
    vram = (volatile uint16_t *)(uintptr_t)(mono ? 0xB0000 : 0xB8000);
    crtc = mono ? 0x3B4 : 0x3D4;
}

void con_set_attr(uint8_t attr) { cur_attr = attr; }

void con_show_cursor(int on)
{
    outb(crtc, 0x0A);
    outb(crtc + 1, on ? 0x0D : 0x20);    /* bit 5 = cursor off */
    if (on) { outb(crtc, 0x0B); outb(crtc + 1, 0x0E); }
}

void con_fill_row(int row, uint8_t attr, char c)
{
    uint16_t cell = (uint16_t)(fix_attr(attr) << 8 | (uint8_t)c);
    for (int x = 0; x < CON_COLS; x++)
        vram[row * CON_COLS + x] = cell;
}

void con_clear(void)
{
    for (int y = win_top; y <= win_bottom; y++)
        con_fill_row(y, cur_attr, ' ');
    cur_row = win_top;
    cur_col = 0;
    update_cursor();
}

void con_set_window(int top, int bottom)
{
    win_top = top;
    win_bottom = bottom;
    cur_row = top;
    cur_col = 0;
}

void con_goto(int row, int col)
{
    cur_row = row;
    cur_col = col;
    update_cursor();
}

static void scroll(void)
{
    for (int y = win_top; y < win_bottom; y++)
        for (int x = 0; x < CON_COLS; x++)
            vram[y * CON_COLS + x] = vram[(y + 1) * CON_COLS + x];
    con_fill_row(win_bottom, cur_attr, ' ');
}

static void putc_raw(char c)
{
    switch (c) {
    case '\n': cur_col = 0; cur_row++; break;
    case '\r': cur_col = 0; break;
    case '\t': cur_col = (cur_col + 8) & ~7; break;
    case '\b': if (cur_col) cur_col--; break;
    default:
        vram[cur_row * CON_COLS + cur_col] =
            (uint16_t)(fix_attr(cur_attr) << 8 | (uint8_t)c);
        cur_col++;
    }
    if (cur_col >= CON_COLS) { cur_col = 0; cur_row++; }
    if (cur_row > win_bottom) { scroll(); cur_row = win_bottom; }
}

void con_putc(char c)
{
    putc_raw(c);
    update_cursor();
}

void con_puts(const char *s)
{
    while (*s)
        putc_raw(*s++);
    update_cursor();
}

void con_write_at(int row, int col, uint8_t attr, const char *s)
{
    uint8_t a = fix_attr(attr);
    while (*s && col < CON_COLS)
        vram[row * CON_COLS + col++] = (uint16_t)(a << 8 | (uint8_t)*s++);
}
