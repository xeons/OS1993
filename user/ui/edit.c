/*
 * edit.c -- the EDIT class: a single-line text field. The outer 3 pixels
 * hold the accent focus ring; inside is an inset white field with a caret.
 *
 * The selection runs from an anchor (w->value) to the caret; the mouse
 * drags it out, Shift moves the caret while the anchor stays, a double
 * click takes a word. The text scrolls sideways so the caret is always in
 * view (w->top = the first character shown). Cut, copy and paste take the
 * selection through the clipboard; Ctrl+Z undoes the last change (and the
 * undo). The caret blinks (w->pressed: in its off half) while the field
 * has the focus. Changes are reported to the parent as WM_COMMAND EN_CHANGE.
 */
#include "uipriv.h"

#define RING 3
#define BLINK_ID 0x7E01
#define BLINK_MS 530

struct sedit {                          /* w->ctl: what doesn't fit in struct wnd */
    char undo[TEXT_MAX];
    int  undo_caret;
    bool undo_ok, typing;               /* typing: a run of characters is one undo step */
};

static int sel_lo(struct wnd *w) { return w->value < w->caret ? w->value : w->caret; }
static int sel_hi(struct wnd *w) { return w->value > w->caret ? w->value : w->caret; }
static bool has_sel(struct wnd *w) { return w->value != w->caret; }
static int limit(struct wnd *w) { return w->max > 0 && w->max < TEXT_MAX ? w->max : TEXT_MAX - 1; }
static int room(struct wnd *w) { int r = (w->w - 2 * RING - 10) / 8; return r > 1 ? r : 1; }

static void show_caret(struct wnd *w)
{
    int len = (int)strlen(w->text), r = room(w);
    if (w->caret < w->top)
        w->top = w->caret > 4 ? w->caret - 4 : 0;   /* a few characters of context */
    else if (w->caret > w->top + r)
        w->top = w->caret - r;
    if (w->top > 0 && len - w->top < r)             /* no empty space at the end */
        w->top = len - r > 0 ? len - r : 0;
}

static void changed(struct wnd *w)
{
    show_caret(w);
    InvalidateRect(w, NULL);
    if (w->parent)
        SendMessage(w->parent, WM_COMMAND, MAKEWPARAM(w->id, EN_CHANGE), (LPARAM)w);
}

static void save_undo(struct wnd *w, bool typing)
{
    struct sedit *e = w->ctl;
    if (!e || (typing && e->typing))
        return;
    strncpy0(e->undo, w->text, sizeof e->undo);
    e->undo_caret = w->caret;
    e->undo_ok = true;
    e->typing = typing;
}

/* Replace the selection with s (n characters); false if nothing changed. */
static bool replace_sel(struct wnd *w, const char *s, int n, bool typing)
{
    int len = (int)strlen(w->text), lo = sel_lo(w), hi = sel_hi(w);
    int keep = len - (hi - lo);
    if (n > limit(w) - keep)
        n = limit(w) - keep;
    if (n < 0) n = 0;
    if (n == 0 && lo == hi)
        return false;
    save_undo(w, typing);
    memmove(w->text + lo + n, w->text + hi, (size_t)(len - hi + 1));
    memcpy(w->text + lo, s, (size_t)n);
    w->caret = w->value = lo + n;
    changed(w);
    return true;
}

static bool is_word(char c) { return c != ' ' && c != '\t'; }

static int word_left(struct wnd *w, int i)
{
    while (i > 0 && !is_word(w->text[i - 1])) i--;
    while (i > 0 && is_word(w->text[i - 1])) i--;
    return i;
}

static int word_right(struct wnd *w, int i)
{
    int len = (int)strlen(w->text);
    while (i < len && is_word(w->text[i])) i++;
    while (i < len && !is_word(w->text[i])) i++;
    return i;
}

static int char_at(struct wnd *w, int x)
{
    int len = (int)strlen(w->text);
    return clampi(w->top + (x - RING - 5 + 4) / 8, 0, len);
}

static void move_to(struct wnd *w, int pos, bool extend)
{
    w->caret = pos;
    if (!extend)
        w->value = pos;
    if (w->ctl) ((struct sedit *)w->ctl)->typing = false;
    show_caret(w);
    InvalidateRect(w, NULL);
}

LRESULT edit_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    if (w->style & ES_MULTILINE)
        return medit_proc(h, msg, wp, lp);
    int len = (int)strlen(w->text);
    struct sedit *e = w->ctl;
    if ((msg == WM_KEYDOWN || msg == WM_CHAR || msg == WM_LBUTTONDOWN) && ui_has_focus(w)) {
        w->pressed = false;                     /* the caret shows at once while working */
        SetTimer(h, BLINK_ID, BLINK_MS);
    }
    switch (msg) {
    case WM_CREATE:
        w->ctl = LocalAlloc(LMEM_ZEROINIT, sizeof(struct sedit));
        w->caret = w->value = len;
        w->top = 0;
        w->max = 0;
        show_caret(w);
        return 0;
    case WM_DESTROY:
        if (w->ctl) LocalFree(w->ctl);
        w->ctl = NULL;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        bool focus = ui_has_focus(w);
        if (focus) {
            RECT a = { 0, 0, w->w, w->h }, b = { 1, 1, w->w - 1, w->h - 1 };
            FrameRect(dc, &a, C_ACCENT);
            FrameRect(dc, &b, C_ACCENT);
        }
        int x = RING, y = RING, fw = w->w - 2 * RING, fh = w->h - 2 * RING;
        RECT s = { x, y, x + fw, y + fh }, k = { x + 1, y + 1, x + fw - 1, y + fh - 1 },
             in = { x + 2, y + 2, x + fw - 2, y + fh - 2 };
        DrawBevel(dc, &s, false);
        FrameRect(dc, &k, C_GRAY1);
        uint8_t bg = w->enabled && !(w->style & ES_READONLY) ? C_WHITE : C_FACE;
        uint8_t fg = w->enabled ? C_BLACK : C_GRAY3;
        ctl_color(w, dc, (w->style & ES_READONLY) || !w->enabled ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT,
                  &fg, &bg);
        FillRect(dc, &in, bg);
        RECT clip, saved;
        clip = in;
        dc_clip(dc, clip, &saved);
        int ty = y + (fh - 16) / 2, tx = x + 5;
        char shown[TEXT_MAX];
        for (int i = 0; w->text[w->top + i]; i++)                   /* passwords as stars */
            shown[i] = (w->style & ES_PASSWORD) ? '*' : w->text[w->top + i];
        shown[len - w->top] = '\0';
        TextOut(dc, tx, ty, shown, fg, -1);
        int lo = sel_lo(w), hi = sel_hi(w);
        if (lo < hi && (focus || (w->style & ES_NOHIDESEL))) {
            int a = lo > w->top ? lo - w->top : 0, z = hi - w->top;
            if (z > a) {
                RECT sel = { tx + a * 8, ty, tx + z * 8, ty + 16 };
                FillRect(dc, &sel, focus ? C_ACCENT : C_GRAY4);
                char part[TEXT_MAX];
                memcpy(part, shown + a, (size_t)(z - a));
                part[z - a] = '\0';
                TextOut(dc, tx + a * 8, ty, part, C_WHITE, -1);
            }
        }
        if (focus && !w->pressed)
            DrawLine(dc, tx + (w->caret - w->top) * 8, y + 4, fh - 8, true, C_BLACK);
        dc_unclip(dc, &saved);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETTEXT:
        DefWindowProc(h, msg, wp, lp);
        w->text[limit(w)] = '\0';
        w->caret = w->value = (int)strlen(w->text);
        w->top = 0;
        if (e) e->undo_ok = e->typing = false;
        show_caret(w);
        InvalidateRect(h, NULL);
        return 1;
    case EM_SETSEL: {                                   /* (0, -1): all; (-1, ...): none */
        int a = (int)wp, z = (int)lp;
        if (a < 0) { a = z = w->caret; }
        if (z < 0 || z > len) z = len;
        if (a > len) a = len;
        w->value = a;
        w->caret = z;
        show_caret(w);
        InvalidateRect(h, NULL);
        return 0;
    }
    case EM_GETSEL:
        if (wp) *(int *)wp = sel_lo(w);
        if (lp) *(int *)lp = sel_hi(w);
        return MAKELONG(sel_lo(w), sel_hi(w));
    case EM_REPLACESEL: {
        const char *s = (const char *)lp;
        int n = 0;
        while (s && s[n] && s[n] != '\r' && s[n] != '\n') n++;
        if (!replace_sel(w, s ? s : "", n, false) && e) e->typing = false;
        return 0;
    }
    case EM_LIMITTEXT:
        w->max = (int)wp;
        return 0;
    case EM_GETLIMITTEXT:
        return limit(w);
    case EM_CANUNDO:
        return e && e->undo_ok;
    case EM_UNDO:
    case WM_UNDO:
        if (e && e->undo_ok) {
            char t[TEXT_MAX];
            int c = w->caret;
            strncpy0(t, w->text, sizeof t);
            strncpy0(w->text, e->undo, sizeof w->text);
            w->caret = w->value = clampi(e->undo_caret, 0, (int)strlen(w->text));
            strncpy0(e->undo, t, sizeof e->undo);           /* undo the undo */
            e->undo_caret = c;
            e->typing = false;
            changed(w);
        }
        return 1;
    case EM_SETREADONLY:
        if (wp) w->style |= ES_READONLY; else w->style &= ~ES_READONLY;
        InvalidateRect(h, NULL);
        return 1;
    case WM_SETFOCUS:
        w->pressed = false;
        SetTimer(h, BLINK_ID, BLINK_MS);
        InvalidateRect(h, NULL);
        return 0;
    case WM_KILLFOCUS:
        KillTimer(h, BLINK_ID);
        w->pressed = false;
        InvalidateRect(h, NULL);
        return 0;
    case WM_ENABLE:
        InvalidateRect(h, NULL);
        return 0;
    case WM_TIMER:
        if (wp == BLINK_ID) {
            w->pressed = ui_has_focus(w) ? !w->pressed : false;
            if (!ui_has_focus(w)) KillTimer(h, BLINK_ID);
            int cx = RING + 5 + (w->caret - w->top) * 8;
            RECT r = { cx - 1, RING, cx + 2, w->h - RING };
            InvalidateRect(h, &r);
        }
        return 0;
    case WM_LBUTTONDBLCLK: {                            /* the word under the pointer */
        int at = char_at(w, GET_X_LPARAM(lp)), a = at, z = at;
        while (a > 0 && is_word(w->text[a - 1])) a--;
        while (z < len && is_word(w->text[z])) z++;
        w->value = a;
        w->caret = z;
        show_caret(w);
        InvalidateRect(h, NULL);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(h);
        move_to(w, char_at(w, GET_X_LPARAM(lp)), (wp & MK_SHIFT) != 0);
        w->tracking = true;
        SetCapture(h);
        return 0;
    case WM_MOUSEMOVE:
        if (w->tracking) {
            int x = GET_X_LPARAM(lp);
            if (x < RING + 5 && w->top > 0)             /* past the edges: scroll */
                w->top--;
            else if (x > w->w - RING - 5 && w->top + room(w) < len)
                w->top++;
            int at = char_at(w, x);
            if (at != w->caret) {
                w->caret = at;
                show_caret(w);
            }
            InvalidateRect(h, NULL);
        }
        return 0;
    case WM_LBUTTONUP:
        if (w->tracking) {
            w->tracking = false;
            ReleaseCapture();
        }
        return 0;
    case WM_COPY:
    case WM_CUT:
        if (!has_sel(w) || (w->style & ES_PASSWORD))
            return 0;
        SetClipboardText(w->text + sel_lo(w), sel_hi(w) - sel_lo(w));
        if (msg == WM_CUT && !(w->style & ES_READONLY))
            replace_sel(w, "", 0, false);
        return 0;
    case WM_CLEAR:
        if (!(w->style & ES_READONLY))
            replace_sel(w, "", 0, false);
        return 0;
    case WM_PASTE: {
        if (w->style & ES_READONLY)
            return 0;
        char clip[TEXT_MAX];
        int n = GetClipboardText(clip, sizeof clip), k = 0;
        while (k < n && clip[k] != '\n' && clip[k] != '\r') k++;
        for (int i = 0; i < k; i++)
            if (clip[i] == '\t') clip[i] = ' ';
        replace_sel(w, clip, k, false);
        return 0;
    }
    case WM_CHAR: {
        char ch = (char)wp;
        switch (ch) {
        case 1:  SendMessage(h, EM_SETSEL, 0, -1); return 0;       /* Ctrl+A */
        case 3:  SendMessage(h, WM_COPY, 0, 0); return 0;          /* Ctrl+C */
        case 22: SendMessage(h, WM_PASTE, 0, 0); return 0;         /* Ctrl+V */
        case 24: SendMessage(h, WM_CUT, 0, 0); return 0;           /* Ctrl+X */
        case 26: SendMessage(h, EM_UNDO, 0, 0); return 0;          /* Ctrl+Z */
        }
        if (w->style & ES_READONLY)
            return 0;
        if (ch >= 32 && ch < 127) {
            if (w->style & ES_NUMBER && (ch < '0' || ch > '9'))
                return 0;
            if (w->style & ES_UPPERCASE && ch >= 'a' && ch <= 'z') ch -= 32;
            if (w->style & ES_LOWERCASE && ch >= 'A' && ch <= 'Z') ch += 32;
            replace_sel(w, &ch, 1, true);
        } else if (ch == 8) {
            if (!has_sel(w) && w->caret > 0)
                w->value = w->caret - 1;
            replace_sel(w, "", 0, false);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (wp) {
        case VK_DELETE:
            if (w->style & ES_READONLY)
                return 0;
            if (shift && has_sel(w)) {                  /* Shift+Del: cut */
                SendMessage(h, WM_CUT, 0, 0);
                return 0;
            }
            if (!has_sel(w) && w->caret < len)
                w->value = ctrl ? word_right(w, w->caret) : w->caret + 1;
            replace_sel(w, "", 0, false);
            return 0;
        case VK_INSERT:
            if (ctrl) SendMessage(h, WM_COPY, 0, 0);
            else if (shift) SendMessage(h, WM_PASTE, 0, 0);
            return 0;
        case VK_LEFT:
            if (!shift && has_sel(w) && !ctrl) move_to(w, sel_lo(w), false);
            else move_to(w, ctrl ? word_left(w, w->caret) : w->caret > 0 ? w->caret - 1 : 0, shift);
            return 0;
        case VK_RIGHT:
            if (!shift && has_sel(w) && !ctrl) move_to(w, sel_hi(w), false);
            else move_to(w, ctrl ? word_right(w, w->caret) : w->caret < len ? w->caret + 1 : len, shift);
            return 0;
        case VK_HOME: move_to(w, 0, shift); return 0;
        case VK_END:  move_to(w, len, shift); return 0;
        }
        return 0;
    }
    }
    return DefWindowProc(h, msg, wp, lp);
}
