/*
 * uipriv.h -- UI.DLL internals: windows, classes, the screen, metrics, and
 * the shared arena.
 *
 * Several processes use the window system at once. Like Win95's USER, it
 * keeps its state in memory every process sees at the same address (the
 * shared arena, struct ui_shared), guarded by one lock: every UI.DLL export
 * takes it on entry (a HOOK in ui.def), and a process lets go of it only
 * when it waits -- in GetMessage, a modal loop, Sleep, or a cross-process
 * SendMessage. Pointers into the arena (HWNDs, HMENUs) mean the same in
 * every process; window procedures don't, so a window's messages are always
 * handled by the process that created it (w->pid).
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
#define MENUBAR_H   20

#define WND_MAX     128
#define CLASS_MAX   32
#define PROP_MAX    4
#define EXTRA_MAX   32
#define TEXT_MAX    64
#define QMAX        64          /* posted messages per process */
#define SEND_MAX    8           /* cross-process SendMessage in flight */
#define TIMER_MAX   32
#define MENU_MAX    48
#define MITEM_MAX   16
#define MITEM_TEXT  32
#define MENU_DEPTH  4
#define COPYDATA_MAX 1024

enum wstate { WS_STATE_NORMAL, WS_STATE_MIN, WS_STATE_MAX };

struct wclass {
    char     name[24];
    int      pid;               /* registering process; 0: built in, everyone's */
    WNDPROC  proc;
    unsigned style;
    int      extra;
    uint8_t  background;
    bool     transparent;       /* mouse goes through (STATIC, group boxes) */
    bool     noactivate;        /* top level that never activates (menus) */
    int      icon;
};

struct wnd {
    uint32_t       magic;
    int            pid;                      /* the process that handles it */
    struct wclass *cls;
    WNDPROC        proc;
    struct wnd    *parent, *first, *next;    /* children: first = bottom */
    struct wnd    *owner;                    /* top level: owning window */
    unsigned       style, exstyle;
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
    struct menu   *menu, *sysmenu;           /* menu bar, system menu */
    int            bar_sel;                  /* highlighted menu bar item, -1 */
    bool           visible, enabled;
    bool           has_upd;                  /* top level: needs painting */
    RECT           upd;                      /* ... this (screen coordinates) */

    /* State for the built-in control classes */
    int            value, min, max, page, top, caret;
    bool           pressed, tracking;
    int            part, grab;
};

#define WND_MAGIC 0x21444E57u   /* "WND!" */

/* Menus (menu.c) */
struct mitem {
    unsigned     flags;
    unsigned     id;
    struct menu *sub;
    char         text[MITEM_TEXT];
};

struct menu {
    uint32_t     magic;
    int          pid;
    bool         bar;
    bool         system;
    int          n;
    struct mitem items[MITEM_MAX];
};

struct menu_level {
    struct menu *m;
    struct wnd  *w;
    int          sel;           /* highlighted item, -1 none */
};

struct menu_track {             /* the open menu (one at a time) */
    struct wnd  *owner;
    bool         bar;           /* tracking the owner's menu bar */
    bool         system;        /* the root is the system menu */
    bool         sysclick;      /* opened by a click on the system box */
    uint32_t     opened;
    struct menu_level lv[MENU_DEPTH];
    int          n;
    bool         done, chosen;
    unsigned     result;
};

/* A process's message queue */
struct pqueue {
    int      pid;               /* 0: free */
    MSG      q[QMAX];
    unsigned head, tail;
    bool     quit;
    int      quit_code;
};

/* A SendMessage to another process's window, waiting for its reply */
struct sent {
    int      from, to;          /* pids; from == 0: free */
    bool     taken, done;
    HWND     hwnd;
    unsigned msg;
    WPARAM   wp;
    LPARAM   lp;
    LRESULT  result;
    COPYDATASTRUCT cds;         /* WM_COPYDATA: the receiver's copy */
    uint8_t  data[COPYDATA_MAX];
};

struct timer { struct wnd *w; int id; uint32_t interval, due; };

/* Mouse and keyboard state, and the cursor */
struct ui_input {
    bool        shift, ctrl, alt, alt_alone;
    int         cur_x, cur_y;
    bool        cur_on;
    uint8_t     last_buttons;
    uint32_t    last_click;
    struct wnd *last_click_wnd;
    int         last_click_part;
    RECT        outline;                     /* XOR drag outline ... */
    bool        outline_on;                  /* ... drawn on the screen */
    int         modal_pid;                   /* raw input reserved for it */
    bool        modal_running;               /* ... which is in a modal loop */
};

#define UI_MAGIC 0x31495553u    /* "SUI1" */

struct ui_shared {
    uint32_t       magic;
    struct wclass  classes[CLASS_MAX];
    int            nclasses;
    struct wnd     wnds[WND_MAX];
    struct wnd    *tops[WND_MAX];            /* top-level z-order, bottom first */
    int            ntops;
    struct wnd    *active, *focus, *capture;
    struct pqueue  queues[PROC_MAX];
    struct sent    sends[SEND_MAX];
    struct timer   timers[TIMER_MAX];
    struct menu    menus[MENU_MAX];
    struct menu_track mt;
    struct ui_input in;
    int            frame_pressed_box;        /* HT_* while being clicked */
    struct wnd    *frame_pressed_wnd;
    bool           background;               /* a DOS session has the screen */
    bool           desk_dirty;               /* uncovered desktop to paint */
    RECT           desk;
    uint32_t       exits_seen;               /* GetProcessExitCount at last cleanup */
    struct wnd    *shell_hook;               /* RegisterShellHookWindow */
    struct wnd    *desktop;                  /* SetShellWindow: kept at the bottom */
    RECT           work;                     /* SPI_SETWORKAREA */
    uint8_t        back[4 * GFX_PLANE_BYTES];/* the off-screen copy of the screen */
};

#define S ((struct ui_shared *)USER_ARENA)

/* Per-process (UI.DLL's own data is private to each process) */
extern int ui_pid;
extern struct gfx ui_gfx;                    /* drawing into S->back */
extern struct gfx ui_screen;                 /* the real screen */
extern const uint8_t theme_palette[16][3];
void  ui_enter(void);                        /* take the lock (export hook) */
void  ui_leave(void);                        /* let go of it (before waiting) */
void  ui_wait(uint32_t ms, bool input);      /* leave, wait, enter */

/* wnd.c */
struct wnd *wnd_from(HWND h);                /* validated, or NULL */
struct wnd *wnd_top(struct wnd *w);          /* its top-level window */
int         wnd_toplevels(struct wnd **out); /* bottom to top */
void        wnd_screen_rect(struct wnd *w, RECT *r);         /* outer, screen */
void        wnd_client_screen(struct wnd *w, int *x, int *y); /* client origin */
struct wnd *wnd_child_at(struct wnd *w, int x, int y);      /* x,y in w's client */
void        wnd_raise(struct wnd *top);
void        wnd_activate(struct wnd *top);
void        wnd_activate_next(struct wnd *gone);   /* after gone hides or goes */
struct wnd *wnd_active(void);
struct wnd *wnd_owned_popup(struct wnd *owner);   /* an enabled window it owns */
bool        wnd_mine(struct wnd *w);         /* handled by this process */
LRESULT     wnd_notify(struct wnd *w, unsigned msg, WPARAM wp, LPARAM lp);  /* send if mine, else post */
void        ui_post_mouse(struct wnd *w, unsigned msg, int sx, int sy, unsigned keys);
void        ui_init_classes(void);
void        ui_shell_notify(int code, struct wnd *w);   /* shell hook messages */
void        ui_cleanup_dead(void);           /* windows of processes that ended */
void        ui_forget_window(struct wnd *w); /* remove without messages */
void        ui_reserve_input(int pid);       /* raw input for one process */

/* frame.c -- the non-client area of top-level windows */
enum { HT_NOWHERE, HT_CLIENT, HT_CAPTION, HT_SYSMENU, HT_MINBUTTON, HT_MAXBUTTON,
       HT_CLOSE, HT_TILE, HT_BORDER, HT_MENU };
#define E_L 1
#define E_R 2
#define E_T 4
#define E_B 8
bool  frame_has(struct wnd *w);              /* top level with a caption */
bool  frame_shown(struct wnd *w);            /* on screen (a hidden minimized one isn't) */
void  frame_outer(struct wnd *w, RECT *r);   /* tile while minimized */
void  frame_client(struct wnd *w, RECT *r);  /* client rect, screen coords */
int   frame_hit(struct wnd *w, int x, int y, int *edges);
RECT  frame_box(struct wnd *w, int which);
void  frame_paint(struct wnd *w, bool active);
void  frame_paint_grip(struct wnd *w);
void  frame_minimize(struct wnd *w);
void  frame_maximize(struct wnd *w);
void  frame_restore(struct wnd *w);
bool  frame_menubar(struct wnd *w, RECT *r);  /* its menu bar, screen coords */

/* paint.c */
void  ui_invalidate_screen(int x, int y, int w, int h);
void  ui_invalidate_wnd(struct wnd *w);      /* whole window, frame included */
bool  ui_paint_pending(void);
void  ui_paint(void);                        /* this process's windows, the desktop */
void  ui_paint_now(struct wnd *w);           /* w at once (before XOR drawing) */
void  ui_set_background(bool background);
void  ui_screen_init(void);
void  ui_present(int x0, int y0, int x1, int y1);  /* back buffer -> screen */

/* input.c */
void  ui_pump_input(void);                   /* raw events -> messages */
void  ui_input_reset(void);
void  ui_cursor(bool on);
bool  ui_cursor_overlaps(int x0, int y0, int x1, int y1);
void  ui_outline(bool on);                   /* the XOR drag outline */
void  ui_track_frame(struct wnd *w, int part, int x, int y);   /* WM_NCLBUTTONDOWN */

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

/* input.c -- modal loops (menus, moving and sizing) read input directly */
struct modal_ev {
    int  type;                               /* EV_KEY, EV_MOUSE, EV_FOCUS */
    int  x, y;
    bool moved, ldown, lup, rdown, rup;
    int  vk;
    bool up;
};
void  ui_modal_begin(void);
void  ui_modal_end(void);
void  ui_modal_event(struct modal_ev *m);    /* waits; paints meanwhile */
void  ui_keyboard_track(struct wnd *w, bool size);   /* SC_MOVE / SC_SIZE */

/* menu.c, msgbox.c */
LRESULT menu_popup_proc(HWND, unsigned, WPARAM, LPARAM);
LRESULT msgbox_proc(HWND, unsigned, WPARAM, LPARAM);
void  menubar_paint(struct wnd *w);
void  menu_track_bar(struct wnd *w, int x, int y);   /* click on the bar */
void  menu_track_sys(struct wnd *w);                 /* click on the system box */
void  menu_keymenu(struct wnd *w, int ch);           /* SC_KEYMENU */
void  menu_window_gone(struct wnd *w);
void  menu_process_gone(int pid);

/* kbd.c */
int   kbd_vk(int scancode);                  /* scancode -> VK_* (0: none) */
char  kbd_char(int vk, bool shift);          /* 0 if not a character */

/* small shared things */
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline bool rect_contains(const RECT *r, int x, int y)
{
    return x >= r->left && y >= r->top && x < r->right && y < r->bottom;
}
static inline bool rect_intersect(RECT a, RECT b, RECT *out)
{
    out->left = a.left > b.left ? a.left : b.left;
    out->top = a.top > b.top ? a.top : b.top;
    out->right = a.right < b.right ? a.right : b.right;
    out->bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
    return out->left < out->right && out->top < out->bottom;
}
static inline void rect_union(RECT *a, RECT b)
{
    if (b.left < a->left) a->left = b.left;
    if (b.top < a->top) a->top = b.top;
    if (b.right > a->right) a->right = b.right;
    if (b.bottom > a->bottom) a->bottom = b.bottom;
}

#endif
