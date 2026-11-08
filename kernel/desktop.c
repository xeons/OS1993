/*
 * desktop.c -- a demo screen for the VGA driver (key G in the VMM console).
 * Not the GUI: that will be ring-3 code in the System VM. This just
 * exercises the drawing primitives and the VDD's mode switching.
 */
#include "kernel.h"
#include "gfx.h"
#include "mm.h"
#include "vga.h"

/* Coordinates the screendump test checks (tools/qemu-vga-test.py) */
#define WIN_X 120
#define WIN_Y 90
#define WIN_W 400
#define WIN_H 260
#define TITLE_H 20

static void window(struct gfx *g, int x, int y, int w, int h, const char *title)
{
    gfx_fill(g, x + 4, y + 4, w, h, GFX_BLACK);             /* drop shadow */
    gfx_fill(g, x, y, w, h, GFX_WHITE);
    gfx_rect(g, x, y, w, h, GFX_BLACK);
    gfx_fill(g, x + 1, y + 1, w - 2, TITLE_H, GFX_BLUE);    /* title bar */
    gfx_hline(g, x + 1, y + TITLE_H + 1, w - 2, GFX_BLACK);
    gfx_fill(g, x + 4, y + 4, 14, 14, GFX_LGRAY);           /* control box */
    gfx_rect(g, x + 4, y + 4, 14, 14, GFX_BLACK);
    gfx_hline(g, x + 7, y + 10, 8, GFX_BLACK);
    int tw = (int)strlen(title) * 8;
    gfx_text(g, x + (w - tw) / 2, y + 3, title, GFX_WHITE, GFX_TRANSPARENT);
}

void desktop_draw(void)
{
    struct gfx g;
    gfx_init(&g, KLOW(0xA0000), vga_font8x16());

    gfx_fill(&g, 0, 0, g.width, g.height, GFX_CYAN);
    window(&g, WIN_X, WIN_Y, WIN_W, WIN_H, "OS1993 VGA Driver");

    int tx = WIN_X + 16, ty = WIN_Y + TITLE_H + 14;
    gfx_text(&g, tx, ty,      "640 x 480, 16 colours, 4 planes.", GFX_BLACK, GFX_TRANSPARENT);
    gfx_text(&g, tx, ty + 20, "Drawn with the Set/Reset registers;", GFX_BLACK, GFX_TRANSPARENT);
    gfx_text(&g, tx, ty + 36, "saved and restored by the VDD.", GFX_BLACK, GFX_TRANSPARENT);
    gfx_text(&g, tx, ty + 64, "N    new DOS session", GFX_DGRAY, GFX_TRANSPARENT);
    gfx_text(&g, tx, ty + 80, "Alt+Tab  switch sessions", GFX_DGRAY, GFX_TRANSPARENT);
    gfx_text(&g, tx, ty + 96, "Esc  back to the text console", GFX_DGRAY, GFX_TRANSPARENT);

    for (int c = 0; c < 16; c++) {                          /* the palette */
        int bx = WIN_X + 16 + c * 23, by = WIN_Y + WIN_H - 40;
        gfx_fill(&g, bx, by, 20, 20, (uint8_t)c);
        gfx_rect(&g, bx, by, 20, 20, GFX_BLACK);
    }
    klog("VGA: desktop drawn\n");
}
