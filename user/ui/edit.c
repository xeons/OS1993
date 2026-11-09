/*
 * edit.c -- the EDIT class: a single-line text field. The outer 3 pixels
 * hold the accent focus ring; inside is an inset white field with a caret.
 * Changes are reported to the parent as WM_COMMAND EN_CHANGE.
 */
#include "uipriv.h"

#define RING 3

static int first_visible(struct wnd *w)
{
    int room = (w->w - 2 * RING - 10) / 8;
    return w->caret > room ? w->caret - room : 0;
}

static void changed(struct wnd *w)
{
    InvalidateRect(w, NULL);
    if (w->parent)
        SendMessage(w->parent, WM_COMMAND, MAKEWPARAM(w->id, EN_CHANGE), (LPARAM)w);
}

LRESULT edit_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    if (w->style & ES_MULTILINE)
        return medit_proc(h, msg, wp, lp);
    int len = (int)strlen(w->text);
    switch (msg) {
    case WM_CREATE:
        w->caret = len;
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
        FillRect(dc, &in, w->enabled ? C_WHITE : C_FACE);
        int first = first_visible(w);
        bool all = w->pressed && len > 0;               /* everything selected */
        if (all) {
            int sw = (len - first) * 8;
            if (sw > fw - 10) sw = fw - 10;
            RECT sel = { x + 4, y + (fh - 16) / 2, x + 6 + sw, y + (fh - 16) / 2 + 16 };
            FillRect(dc, &sel, focus ? C_ACCENT : C_GRAY4);
        }
        TextOut(dc, x + 5, y + (fh - 16) / 2, w->text + first,
                all ? C_WHITE : w->enabled ? C_BLACK : C_GRAY3, -1);
        if (focus && !all)
            DrawLine(dc, x + 5 + (w->caret - first) * 8, y + 4, fh - 8, true, C_BLACK);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETTEXT:
        DefWindowProc(h, msg, wp, lp);
        w->caret = (int)strlen(w->text);
        w->pressed = false;
        return 1;
    case EM_SETSEL:                                     /* (0, -1): all; (-1, ...): none */
        w->pressed = (int)wp == 0 && (int)lp == -1;
        w->caret = (int)strlen(w->text);
        InvalidateRect(h, NULL);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(h, NULL);
        return 0;
    case WM_LBUTTONDBLCLK:
        SendMessage(h, EM_SETSEL, 0, -1);
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        w->pressed = false;
        w->caret = clampi(first_visible(w) + (GET_X_LPARAM(lp) - RING - 5 + 4) / 8, 0, len);
        InvalidateRect(h, NULL);
        return 0;
    case WM_COPY:
    case WM_CUT:
        SetClipboardText(w->text, len);             /* one line: the lot */
        if (msg == WM_CUT && !(w->style & ES_READONLY)) {
            w->text[0] = '\0';
            w->caret = 0;
            w->pressed = false;
            changed(w);
        }
        return 0;
    case WM_PASTE: {
        char clip[TEXT_MAX];
        int n = GetClipboardText(clip, sizeof clip);
        for (int i = 0; i < n && clip[i] != '\n' && clip[i] != '\r'; i++)
            SendMessage(h, WM_CHAR, (WPARAM)(uint8_t)clip[i], 0);
        return 0;
    }
    case WM_CHAR: {
        char ch = (char)wp;
        switch (ch) {
        case 1:  SendMessage(h, EM_SETSEL, 0, -1); return 0;       /* Ctrl+A */
        case 3:  SendMessage(h, WM_COPY, 0, 0); return 0;          /* Ctrl+C */
        case 22: SendMessage(h, WM_PASTE, 0, 0); return 0;         /* Ctrl+V */
        case 24: SendMessage(h, WM_CUT, 0, 0); return 0;           /* Ctrl+X */
        }
        if (w->pressed && (ch == 8 || (ch >= 32 && ch < 127))) {   /* typing replaces it */
            w->pressed = false;
            w->text[0] = '\0';
            w->caret = len = 0;
            if (ch == 8) {
                changed(w);
                return 0;
            }
        }
        if (ch >= 32 && ch < 127 && len < TEXT_MAX - 1) {
            for (int i = len; i >= w->caret; i--)
                w->text[i + 1] = w->text[i];
            w->text[w->caret++] = ch;
            changed(w);
        } else if (ch == 8 && w->caret > 0) {
            for (int i = w->caret - 1; i < len; i++)
                w->text[i] = w->text[i + 1];
            w->caret--;
            changed(w);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (w->pressed && (wp == VK_DELETE || wp == VK_LEFT || wp == VK_RIGHT ||
                           wp == VK_HOME || wp == VK_END)) {
            w->pressed = false;
            if (wp == VK_DELETE) {
                w->text[0] = '\0';
                w->caret = 0;
                changed(w);
                return 0;
            }
        }
        switch (wp) {
        case VK_DELETE:
            if (w->caret < len) {
                for (int i = w->caret; i < len; i++)
                    w->text[i] = w->text[i + 1];
                changed(w);
            }
            return 0;
        case VK_LEFT:  if (w->caret > 0) w->caret--; break;
        case VK_RIGHT: if (w->caret < len) w->caret++; break;
        case VK_HOME:  w->caret = 0; break;
        case VK_END:   w->caret = len; break;
        default:       return 0;
        }
        InvalidateRect(h, NULL);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}
