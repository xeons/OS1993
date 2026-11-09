/*
 * accel.c -- keyboard accelerator tables, as in Windows.
 *
 * A table maps keys (a virtual key with Shift/Ctrl/Alt, or a character)
 * to command ids. TranslateAccelerator, called in the message loop before
 * TranslateMessage, sends WM_COMMAND with HIWORD(wParam) = 1 to the given
 * window when a key matches -- unless the command is a greyed item of
 * that window's menu, which makes the shortcut unavailable too.
 */
#include "uipriv.h"

#define TABLE_MAX   8
#define ACCEL_MAX   32
#define ACCEL_MAGIC 0x4C434341u     /* "ACCL" */

struct accel_table {
    uint32_t magic;
    int      n;
    ACCEL    a[ACCEL_MAX];
};

static struct accel_table tables[TABLE_MAX];

static struct accel_table *table_from(HACCEL h)
{
    uintptr_t p = (uintptr_t)h, base = (uintptr_t)tables;
    if (p < base || p >= base + sizeof tables || (p - base) % sizeof(struct accel_table))
        return NULL;
    struct accel_table *t = (struct accel_table *)h;
    return t->magic == ACCEL_MAGIC ? t : NULL;
}

HACCEL CreateAcceleratorTable(const ACCEL *accels, int count)
{
    if (!accels || count <= 0 || count > ACCEL_MAX)
        return NULL;
    for (int i = 0; i < TABLE_MAX; i++)
        if (tables[i].magic != ACCEL_MAGIC) {
            tables[i].magic = ACCEL_MAGIC;
            tables[i].n = count;
            memcpy(tables[i].a, accels, (size_t)count * sizeof(ACCEL));
            return &tables[i];
        }
    return NULL;
}

bool DestroyAcceleratorTable(HACCEL h)
{
    struct accel_table *t = table_from(h);
    if (!t)
        return false;
    t->magic = 0;
    return true;
}

int TranslateAccelerator(HWND hwnd, HACCEL h, MSG *m)
{
    struct accel_table *t = table_from(h);
    struct wnd *w = wnd_from(hwnd);
    if (!t || !w || !m)
        return 0;
    bool virt = m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN;
    if (!virt && m->message != WM_CHAR)
        return 0;
    unsigned mods = ((m->lParam & (1 << 25)) ? FSHIFT : 0) |
                    ((m->lParam & (1 << 27)) ? FCONTROL : 0) |
                    ((m->lParam & (1 << 29)) ? FALT : 0);
    for (int i = 0; i < t->n; i++) {
        const ACCEL *a = &t->a[i];
        bool match = virt ? (a->fVirt & FVIRTKEY) && a->key == m->wParam &&
                            (a->fVirt & (FSHIFT | FCONTROL | FALT)) == mods
                          : !(a->fVirt & FVIRTKEY) && a->key == m->wParam;
        if (!match)
            continue;
        HMENU menu = GetMenu(hwnd);
        if (menu) {                                 /* greyed in the menu: unavailable */
            int state = EnableMenuItem(menu, a->cmd, MF_BYCOMMAND);
            if (state >= 0)
                EnableMenuItem(menu, a->cmd, (unsigned)state);
            if (state > 0)
                return 1;
        }
        SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(a->cmd, 1), 0);
        return 1;
    }
    return 0;
}
