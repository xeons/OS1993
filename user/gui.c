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

#include <stdbool.h>

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

static void draw_window(struct gfx *g, int wx, int wy)
{
    window(g, wx, wy, WIN_W, WIN_H, "OS1993 VGA Driver");

    int tx = wx + 16, ty = wy + TITLE_H + 14;
    gfx_text(g, tx, ty,      "640 x 480, 16 colours, 4 planes.", GFX_BLACK, GFX_TRANSPARENT);
    gfx_text(g, tx, ty + 20, "Drawn from ring 3 in the System VM;", GFX_BLACK, GFX_TRANSPARENT);
    gfx_text(g, tx, ty + 36, "saved and restored by the VDD.", GFX_BLACK, GFX_TRANSPARENT);
    gfx_text(g, tx, ty + 64, "N    new DOS session   (drag me)", GFX_DGRAY, GFX_TRANSPARENT);
    gfx_text(g, tx, ty + 80, "Alt+Tab  switch sessions", GFX_DGRAY, GFX_TRANSPARENT);
    gfx_text(g, tx, ty + 96, "Esc  back to the text console", GFX_DGRAY, GFX_TRANSPARENT);

    for (int c = 0; c < 16; c++) {                          /* the palette */
        int bx = wx + 16 + c * 23, by = wy + WIN_H - 40;
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

/* The classic arrow, 11 x 16 (MSB = leftmost pixel) */
static const uint16_t arrow[16] = {
    0x8000, 0xC000, 0xA000, 0x9000, 0x8800, 0x8400, 0x8200, 0x8100,
    0x8080, 0x8040, 0x8780, 0x9400, 0xB400, 0xCA00, 0x0A00, 0x0400,
};

static struct gfx g;
static int win_x = WIN_X, win_y = WIN_Y;
static int cur_x, cur_y;
static bool cursor_shown;

static void cursor(bool show)
{
    if (show != cursor_shown) {
        gfx_xor_bitmap(&g, cur_x, cur_y, arrow, 16);
        cursor_shown = show;
    }
}

static void print_xy(const char *what, int x, int y)
{
    char buf[48];
    int n = 0;
    while (*what) buf[n++] = *what++;
    int v[2] = { x, y };
    for (int k = 0; k < 2; k++) {
        char d[8];
        int m = 0, val = v[k] < 0 ? -v[k] : v[k];
        do { d[m++] = (char)('0' + val % 10); val /= 10; } while (val);
        if (v[k] < 0) buf[n++] = '-';
        while (m) buf[n++] = d[--m];
        if (k == 0) buf[n++] = ',';
    }
    buf[n] = '\0';
    sys_print(buf);
}

static void move_window(int nx, int ny)
{
    cursor(false);
    gfx_fill(&g, win_x, win_y, WIN_W + 4, WIN_H + 4, GFX_CYAN);   /* incl. shadow */
    win_x = nx;
    win_y = ny;
    draw_window(&g, win_x, win_y);
    cursor(true);
}

int main(struct user_info *info)
{
    if (info->flags & (UINFO_TEST_CLI | UINFO_TEST_KERNEL))
        return selftest(info);

    gfx_init(&g, (volatile uint8_t *)info->vga, (const uint8_t *)info->font);
    gfx_fill(&g, 0, 0, g.width, g.height, GFX_CYAN);
    draw_window(&g, win_x, win_y);
    sys_print("desktop drawn");
    bool mouse = sys_mouse();
    if (mouse)
        sys_print("mouse ready");

    bool dragging = false;
    int grab_dx = 0, grab_dy = 0;
    uint8_t last_buttons = 0;

    for (;;) {
        struct user_event e;
        if (!sys_get_event(&e)) {
            sys_wait_tick();
            continue;
        }
        if (e.type == EV_KEY) {
            if (e.key == SC_ESC)
                return 0;
            if (e.key == SC_N)
                sys_new_session();
            continue;
        }
        /* EV_MOUSE */
        cursor(false);
        cur_x = e.x;
        cur_y = e.y;
        bool pressed  = (e.buttons & MB_LEFT) && !(last_buttons & MB_LEFT);
        bool released = !(e.buttons & MB_LEFT) && (last_buttons & MB_LEFT);
        last_buttons = e.buttons;

        if (pressed) {
            print_xy("click at ", cur_x, cur_y);
            if (cur_x >= win_x && cur_x < win_x + WIN_W &&
                cur_y >= win_y && cur_y <= win_y + TITLE_H) {
                dragging = true;                /* grabbed the title bar */
                grab_dx = cur_x - win_x;
                grab_dy = cur_y - win_y;
            }
        }
        if (dragging && (cur_x - grab_dx != win_x || cur_y - grab_dy != win_y))
            move_window(cur_x - grab_dx, cur_y - grab_dy);
        if (released && dragging) {
            dragging = false;
            print_xy("window moved to ", win_x, win_y);
        }
        cursor(true);
    }
}
