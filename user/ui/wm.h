/*
 * wm.h -- the window manager (ring 3).
 *
 * Overlapping windows in a z-order, painted back to front into a dirty
 * rectangle. Windows 95 behaviour (system box, minimize, maximize/restore,
 * close, resizable borders and size grip, double-click to maximize,
 * Alt+F4, Tab between controls) with Platinum-inspired drawing.
 */
#ifndef OS1993_WM_H
#define OS1993_WM_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

#define WIN_MAX 12

#define WF_RESIZABLE  0x01
#define WF_DOCUMENT   0x02          /* white client area (else control face) */

enum wstate { WS_NORMAL, WS_MIN, WS_MAX };

struct ctl;

struct window {
    bool        used;
    int         x, y, w, h;         /* outer rectangle when normal/maximized */
    int         rx, ry, rw, rh;     /* rectangle to restore to */
    char        title[32];
    unsigned    flags;
    enum wstate state;
    int         min_slot;           /* tile position while minimized */
    struct ctl *ctls;
    int         nctl;
    int         focus;              /* focused control, or -1 */
    void      (*on_command)(struct window *w, int id);
    void      (*on_close)(struct window *w);
};

extern struct gfx wm_gfx;

void wm_init(void);
struct window *wm_create(const char *title, int x, int y, int w, int h, unsigned flags);
void wm_close(struct window *w);
void wm_activate(struct window *w);
struct window *wm_active(void);

void wm_client_rect(const struct window *w, int *x, int *y, int *cw, int *ch);
void wm_invalidate(int x, int y, int w, int h);
void wm_invalidate_window(const struct window *w);
void wm_flush(void);                /* repaint the dirty rectangle */
void wm_screen(bool ours);          /* EV_FOCUS: stop drawing / repaint all */

void wm_mouse(int x, int y, uint8_t buttons, uint32_t tick);
void wm_key(int scancode);

#endif
