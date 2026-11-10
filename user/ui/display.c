/*
 * display.c -- display modes for programs: EnumDisplaySettings and
 * ChangeDisplaySettings (as in Win32), GetSystemMetrics.
 *
 * A mode change is made by one process while it holds the window manager
 * lock; every other process notices the new mode (S->mode_gen) the next
 * time it takes the lock, before it can draw, and re-creates its view of
 * the screen and the back buffer. Every top-level window gets
 * WM_DISPLAYCHANGE; maximized ones are fitted to the new screen and the
 * others kept on it.
 */
#include "uipriv.h"

static uint32_t my_gen;                 /* the mode this process draws in */

void ui_screen_init(void)
{
    const struct user_info *info = GetStartupInfo();
    struct display_info d;
    GetDisplayInfo(&d);
    const uint8_t *font = (const uint8_t *)info->font;
    if (d.bpp == 4) {
        gfx_init(&ui_screen, (volatile uint8_t *)d.fb, font);
        gfx_init_mem(&ui_gfx, (uint8_t *)d.back, font);
    } else {
        gfx_init_linear(&ui_screen, (uint8_t *)d.fb, d.width, d.height, d.bpp, d.pitch, font);
        gfx_init_linear(&ui_gfx, (uint8_t *)d.back, d.width, d.height, d.bpp, d.back_pitch, font);
    }
    gfx_set_colors(&ui_screen, theme_palette, d.red_pos, d.red_size, d.green_pos, d.green_size,
                   d.blue_pos, d.blue_size);
    gfx_set_colors(&ui_gfx, theme_palette, d.red_pos, d.red_size, d.green_pos, d.green_size,
                   d.blue_pos, d.blue_size);
    my_gen = d.gen;
}

/* Taking the lock: catch up with a mode another process switched to. */
void ui_sync_display(void)
{
    if (ui_pid && S->magic == UI_MAGIC && S->mode_gen != my_gen)
        ui_screen_init();
}

int GetSystemMetrics(int index)
{
    switch (index) {
    case SM_CXSCREEN: return S->scr_w;
    case SM_CYSCREEN: return S->scr_h;
    }
    return 0;
}

bool EnumDisplaySettings(int index, DEVMODE *dm)
{
    if (!dm)
        return false;
    if (index == ENUM_CURRENT_SETTINGS) {
        struct display_info d;
        GetDisplayInfo(&d);
        *dm = (DEVMODE){ d.width, d.height, d.bpp };
        return true;
    }
    struct display_mode list[16];
    int n = GetDisplayModes(list, 16);
    if (index < 0 || index >= n)
        return false;
    *dm = (DEVMODE){ list[index].width, list[index].height, list[index].bpp };
    return true;
}

int ChangeDisplaySettings(const DEVMODE *dm, unsigned flags)
{
    (void)flags;
    if (!dm)
        return DISP_CHANGE_BADMODE;
    DEVMODE now;
    EnumDisplaySettings(ENUM_CURRENT_SETTINGS, &now);
    if (now.dmPelsWidth == dm->dmPelsWidth && now.dmPelsHeight == dm->dmPelsHeight &&
        now.dmBitsPerPel == dm->dmBitsPerPel)
        return DISP_CHANGE_SUCCESSFUL;
    if (!SetDisplayMode((int)dm->dmPelsWidth, (int)dm->dmPelsHeight, (int)dm->dmBitsPerPel))
        return DISP_CHANGE_FAILED;

    struct display_info d;
    GetDisplayInfo(&d);
    int old_w = S->scr_w, old_h = S->scr_h;
    S->scr_w = d.width;
    S->scr_h = d.height;
    S->bpp = d.bpp;
    S->mode_gen = d.gen;
    ui_screen_init();
    if (d.bpp <= 8)
        gfx_set_palette(theme_palette);             /* the DAC holds the theme */
    if (S->work.right == old_w && S->work.left == 0)
        S->work.right = S->scr_w;                   /* the shell adjusts it (WM_DISPLAYCHANGE) */
    if (S->work.bottom >= old_h - 64)
        S->work.bottom = S->scr_h - (old_h - S->work.bottom);
    for (int i = 0; i < S->ntops; i++) {            /* keep windows on the screen */
        struct wnd *t = S->tops[i];
        if (t->state == WS_STATE_MAX) {
            t->x = S->work.left;
            t->y = S->work.top;
            t->w = S->work.right - S->work.left;
            t->h = S->work.bottom - S->work.top;
        } else if (t != S->desktop) {
            if (t->x > S->scr_w - 40) t->x = S->scr_w - 40 > 0 ? S->scr_w - 40 : 0;
            if (t->y > S->scr_h - 40) t->y = S->scr_h - 40 > 0 ? S->scr_h - 40 : 0;
        }
    }
    S->in.cur_x = clampi(S->in.cur_x, 0, S->scr_w - 1);
    S->in.cur_y = clampi(S->in.cur_y, 0, S->scr_h - 1);
    S->in.cur_on = false;                           /* the new screen has no cursor yet */
    S->in.outline_on = false;
    ui_invalidate_screen(0, 0, S->scr_w, S->scr_h);
    for (int i = 0; i < S->ntops; i++)              /* all of them, the taskbar too */
        PostMessage(S->tops[i], WM_DISPLAYCHANGE, (WPARAM)S->bpp, MAKELPARAM(S->scr_w, S->scr_h));
    DebugLog("display %ux%u, %u bits", d.width, d.height, d.bpp);
    return DISP_CHANGE_SUCCESSFUL;
}
