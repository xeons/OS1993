/*
 * medit.c -- the EDIT class with ES_MULTILINE: a text editor control.
 *
 * Lines end with LF (CRs are dropped on the way in); tabs stop every 8
 * columns. The text lives in the owning process's heap (LocalAlloc) and
 * grows up to the limit (EM_LIMITTEXT, 32767 by default, as Win95's edit
 * control). Without WS_HSCROLL (or ES_AUTOHSCROLL) it wraps words at the
 * window's width, as Win32's does; otherwise it scrolls sideways. Scroll
 * bars for WS_VSCROLL and WS_HSCROLL; selects with the mouse or Shift and the movement keys;
 * cuts, copies and pastes through the clipboard; and undoes the last run
 * of typing (and undoes the undo). The usual EM_* messages work.
 */
#include "uipriv.h"

#define LIMIT_DEFAULT 32767
#define TABW          8

struct medit {
    char *buf;
    int   len, cap, limit;
    int  *line;                         /* where each row (line, or wrapped part) starts */
    int   nlines, lcap;
    int   wrapw;                        /* wrap at this many columns; 0: don't */
    int   maxw;                         /* the widest line, in columns */
    int   anchor, caret;                /* the selection: between them */
    int   top, left;                    /* first line and column shown */
    int   want;                         /* column Up/Down aim for, -1 none */
    char *undo;
    int   undo_len;
    bool  undo_ok, armed, modified;
    int   track;                        /* 1 selecting, 2 vertical bar, 3 horizontal bar */
    int   part, grab;
};

static struct medit *me(struct wnd *w) { return w->ctl; }

/* ---- geometry ---------------------------------------------------------- */

static int border(struct wnd *w) { return (w->style & WS_BORDER) ? 2 : 0; }
static bool has_v(struct wnd *w) { return (w->style & WS_VSCROLL) != 0; }
static bool has_h(struct wnd *w) { return (w->style & WS_HSCROLL) != 0; }

static RECT text_rect(struct wnd *w)
{
    int b = border(w);
    return (RECT){ b + 3, b + 1, w->w - b - (has_v(w) ? SCROLL_W : 0) - 1,
                   w->h - b - (has_h(w) ? SCROLL_W : 0) };
}

static int rows(struct wnd *w) { RECT t = text_rect(w); int r = (t.bottom - t.top) / 16; return r > 0 ? r : 1; }
static int cols(struct wnd *w) { RECT t = text_rect(w); int c = (t.right - t.left) / 8; return c > 0 ? c : 1; }

/* ---- lines and columns --------------------------------------------------- */

static int advance(int col, char c) { return c == '\t' ? (col / TABW + 1) * TABW : col + 1; }

static bool push_row(struct medit *m, int start)
{
    if (m->nlines == m->lcap) {
        int cap = m->lcap + 256;
        int *p = LocalReAlloc(m->line, (uint32_t)cap * sizeof(int), 0);
        if (!p) return false;
        m->line = p;
        m->lcap = cap;
    }
    m->line[m->nlines++] = start;
    return true;
}

/* Where each row starts: after every LF and, when wrapping, after the last
 * space that still fits (or hard at the width when a word is longer). */
static void relines(struct medit *m)
{
    int col = 0, maxw = 0, brk = -1, start = 0;
    m->nlines = 0;
    if (!push_row(m, 0))
        return;
    for (int i = 0; i < m->len; i++) {
        char c = m->buf[i];
        if (c == '\n') {
            if (col > maxw) maxw = col;
            col = 0;
            brk = -1;
            start = i + 1;
            if (!push_row(m, start)) break;
            continue;
        }
        int next = advance(col, c);
        if (m->wrapw && next > m->wrapw && i > start) {
            int at = brk > start ? brk : i;
            if (col > maxw) maxw = col;
            if (!push_row(m, at)) break;
            start = at;
            col = 0;
            for (int k = at; k < i; k++)
                col = advance(col, m->buf[k]);
            brk = -1;
            next = advance(col, c);
        }
        if (c == ' ')
            brk = i + 1;
        col = next;
    }
    m->maxw = col > maxw ? col : maxw;
}

static bool wraps(struct wnd *w)
{
    return !(w->style & (WS_HSCROLL | ES_AUTOHSCROLL));
}

static int cols(struct wnd *w);

static void layout(struct wnd *w)                   /* rows for the current width */
{
    struct medit *m = w->ctl;
    m->wrapw = wraps(w) ? cols(w) : 0;
    relines(m);
}

static int line_of(struct medit *m, int idx)
{
    int lo = 0, hi = m->nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (m->line[mid] <= idx) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static int line_end(struct medit *m, int n)            /* where End puts the caret */
{
    return n + 1 < m->nlines ? m->line[n + 1] - 1 : m->len;
}

static int row_stop(struct medit *m, int n)            /* the row's text ends here */
{
    if (n + 1 >= m->nlines)
        return m->len;
    int next = m->line[n + 1];
    return m->buf[next - 1] == '\n' ? next - 1 : next;
}

static int vcol(struct medit *m, int idx)
{
    int col = 0;
    for (int i = m->line[line_of(m, idx)]; i < idx; i++)
        col = m->buf[i] == '\t' ? (col / TABW + 1) * TABW : col + 1;
    return col;
}

static int at_vcol(struct medit *m, int n, int want)   /* the nearest character */
{
    int col = 0, i = m->line[n], end = line_end(m, n);
    for (; i < end; i++) {
        int next = m->buf[i] == '\t' ? (col / TABW + 1) * TABW : col + 1;
        if (want < next && want - col < next - want)
            break;
        if (want < next) { i++; break; }
        col = next;
    }
    return i > end ? end : i;
}

static int sel_start(struct medit *m) { return m->anchor < m->caret ? m->anchor : m->caret; }
static int sel_end(struct medit *m)   { return m->anchor > m->caret ? m->anchor : m->caret; }

/* ---- scrolling -------------------------------------------------------------- */

static int vmax(struct wnd *w) { int v = me(w)->nlines - rows(w); return v > 0 ? v : 0; }
static int hmax(struct wnd *w) { int v = me(w)->maxw + 1 - cols(w); return v > 0 ? v : 0; }

static void scroll_to(struct wnd *w, int top, int left)
{
    struct medit *m = me(w);
    top = clampi(top, 0, vmax(w));
    left = clampi(left, 0, hmax(w) > m->left ? hmax(w) : m->left);
    if (left < 0) left = 0;
    if (top != m->top || left != m->left) {
        m->top = top;
        m->left = left;
        InvalidateRect(w, NULL);
    }
}

static void show_caret(struct wnd *w)
{
    struct medit *m = me(w);
    int ln = line_of(m, m->caret), c = vcol(m, m->caret), r = rows(w), k = cols(w);
    int top = m->top, left = m->left;
    if (ln < top) top = ln;
    if (ln >= top + r) top = ln - r + 1;
    if (c < left) left = c - k / 4 > 0 ? c - k / 4 : 0;
    if (c >= left + k) left = c - k + k / 4 + 1;
    if (top != m->top || left != m->left) {
        m->top = clampi(top, 0, m->nlines - 1);
        m->left = left < 0 ? 0 : left;
        InvalidateRect(w, NULL);
    }
}

/* ---- editing ------------------------------------------------------------------ */

static void changed(struct wnd *w)
{
    struct medit *m = me(w);
    m->modified = true;
    m->want = -1;
    layout(w);
    show_caret(w);
    InvalidateRect(w, NULL);
    if (w->parent)
        SendMessage(w->parent, WM_COMMAND, MAKEWPARAM(w->id, EN_CHANGE), (LPARAM)w);
}

static void snapshot(struct medit *m)               /* undo point, once per run */
{
    if (m->armed)
        return;
    char *u = LocalReAlloc(m->undo, (uint32_t)m->len + 1, 0);
    if (!u)
        return;
    m->undo = u;
    memcpy(u, m->buf, (size_t)m->len);
    m->undo_len = m->len;
    m->undo_ok = m->armed = true;
}

static void moved(struct medit *m) { m->armed = false; }   /* a new run starts */

static void delete_range(struct medit *m, int a, int b)
{
    memmove(m->buf + a, m->buf + b, (size_t)(m->len - b));
    m->len -= b - a;
    m->buf[m->len] = '\0';
    m->caret = m->anchor = a;
}

/* Replace the selection with text (n bytes, CRs dropped); false if the
 * limit kept some of it out. */
static bool replace(struct wnd *w, const char *text, int n)
{
    struct medit *m = me(w);
    if (w->style & ES_READONLY)
        return false;
    snapshot(m);
    int a = sel_start(m), b = sel_end(m);
    if (b > a)
        delete_range(m, a, b);
    int add = 0;
    for (int i = 0; i < n; i++)
        if (text[i] != '\r') add++;
    bool all = true;
    if (m->len + add > m->limit) {
        add = m->limit - m->len;
        all = false;
    }
    if (m->len + add + 1 > m->cap) {
        int cap = (m->len + add + 1 + 1023) & ~1023;
        char *p = LocalReAlloc(m->buf, (uint32_t)cap, 0);
        if (!p) return false;
        m->buf = p;
        m->cap = cap;
    }
    memmove(m->buf + a + add, m->buf + a, (size_t)(m->len - a + 1));
    for (int i = 0, k = 0; i < n && k < add; i++)
        if (text[i] != '\r')
            m->buf[a + k++] = text[i];
    m->len += add;
    m->caret = m->anchor = a + add;
    changed(w);
    return all;
}

static void set_text(struct wnd *w, const char *text)
{
    struct medit *m = me(w);
    m->len = 0;
    m->buf[0] = '\0';
    m->caret = m->anchor = 0;
    bool ro = w->style & ES_READONLY;
    w->style &= ~(unsigned)ES_READONLY;
    replace(w, text ? text : "", text ? (int)strlen(text) : 0);
    if (ro) w->style |= ES_READONLY;
    m->caret = m->anchor = 0;
    m->top = m->left = 0;
    m->undo_ok = m->armed = m->modified = false;
    InvalidateRect(w, NULL);
}

static void copy(struct medit *m)
{
    int a = sel_start(m), b = sel_end(m);
    if (b > a)
        SetClipboardText(m->buf + a, b - a);
}

static void paste(struct wnd *w)
{
    int n = GetClipboardTextLength();
    if (!n)
        return;
    char *t = LocalAlloc(0, (uint32_t)n + 1);
    if (!t)
        return;
    GetClipboardText(t, n + 1);
    replace(w, t, n);
    LocalFree(t);
}

static void undo(struct wnd *w)                     /* swap with the snapshot */
{
    struct medit *m = me(w);
    if (!m->undo_ok)
        return;
    char *cur = LocalAlloc(0, (uint32_t)m->len + 1);
    if (!cur)
        return;
    memcpy(cur, m->buf, (size_t)m->len);
    int cur_len = m->len;
    if (m->undo_len + 1 > m->cap) {
        char *p = LocalReAlloc(m->buf, (uint32_t)m->undo_len + 1, 0);
        if (!p) { LocalFree(cur); return; }
        m->buf = p;
        m->cap = m->undo_len + 1;
    }
    memcpy(m->buf, m->undo, (size_t)m->undo_len);
    m->len = m->undo_len;
    m->buf[m->len] = '\0';
    LocalFree(m->undo);
    m->undo = cur;
    m->undo_len = cur_len;
    m->caret = m->anchor = clampi(m->caret, 0, m->len);
    m->armed = false;
    changed(w);
}

/* ---- keyboard ---------------------------------------------------------------- */

static bool is_word(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }

static int word_left(struct medit *m, int i)
{
    while (i > 0 && !is_word(m->buf[i - 1])) i--;
    while (i > 0 && is_word(m->buf[i - 1])) i--;
    return i;
}

static int word_right(struct medit *m, int i)
{
    while (i < m->len && is_word(m->buf[i])) i++;
    while (i < m->len && !is_word(m->buf[i])) i++;
    return i;
}

static void key(struct wnd *w, WPARAM vk, LPARAM lp)
{
    struct medit *m = me(w);
    bool shift = lp & (1 << 25), ctrl = lp & (1 << 27);
    int c = m->caret, ln = line_of(m, c);
    bool vertical = false;
    switch (vk) {
    case VK_LEFT:
        if (!shift && m->anchor != c) c = sel_start(m);
        else c = ctrl ? word_left(m, c) : c > 0 ? c - 1 : 0;
        break;
    case VK_RIGHT:
        if (!shift && m->anchor != c) c = sel_end(m);
        else c = ctrl ? word_right(m, c) : c < m->len ? c + 1 : m->len;
        break;
    case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: {
        if (m->want < 0) m->want = vcol(m, c);
        int d = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : vk == VK_PRIOR ? -rows(w) : rows(w);
        int to = clampi(ln + d, 0, m->nlines - 1);
        if (vk == VK_PRIOR || vk == VK_NEXT)
            scroll_to(w, m->top + d, m->left);
        c = at_vcol(m, to, m->want);
        vertical = true;
        break;
    }
    case VK_HOME: c = ctrl ? 0 : m->line[ln]; break;
    case VK_END:  c = ctrl ? m->len : line_end(m, ln); break;
    case VK_DELETE:
        if (m->anchor == c && c < m->len) m->caret = c + 1;
        if (m->anchor != m->caret) {
            if (shift) copy(m);                     /* Shift+Del: cut */
            replace(w, "", 0);
        }
        return;
    default:
        return;
    }
    m->caret = c;
    if (!shift) m->anchor = c;
    if (!vertical) m->want = -1;
    moved(m);
    show_caret(w);
    InvalidateRect(w, NULL);
}

static void typed(struct wnd *w, char ch)
{
    struct medit *m = me(w);
    switch (ch) {
    case 1:  m->anchor = 0; m->caret = m->len; InvalidateRect(w, NULL); return;   /* Ctrl+A */
    case 3:  copy(m); return;                                                   /* Ctrl+C */
    case 22: paste(w); return;                                                  /* Ctrl+V */
    case 24: copy(m); replace(w, "", 0); return;                                /* Ctrl+X */
    case 26: undo(w); return;                                                   /* Ctrl+Z */
    case 13: replace(w, "\n", 1); return;
    case 9:  replace(w, "\t", 1); return;
    case 8:
        if (m->anchor == m->caret && m->caret > 0)
            m->anchor = m->caret - 1;
        replace(w, "", 0);
        return;
    }
    if ((uint8_t)ch >= 32 && (uint8_t)ch < 127)
        replace(w, &ch, 1);
}

/* ---- the mouse --------------------------------------------------------------------- */

static int index_at(struct wnd *w, int x, int y)
{
    struct medit *m = me(w);
    RECT t = text_rect(w);
    int ln = m->top + (y < t.top ? -1 : (y - t.top) / 16);
    ln = clampi(ln, 0, m->nlines - 1);
    int c = m->left + (x - t.left + 4) / 8;
    return at_vcol(m, ln, c < 0 ? 0 : c);
}

static struct sbar vbar(struct wnd *w)
{
    int b = border(w);
    return (struct sbar){ true, w->w - b - SCROLL_W, b, SCROLL_W, w->h - 2 * b - (has_h(w) ? SCROLL_W : 0),
                          me(w)->top, 0, vmax(w), rows(w) };
}

static struct sbar hbar(struct wnd *w)
{
    int b = border(w);
    return (struct sbar){ false, b, w->h - b - SCROLL_W, w->w - 2 * b - (has_v(w) ? SCROLL_W : 0), SCROLL_W,
                          me(w)->left, 0, hmax(w), cols(w) };
}

static bool bar_press(struct wnd *w, int which, int x, int y)
{
    struct medit *m = me(w);
    struct sbar s = which == 2 ? vbar(w) : hbar(w);
    RECT r = { s.x, s.y, s.x + s.w, s.y + s.h };
    if (!(which == 2 ? has_v(w) : has_h(w)) || !rect_contains(&r, x, y))
        return false;
    m->part = sbar_hit(&s, x, y);
    int v = s.value, step = m->part == SB_PART_DEC ? -1 : m->part == SB_PART_INC ? 1 :
                           m->part == SB_PART_PGDEC ? -s.page : m->part == SB_PART_PGINC ? s.page : 0;
    if (m->part == SB_PART_THUMB) {
        int pos, len, track, tlen;
        sbar_thumb(&s, &pos, &len, &track, &tlen);
        m->grab = (s.vert ? y : x) - pos;
    }
    if (which == 2) scroll_to(w, v + step, m->left);
    else scroll_to(w, m->top, v + step);
    m->track = which;
    SetCapture(w);
    InvalidateRect(w, NULL);
    return true;
}

/* ---- painting ---------------------------------------------------------------------- */

static void paint(struct wnd *w, HDC dc)
{
    struct medit *m = me(w);
    RECT all = { 0, 0, w->w, w->h };
    int b = border(w);
    FillRect(dc, &all, C_WHITE);
    if (b) {
        RECT s = { 0, 0, w->w, w->h }, k = { 1, 1, w->w - 1, w->h - 1 };
        DrawBevel(dc, &s, false);
        FrameRect(dc, &k, C_GRAY1);
    }
    RECT t = text_rect(w);
    bool focus = ui_has_focus(w);
    int a = sel_start(m), z = sel_end(m), r = rows(w), k = cols(w);
    char run[96];
    for (int row = 0; row <= r && m->top + row < m->nlines; row++) {
        int ln = m->top + row, i = m->line[ln], end = row_stop(m, ln), col = 0, y = t.top + row * 16;
        int n = 0, run_col = 0;
        bool run_sel = false;
        for (;; i++) {
            bool at_end = i >= end;
            char ch = at_end ? 0 : m->buf[i];
            bool sel = !at_end && i >= a && i < z;
            int next = ch == '\t' ? (col / TABW + 1) * TABW : col + 1;
            if (n && (at_end || sel != run_sel || ch == '\t' || n == (int)sizeof run - 1 ||
                      col >= m->left + k + 1)) {
                run[n] = '\0';                      /* flush the run */
                int x = t.left + (run_col - m->left) * 8;
                if (run_sel) {
                    RECT s = { x, y, x + n * 8, y + 16 };
                    FillRect(dc, &s, focus ? C_ACCENT : C_GRAY4);
                }
                TextOut(dc, x, y, run, run_sel ? C_WHITE : C_BLACK, -1);
                n = 0;
            }
            if (at_end || col >= m->left + k + 1)
                break;
            if (ch == '\t') {
                if (sel && next > m->left) {
                    int x0 = t.left + ((col > m->left ? col : m->left) - m->left) * 8;
                    RECT s = { x0, y, t.left + (next - m->left) * 8, y + 16 };
                    FillRect(dc, &s, focus ? C_ACCENT : C_GRAY4);
                }
            } else if (col >= m->left) {
                if (!n) { run_col = col; run_sel = sel; }
                run[n++] = ch;
            }
            col = next;
        }
        if (z > end && a <= end && end < m->len && m->buf[end] == '\n') {   /* the selected newline */
            int x = t.left + (col - m->left) * 8;
            RECT s = { x, y, x + 8, y + 16 };
            if (col >= m->left) FillRect(dc, &s, focus ? C_ACCENT : C_GRAY4);
        }
    }
    if (focus) {                                    /* the caret */
        int ln = line_of(m, m->caret), c = vcol(m, m->caret);
        if (ln >= m->top && ln < m->top + r + 1 && c >= m->left)
            DrawLine(dc, t.left + (c - m->left) * 8, t.top + (ln - m->top) * 16, 16, true, C_BLACK);
    }
    if (has_v(w)) { struct sbar s = vbar(w); sbar_draw(dc, &s, m->track == 2 ? m->part : SB_PART_NONE); }
    if (has_h(w)) { struct sbar s = hbar(w); sbar_draw(dc, &s, m->track == 3 ? m->part : SB_PART_NONE); }
    if (has_v(w) && has_h(w)) {
        RECT c = { w->w - b - SCROLL_W, w->h - b - SCROLL_W, w->w - b, w->h - b };
        FillRect(dc, &c, C_FACE);
    }
}

/* ---- the window procedure --------------------------------------------------------- */

LRESULT medit_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    struct medit *m = me(w);
    if (!m && msg != WM_CREATE)
        return DefWindowProc(h, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        m = LocalAlloc(LMEM_ZEROINIT, sizeof *m);
        if (!m || !(m->buf = LocalAlloc(LMEM_ZEROINIT, 1024)))
            return -1;
        w->ctl = m;
        m->cap = 1024;
        m->limit = LIMIT_DEFAULT;
        m->want = -1;
        layout(w);
        set_text(w, w->text);
        return 0;
    case WM_DESTROY:
        LocalFree(m->buf);
        LocalFree(m->line);
        LocalFree(m->undo);
        LocalFree(m);
        w->ctl = NULL;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(w, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        layout(w);
        scroll_to(w, m->top, m->left);
        show_caret(w);
        InvalidateRect(h, NULL);
        return 0;
    case WM_STYLECHANGED:                           /* word wrap on or off */
        layout(w);
        m->left = 0;
        m->top = clampi(m->top, 0, vmax(w));
        show_caret(w);
        InvalidateRect(h, NULL);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(h, NULL);
        return 0;
    case WM_SETTEXT:
        set_text(w, (const char *)lp);
        return 1;
    case WM_GETTEXT: {
        if (!lp || !wp) return 0;
        int n = m->len < (int)wp - 1 ? m->len : (int)wp - 1;
        memcpy((char *)lp, m->buf, (size_t)n);
        ((char *)lp)[n] = '\0';
        return n;
    }
    case WM_GETTEXTLENGTH:
        return m->len;
    case WM_KEYDOWN:
        key(w, wp, lp);
        return 0;
    case WM_CHAR:
        typed(w, (char)wp);
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        SetFocus(h);
        if (bar_press(w, 2, x, y) || bar_press(w, 3, x, y))
            return 0;
        m->caret = index_at(w, x, y);
        if (!(GetKeyState(VK_SHIFT) < 0))
            m->anchor = m->caret;
        m->want = -1;
        m->track = 1;
        moved(m);
        SetCapture(h);
        InvalidateRect(h, NULL);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {                        /* a word */
        int i = index_at(w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)), a = i, z = i;
        while (a > 0 && is_word(m->buf[a - 1])) a--;
        while (z < m->len && is_word(m->buf[z])) z++;
        m->anchor = a;
        m->caret = z;
        moved(m);
        InvalidateRect(h, NULL);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (m->track == 1) {
            int c = index_at(w, x, y);
            if (c != m->caret) {
                m->caret = c;
                show_caret(w);
                InvalidateRect(h, NULL);
            }
        } else if ((m->track == 2 || m->track == 3) && m->part == SB_PART_THUMB) {
            struct sbar s = m->track == 2 ? vbar(w) : hbar(w);
            int v = sbar_thumb_value(&s, s.vert ? y : x, m->grab);
            if (m->track == 2) scroll_to(w, v, m->left);
            else scroll_to(w, m->top, v);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (m->track) {
            ReleaseCapture();
            m->track = 0;
            InvalidateRect(h, NULL);
        }
        return 0;
    case WM_CUT:   copy(m); replace(w, "", 0); return 0;
    case WM_COPY:  copy(m); return 0;
    case WM_PASTE: paste(w); return 0;
    case WM_CLEAR: replace(w, "", 0); return 0;
    case WM_UNDO:
    case EM_UNDO:  undo(w); return 1;
    case EM_CANUNDO: return m->undo_ok;
    case EM_GETSEL:
        if (wp) *(int *)wp = sel_start(m);
        if (lp) *(int *)lp = sel_end(m);
        return MAKELONG(sel_start(m), sel_end(m));
    case EM_SETSEL:
        if ((int)wp == -1) {
            m->anchor = m->caret;
        } else {
            int a = clampi((int)wp, 0, m->len), z = (int)lp < 0 ? m->len : clampi((int)lp, 0, m->len);
            m->anchor = a;
            m->caret = z;
        }
        moved(m);
        InvalidateRect(h, NULL);
        return 0;
    case EM_REPLACESEL: {               /* the program may change a read-only one (as in Win32) */
        bool ro = w->style & ES_READONLY;
        w->style &= ~(unsigned)ES_READONLY;
        replace(w, lp ? (const char *)lp : "", lp ? (int)strlen((const char *)lp) : 0);
        if (ro) w->style |= ES_READONLY;
        return 0;
    }
    case EM_SCROLLCARET:   show_caret(w); return 1;
    case EM_GETMODIFY:     return m->modified;
    case EM_SETMODIFY:     m->modified = wp != 0; return 0;
    case EM_LIMITTEXT:     m->limit = wp ? (int)wp : 65535; return 0;
    case EM_GETLINECOUNT:  return m->nlines;
    case EM_LINEFROMCHAR:  return line_of(m, (int)wp < 0 ? m->caret : clampi((int)wp, 0, m->len));
    case EM_LINEINDEX: {
        int ln = (int)wp < 0 ? line_of(m, m->caret) : (int)wp;
        return ln < m->nlines ? m->line[ln] : -1;
    }
    }
    return DefWindowProc(h, msg, wp, lp);
}
