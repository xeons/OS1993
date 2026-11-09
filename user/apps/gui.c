/*
 * user/apps/gui.c -- GUI.BIN: the demo applications and the event loop.
 * The window manager and controls come from UI.DLL, drawing from GFX.DLL.
 *
 * Three windows show the window manager and the controls:
 *   Programs       MS-DOS Prompt (a DOS session VM), About, Exit to VMM
 *   Controls       every control type, live
 *   About OS1993   a small dialog
 * With a self-test flag it instead checks that ring 3 is fenced in.
 */
#include "os.h"
#include "theme.h"
#include "ui.h"
#include "wm.h"

#define SC_ESC 0x01

static bool quit;

/* ---- Programs ---------------------------------------------------------- */

enum { ID_DOS = 1, ID_ABOUT, ID_EXIT };

static struct ctl programs_ctls[] = {
    { .type = CT_LABEL,  .x = 12, .y = 10, .text = "Run a program:" },
    { .type = CT_BUTTON, .x = 12, .y = 36,  .w = 176, .h = BTN_H, .id = ID_DOS,   .text = "MS-DOS Prompt" },
    { .type = CT_BUTTON, .x = 12, .y = 66,  .w = 176, .h = BTN_H, .id = ID_ABOUT, .text = "About OS1993" },
    { .type = CT_BUTTON, .x = 12, .y = 96,  .w = 176, .h = BTN_H, .id = ID_EXIT,  .text = "Exit to VMM" },
};

static void open_about(void);

static void programs_command(struct window *w, int id)
{
    (void)w;
    switch (id) {
    case ID_DOS:
        ulog("button MS-DOS Prompt");
        sys_new_session();
        break;
    case ID_ABOUT:
        open_about();
        break;
    case ID_EXIT:
        ulog("button Exit to VMM");
        quit = true;
        break;
    }
}

/* ---- About -------------------------------------------------------------- */

static struct window *about_win;

static struct ctl about_ctls[] = {
    { .type = CT_LABEL,  .x = 16, .y = 12, .text = "OS1993 Virtual Machine Manager" },
    { .type = CT_LABEL,  .x = 16, .y = 32, .text = "A 386 enhanced mode VMM with" },
    { .type = CT_LABEL,  .x = 16, .y = 48, .text = "a ring-3 window manager." },
    { .type = CT_BUTTON, .x = 200, .y = 76, .w = 80, .h = BTN_H, .id = 1, .text = "OK", .flags = CF_DEFAULT },
};

static void about_command(struct window *w, int id)
{
    (void)id;
    ulog("button About OK");
    wm_close(w);
}

static void about_closed(struct window *w)
{
    (void)w;
    about_win = NULL;
}

static void open_about(void)
{
    if (about_win) {
        wm_activate(about_win);
        return;
    }
    about_win = wm_create("About OS1993", 40, 300, 310, 140, 0);
    if (!about_win)
        return;
    about_win->ctls = about_ctls;
    about_win->nctl = sizeof about_ctls / sizeof about_ctls[0];
    about_win->focus = 3;
    about_win->on_command = about_command;
    about_win->on_close = about_closed;
}

/* ---- Controls ----------------------------------------------------------- */

enum { ID_SOUND = 1, ID_CLOCK, ID_SLOW, ID_NORMAL, ID_TURBO, ID_NAME, ID_LIST,
       ID_VOLUME, ID_SCROLL, ID_OK, ID_CANCEL };

static char name_buf[32] = "Hello";
static const char *const colours[] = {
    "Platinum", "Graphite", "Lavender", "Teal", "Ocean", "Forest",
    "Sunset", "Sand", "Midnight", "Rose", "Slate", "Copper",
};
static struct ctl *progress;

static struct ctl controls_ctls[] = {
    { .type = CT_GROUP,  .x = 10,  .y = 6,   .w = 170, .h = 76,  .text = "Options" },
    { .type = CT_CHECK,  .x = 22,  .y = 28,  .w = 150, .h = 20,  .id = ID_SOUND, .text = "Sound effects" },
    { .type = CT_CHECK,  .x = 22,  .y = 52,  .w = 150, .h = 20,  .id = ID_CLOCK, .text = "Show clock", .value = 1 },
    { .type = CT_GROUP,  .x = 190, .y = 6,   .w = 170, .h = 100, .text = "Speed" },
    { .type = CT_RADIO,  .x = 202, .y = 28,  .w = 150, .h = 20,  .id = ID_SLOW,   .text = "Slow",   .group = 1 },
    { .type = CT_RADIO,  .x = 202, .y = 52,  .w = 150, .h = 20,  .id = ID_NORMAL, .text = "Normal", .group = 1, .value = 1 },
    { .type = CT_RADIO,  .x = 202, .y = 76,  .w = 150, .h = 20,  .id = ID_TURBO,  .text = "Turbo",  .group = 1 },
    { .type = CT_LABEL,  .x = 12,  .y = 96,  .text = "Name:" },
    { .type = CT_EDIT,   .x = 64,  .y = 92,  .w = 116, .h = EDIT_H, .id = ID_NAME, .buf = name_buf, .buflen = 32, .caret = 5 },
    { .type = CT_LABEL,  .x = 12,  .y = 124, .text = "Colour scheme:" },
    { .type = CT_LIST,   .x = 12,  .y = 144, .w = 168, .h = 100, .id = ID_LIST,
      .items = colours, .nitems = 12 },
    { .type = CT_LABEL,  .x = 192, .y = 124, .text = "Volume:" },
    { .type = CT_SLIDER, .x = 192, .y = 146, .w = 166, .h = 22,  .id = ID_VOLUME, .min = 0, .max = 100, .value = 40 },
    { .type = CT_LABEL,  .x = 192, .y = 180, .text = "Progress:" },
    { .type = CT_PROGRESS, .x = 192, .y = 200, .w = 166, .h = 16, .min = 0, .max = 100, .value = 0 },
    { .type = CT_HSCROLL, .x = 192, .y = 228, .w = 166, .h = SCROLL_W, .id = ID_SCROLL,
      .min = 0, .max = 50, .page = 10, .value = 10 },
    { .type = CT_BUTTON, .x = 196, .y = 262, .w = 76,  .h = BTN_H, .id = ID_OK, .text = "OK", .flags = CF_DEFAULT },
    { .type = CT_BUTTON, .x = 282, .y = 262, .w = 76,  .h = BTN_H, .id = ID_CANCEL, .text = "Cancel" },
};

static struct ctl *find(struct window *w, int id)
{
    for (int i = 0; i < w->nctl; i++)
        if (w->ctls[i].id == id && w->ctls[i].type != CT_LABEL && w->ctls[i].type != CT_GROUP)
            return &w->ctls[i];
    return NULL;
}

static void controls_command(struct window *w, int id)
{
    struct ctl *c = find(w, id);
    switch (id) {
    case ID_SOUND: case ID_CLOCK:
        ulog("check %s %s", c->text, c->value ? "on" : "off");
        break;
    case ID_SLOW: case ID_NORMAL: case ID_TURBO:
        ulog("radio %s", c->text);
        break;
    case ID_NAME:
        ulog("edit %s", c->buf);
        break;
    case ID_LIST:
        ulog("list %s", colours[c->value]);
        break;
    case ID_VOLUME:
        ulog("slider %d", c->value);
        break;
    case ID_SCROLL:
        ulog("scroll %d", c->value);
        break;
    case ID_OK: {
        const char *speed = find(w, ID_SLOW)->value ? "Slow" : find(w, ID_TURBO)->value ? "Turbo" : "Normal";
        ulog("OK: sound=%d clock=%d speed=%s name=%s colour=%s volume=%d",
             find(w, ID_SOUND)->value, find(w, ID_CLOCK)->value, speed, name_buf,
             colours[find(w, ID_LIST)->value], find(w, ID_VOLUME)->value);
        break;
    }
    case ID_CANCEL:
        ulog("button Cancel");
        break;
    }
}

/* ---- self-test ------------------------------------------------------------ */

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

/* ---- main ------------------------------------------------------------------- */

int main(struct user_info *info)
{
    if (info->flags & (UINFO_TEST_CLI | UINFO_TEST_KERNEL))
        return selftest(info);

    ui_init(info);

    struct window *p = wm_create("Programs", 16, 16, 210, 160, 0);
    p->ctls = programs_ctls;
    p->nctl = sizeof programs_ctls / sizeof programs_ctls[0];
    p->focus = 1;
    p->on_command = programs_command;

    open_about();

    struct window *c = wm_create("Controls", 246, 40, 380, 330, WF_RESIZABLE);
    c->ctls = controls_ctls;
    c->nctl = sizeof controls_ctls / sizeof controls_ctls[0];
    c->focus = 8;                                           /* the Name field */
    c->on_command = controls_command;
    for (int i = 0; i < c->nctl; i++)
        if (controls_ctls[i].type == CT_PROGRESS)
            progress = &controls_ctls[i];

    wm_flush();
    sys_print("desktop ready");
    if (sys_mouse())
        sys_print("mouse ready");

    uint32_t last = sys_ticks();
    while (!quit) {
        struct user_event e;
        if (sys_get_event(&e)) {
            if (e.type == EV_KEY && e.key == SC_ESC)
                break;
            if (e.type == EV_FOCUS)
                wm_screen(e.key != 0);
            else if (e.type == EV_KEY)
                wm_key(e.key);
            else
                wm_mouse(e.x, e.y, e.buttons, sys_ticks());
            continue;
        }
        uint32_t now = sys_ticks();
        if (now != last && progress) {                      /* animate the progress bar */
            last = now;
            progress->value = (progress->value + 1) % 101;
            if (c->used && c->state != WS_MIN)
                ui_redraw(c, progress);
            wm_flush();
        }
        sys_wait_tick();
    }
    return 0;
}
