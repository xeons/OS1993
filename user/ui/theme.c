/* user/ui/theme.c -- the Platinum-inspired palette, and UI.DLL's start-up. */
#include "os.h"
#include "theme.h"
#include "ui.h"
#include "wm.h"

/* 6-bit DAC values */
const uint8_t theme_palette[16][3] = {
    [C_BLACK]     = {  0,  0,  0 },
    [C_GRAY1]     = { 13, 13, 13 },
    [C_GRAY2]     = { 21, 21, 21 },
    [C_GRAY3]     = { 30, 30, 30 },
    [C_GRAY4]     = { 38, 38, 38 },
    [C_GRAY5]     = { 46, 46, 46 },
    [C_FACE]      = { 55, 55, 55 },
    [C_LIGHT]     = { 59, 59, 59 },
    [C_WHITE]     = { 63, 63, 63 },
    [C_ACCENT]    = { 26, 26, 50 },
    [C_ACCENT_LT] = { 50, 50, 63 },
    [C_DESKTOP]   = { 17, 30, 40 },
    [C_RED]       = { 52,  8,  8 },
    [C_GREEN]     = {  8, 40, 12 },
    [C_YELLOW]    = { 63, 56, 16 },
    [C_ACCENT_DK] = { 16, 16, 40 },
};

/* Set up drawing for the program: its video memory and font, our palette,
 * an empty desktop. */
void ui_init(const struct user_info *info)
{
    gfx_init(&wm_gfx, (volatile uint8_t *)info->vga, (const uint8_t *)info->font);
    gfx_set_palette(theme_palette);
    wm_init();
}
