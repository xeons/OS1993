/*
 * uipriv.h -- UI.DLL internals: windows, classes, the screen, metrics.
 */
#ifndef OS1993_UIPRIV_H
#define OS1993_UIPRIV_H

#include "os1993.h"
#include "gfx.h"

/* ---- metrics ---------------------------------------------------------- */

#define FRAME       5           /* frame border, resizable */
#define TITLE_H     19          /* title bar height inside the frame */
#define CLIENT_TOP  (FRAME + TITLE_H + 1)
#define BOX         13          /* title bar box */
#define TILE_W      160         /* minimized window tile */
#define TILE_H      (FRAME * 2 + TITLE_H)
#define SCROLL_W    16
#define CHECK       13
#define ITEM_H      16
#define MIN_W       160
#define MIN_H       100

#define WND_MAX     96
#define CLASS_MAX   24
#define PROP_MAX    4
#define EXTRA_MAX   32
#define TEXT_MAX    64

enum wstate { WS_STATE_NORMAL, WS_STATE_MIN, WS_STATE_MAX };

struct wclass {
    char     name[24];
    WNDPROC  proc;
    unsigned style;
    int      extra;
    uint8_t  background;
    bool     transparent;       /* mouse goes through (STATIC, group boxes) */
};

struct wnd {
    uint32_t       magic;
    struct wclass *cls;
    WNDPROC        proc;
    struct wnd    *parent, *first, *next;    /* children: first = bottom */
    unsigned       style;
    int            id;
    int            x, y, w, h;               /* parent client coords (top level: screen) */
    int            rx, ry, rw, rh;           /* restore rectangle */
    enum wstate    state;
    int            min_slot;
    char           text[TEXT_MAX];
    int32_t        userdata;
    uint8_t        extra[EXTRA_MAX];
    struct { char name[16]; void *value; } props[PROP_MAX];
    struct wnd    *focus_child;              /* top level: focus to restore */
    bool           visible, enabled;

    /* State for the built-in control classes */
    int            value, min, max, page, top, caret;
    bool           pressed, tracking;
    int            part, grab;
};

#define WND_MAGIC 0x21444E57u   /* "WND!" */

extern struct gfx ui_gfx;                    /* the off-screen buffer */
extern const uint8_t theme_palette[16][3];

/* wnd.c */
struct wnd *wnd_from(HWND h);                /* validated, or NULL */
struct wnd *wnd_top(struct wnd *w);          /* its top-level window */
int         wnd_toplevels(struct wnd **out); /* bottom to top */
void        wnd_screen_rect(struct wnd *w, RECT *r);         /* outer, screen */
void        wnd_client_screen(struct wnd *w, int *x, int *y); /* client origin */
struct wnd *wnd_child_at(struct wnd *w, int x, int y);      /* x,y in w's client */
void        wnd_raise(struct wnd *top);
void        wnd_activate(struct wnd *top);
struct wnd *wnd_active(void);
void        ui_post_mouse(struct wnd *w, unsigned msg, int sx, int sy, unsigned keys);
void        ui_init_classes(void);

/* frame.c -- the non-client area of top-level windows */
enum { HT_NOWHERE, HT_CLIENT, HT_CAPTION, HT_SYSMENU, HT_MINBUTTON, HT_MAXBUTTON,
       HT_CLOSE, HT_TILE, HT_BORDER };
#define E_L 1
#define E_R 2
#define E_T 4
#define E_B 8
bool  frame_has(struct wnd *w);              /* top level with a caption */
void  frame_outer(struct wnd *w, RECT *r);   /* tile while minimized */
void  frame_client(struct wnd *w, RECT *r);  /* client rect, screen coords */
int   frame_hit(struct wnd *w, int x, int y, int *edges);
RECT  frame_box(struct wnd *w, int which);
void  frame_paint(struct wnd *w, bool active);
void  frame_paint_grip(struct wnd *w);
void  frame_minimize(struct wnd *w);
void  frame_maximize(struct wnd *w);
void  frame_restore(struct wnd *w);
extern int  frame_pressed_box;               /* HT_* while being clicked */
extern struct wnd *frame_pressed_wnd;

/* paint.c */
void  ui_invalidate_screen(int x, int y, int w, int h);
void  ui_invalidate_wnd(struct wnd *w);      /* whole window, frame included */
bool  ui_paint_pending(void);
void  ui_paint(void);                        /* repaint the dirty rectangle */
void  ui_set_background(bool background);
void  ui_screen_init(void);

/* input.c */
void  ui_input_event(const struct user_event *e);
void  ui_cursor(bool on);
bool  ui_cursor_overlaps(int x0, int y0, int x1, int y1);
extern struct gfx ui_screen;                 /* the real screen */
extern bool ui_background;

/* drawing helpers for controls (paint.c) */
struct dc {
    struct wnd *w;
    int ox, oy;                              /* client origin on screen */
    int cx0, cy0, cx1, cy1;                  /* clip, screen coords */
};
void  dc_apply(HDC dc);                      /* set ui_gfx's clip */

/* controls (one file each) */
bool  ui_has_focus(struct wnd *w);            /* focus, in the active window */
LRESULT button_proc(HWND, unsigned, WPARAM, LPARAM);
LRESULT static_proc(HWND, unsigned, WPARAM, LPARAM);
LRESULT edit_proc(HWND, unsigned, WPARAM, LPARAM);
LRESULT listbox_proc(HWND, unsigned, WPARAM, LPARAM);
LRESULT scrollbar_proc(HWND, unsigned, WPARAM, LPARAM);
LRESULT trackbar_proc(HWND, unsigned, WPARAM, LPARAM);
LRESULT progress_proc(HWND, unsigned, WPARAM, LPARAM);

/* scroll bar geometry, shared with list boxes (scroll.c) */
struct sbar { bool vert; int x, y, w, h; int value, min, max, page; };
enum { SB_PART_NONE, SB_PART_DEC, SB_PART_INC, SB_PART_PGDEC, SB_PART_PGINC, SB_PART_THUMB };
void  sbar_draw(HDC dc, const struct sbar *s, int pressed_part);
int   sbar_hit(const struct sbar *s, int x, int y);
int   sbar_thumb_value(const struct sbar *s, int along, int grab);
void  sbar_thumb(const struct sbar *s, int *pos, int *len, int *track, int *tlen);

/* kbd.c */
int   kbd_vk(int scancode);                  /* scancode -> VK_* (0: none) */
char  kbd_char(int vk, bool shift);          /* 0 if not a character */

/* small shared things */
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline bool rect_contains(const RECT *r, int x, int y)
{
    return x >= r->left && y >= r->top && x < r->right && y < r->bottom;
}

#endif
