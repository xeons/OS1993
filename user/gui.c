/*
 * user/gui.c -- the first ring-3 program: the desktop.
 *
 * Runs in ring 3 in the System VM. It draws with the same display driver
 * (kernel/gfx.c, compiled into this binary) straight into the video memory
 * the VMM mapped for it, reads keys through INT 30h, and asks the VMM for
 * DOS sessions. With a test flag it instead checks that ring 3 is fenced in.
 */
#include "gfx.h"
#include "os.h"

/* Coordinates the screendump test checks (tools/qemu-vga-test.py) */
#define WIN_X 120
#define WIN_Y 90
#define WIN_W 400
#define WIN_H 260
#define TITLE_H 20

#define SC_ESC 0x01
#define SC_N   0x31

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

static void draw_desktop(struct gfx *g)
{
    gfx_fill(g, 0, 0, g->width, g->height, GFX_CYAN);
    window(g, WIN_X, WIN_Y, WIN_W, WIN_H, "OS1993 VGA Driver");

    int tx = WIN_X + 16, ty = WIN_Y + TITLE_H + 14;
    gfx_text(g, tx, ty,      "640 x 480, 16 colours, 4 planes.", GFX_BLACK, GFX_TRANSPARENT);
    gfx_text(g, tx, ty + 20, "Drawn from ring 3 in the System VM;", GFX_BLACK, GFX_TRANSPARENT);
    gfx_text(g, tx, ty + 36, "saved and restored by the VDD.", GFX_BLACK, GFX_TRANSPARENT);
    gfx_text(g, tx, ty + 64, "N    new DOS session", GFX_DGRAY, GFX_TRANSPARENT);
    gfx_text(g, tx, ty + 80, "Alt+Tab  switch sessions", GFX_DGRAY, GFX_TRANSPARENT);
    gfx_text(g, tx, ty + 96, "Esc  back to the text console", GFX_DGRAY, GFX_TRANSPARENT);

    for (int c = 0; c < 16; c++) {                          /* the palette */
        int bx = WIN_X + 16 + c * 23, by = WIN_Y + WIN_H - 40;
        gfx_fill(g, bx, by, 20, 20, (uint8_t)c);
        gfx_rect(g, bx, by, 20, 20, GFX_BLACK);
    }
}

/* Self-tests: each one must end with the VMM killing this program. */
static int selftest(const struct user_info *info)
{
    sys_print("ring-3 self-test running");
    uint32_t t0 = sys_ticks();
    sys_wait_tick();
    sys_wait_tick();
    if (sys_ticks() > t0)
        sys_print("timer ticks advance");
    if (info->flags & UINFO_TEST_CLI)
        __asm__ volatile("cli");                            /* must #GP */
    if (info->flags & UINFO_TEST_KERNEL)
        *(volatile uint32_t *)0x80000000u = 0;              /* must #PF */
    sys_print("privileged operation was allowed!");
    return 1;
}

int main(struct user_info *info)
{
    if (info->flags & (UINFO_TEST_CLI | UINFO_TEST_KERNEL))
        return selftest(info);

    struct gfx g;
    gfx_init(&g, (volatile uint8_t *)info->vga, (const uint8_t *)info->font);
    draw_desktop(&g);
    sys_print("desktop drawn");

    for (;;) {
        int sc = sys_get_key();
        if (sc == SC_ESC)
            return 0;
        if (sc == SC_N)
            sys_new_session();
        if (sc < 0)
            sys_wait_tick();
    }
}
