/*
 * ui.h -- controls: the Windows 95 set, drawn in a Platinum-inspired style.
 * Positions are relative to the window's client area.
 */
#ifndef OS1993_UI_H
#define OS1993_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "wm.h"

enum ctype {
    CT_LABEL, CT_BUTTON, CT_CHECK, CT_RADIO, CT_GROUP, CT_EDIT,
    CT_LIST, CT_VSCROLL, CT_HSCROLL, CT_SLIDER, CT_PROGRESS,
};

#define CF_DEFAULT  0x01            /* the default push button (Enter) */
#define CF_DISABLED 0x02

struct ctl {
    uint8_t     type;
    uint8_t     flags;
    int16_t     x, y, w, h;
    int         id;                 /* passed to on_command */
    const char *text;
    int         value;              /* check/radio state, list selection,
                                       scroll/slider/progress position */
    int         min, max, page;     /* ranges (scroll, slider, progress) */
    int         group;              /* radio group */
    char       *buf;                /* edit text */
    int         buflen;
    int         caret;
    const char *const *items;       /* list items */
    int         nitems;
    int         top;                /* first visible list item */
    bool        pressed;            /* being clicked */
};

void ui_draw(struct window *w, struct ctl *c);
void ui_draw_all(struct window *w);
void ui_redraw(struct window *w, struct ctl *c);    /* invalidate it */

/* Mouse, in screen coordinates. ui_mouse_down returns true if a control
 * took the click; it then gets moves and the release. */
bool ui_mouse_down(struct window *w, int x, int y);
void ui_mouse_move(int x, int y);
void ui_mouse_up(int x, int y);

bool ui_key(struct window *w, int scancode, char ch);
void ui_focus_next(struct window *w, int dir);

/* theme.c: call once, before creating windows */
struct user_info;
void ui_init(const struct user_info *info);

/* kbd.c */
char kbd_ascii(int scancode);       /* tracks Shift; 0 if not a character */
bool kbd_shift(void);

#endif
