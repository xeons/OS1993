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
 *
 * Undo keeps a history: each change is a record (where, what was taken
 * out, what was put in), and the records of one run of typing share a
 * group, so Ctrl+Z steps back a run at a time and Ctrl+Y forward again
 * (the oldest are dropped past UNDO_MAX groups or UNDO_BYTES of text).
 * The caret blinks while the control has the focus. End on a wrapped row
 * puts the caret after its last character: the index there is also the
 * next row's start, so a flag (eol) says which row it belongs to.
 */
#include "uipriv.h"

#define LIMIT_DEFAULT 32767
#define TABW          8
#define UNDO_MAX      64                /* groups kept */
#define UNDO_BYTES    65536             /* text kept in all records */
#define BLINK_ID      0x7E01
#define SCROLL_ID     0x7E02
#define BLINK_MS      530

struct urec {
    int   pos, dlen, ilen, group;
    char *text;                         /* the dlen taken out, then the ilen put in */
};

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
    struct urec *u;                     /* [0, nu) done, [nu, nall) undone (redo) */
    int   nu, nall, ucap, group, ubytes;
    bool  armed, modified, eol, blink_off;
    int   mx, my;                       /* the pointer while selecting */
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

static bool wrapped_row(struct medit *m, int n)        /* ends without a newline */
{
    return n + 1 < m->nlines && m->buf[m->line[n + 1] - 1] != '\n';
}

static int line_end(struct medit *m, int n)            /* where End puts the caret */
{
    if (n + 1 >= m->nlines)
        return m->len;
    return wrapped_row(m, n) ? m->line[n + 1] : m->line[n + 1] - 1;
}

static int caret_line(struct medit *m)                 /* the caret's row (eol: the one above) */
{
    int ln = line_of(m, m->caret);
    if (m->eol && ln > 0 && m->line[ln] == m->caret && wrapped_row(m, ln - 1))
        return ln - 1;
    return ln;
}

static int row_stop(struct medit *m, int n)            /* the row's text ends here */
{
    if (n + 1 >= m->nlines)
        return m->len;
    int next = m->line[n + 1];
    return m->buf[next - 1] == '\n' ? next - 1 : next;
}

static int vcol_in(struct medit *m, int idx, int ln)
{
    int col = 0;
    for (int i = m->line[ln]; i < idx; i++)
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
    int ln = caret_line(m), c = vcol_in(m, m->caret, ln), r = rows(w), k = cols(w);
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

static void drop_records(struct medit *m, int from, int to)
{
    for (int i = from; i < to; i++) {
        m->ubytes -= m->u[i].dlen + m->u[i].ilen;
        LocalFree(m->u[i].text);
    }
}

static void clear_undo(struct medit *m)
{
    drop_records(m, 0, m->nall);
    m->nu = m->nall = 0;
    m->armed = false;
}

/* Remember a change about to happen: at pos, del characters out, ins (n) in. */
static void record(struct medit *m, int pos, int del, const char *ins, int n)
{
    if (del == 0 && n == 0)
        return;
    drop_records(m, m->nu, m->nall);            /* a new change: no more redo */
    m->nall = m->nu;
    if (!m->armed) {
        m->group++;
        m->armed = true;
    }
    struct urec *last = m->nu ? &m->u[m->nu - 1] : NULL;
    if (last && last->group == m->group && del == 0 && last->pos + last->ilen == pos) {
        char *t = LocalReAlloc(last->text, (uint32_t)(last->dlen + last->ilen + n), 0);
        if (t) {                                 /* typing on: the same record grows */
            memcpy(t + last->dlen + last->ilen, ins, (size_t)n);
            last->text = t;
            last->ilen += n;
            m->ubytes += n;
            return;
        }
    }
    if (m->nu == m->ucap) {
        int cap = m->ucap + 32;
        struct urec *u = LocalReAlloc(m->u, (uint32_t)cap * sizeof *u, 0);
        if (!u) { clear_undo(m); return; }
        m->u = u;
        m->ucap = cap;
    }
    char *t = LocalAlloc(0, (uint32_t)(del + n + 1));
    if (!t) { clear_undo(m); return; }
    memcpy(t, m->buf + pos, (size_t)del);
    memcpy(t + del, ins, (size_t)n);
    m->u[m->nu++] = (struct urec){ pos, del, n, m->group, t };
    m->nall = m->nu;
    m->ubytes += del + n;
    int groups = m->u[m->nu - 1].group - m->u[0].group + 1;    /* too much kept: the oldest go */
    while (m->nu > 1 && (groups > UNDO_MAX || m->ubytes > UNDO_BYTES)) {
        int g = m->u[0].group, k = 0;
        while (k < m->nu - 1 && m->u[k].group == g) k++;
        drop_records(m, 0, k);
        memmove(m->u, m->u + k, (size_t)(m->nu - k) * sizeof *m->u);
        m->nu -= k;
        m->nall = m->nu;
        groups = m->u[m->nu - 1].group - m->u[0].group + 1;
    }
}

static void moved(struct medit *m) { m->armed = false; }   /* a new run starts */

/* Replace the selection with text (n bytes, CRs dropped); false if the
 * limit kept some of it out. */
static bool grow(struct medit *m, int need)
{
    if (need + 1 <= m->cap)
        return true;
    int cap = (need + 1 + 1023) & ~1023;
    char *p = LocalReAlloc(m->buf, (uint32_t)cap, 0);
    if (!p) return false;
    m->buf = p;
    m->cap = cap;
    return true;
}

/* The buffer itself: n bytes at pos replace del (no undo record). */
static bool raw_replace(struct medit *m, int pos, int del, const char *text, int n)
{
    if (!grow(m, m->len - del + n))
        return false;
    memmove(m->buf + pos + n, m->buf + pos + del, (size_t)(m->len - pos - del + 1));
    memcpy(m->buf + pos, text, (size_t)n);
    m->len += n - del;
    return true;
}

static bool replace(struct wnd *w, const char *text, int n)
{
    struct medit *m = me(w);
    if (w->style & ES_READONLY)
        return false;
    int a = sel_start(m), b = sel_end(m);
    int add = 0;
    for (int i = 0; i < n; i++)
        if (text[i] != '\r') add++;
    bool all = true;
    if (m->len - (b - a) + add > m->limit) {
        add = m->limit - (m->len - (b - a));
        if (add < 0) add = 0;
        all = false;
    }
    char small[64], *clean = add <= (int)sizeof small ? small : LocalAlloc(0, (uint32_t)add);
    if (!clean)
        return false;
    for (int i = 0, k = 0; i < n && k < add; i++)
        if (text[i] != '\r')
            clean[k++] = text[i];
    m->eol = false;
    if (b > a || add) {
        record(m, a, b - a, clean, add);
        raw_replace(m, a, b - a, clean, add);
    }
    if (clean != small)
        LocalFree(clean);
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
    clear_undo(m);
    m->modified = false;
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

/* Step back (or, redo, forward) one group of records. */
static void undo(struct wnd *w, bool redo)
{
    struct medit *m = me(w);
    if (redo ? m->nu == m->nall : m->nu == 0)
        return;
    int g = m->u[redo ? m->nu : m->nu - 1].group, sel_a = 0, sel_z = 0;
    while (redo ? m->nu < m->nall && m->u[m->nu].group == g : m->nu > 0 && m->u[m->nu - 1].group == g) {
        struct urec *r = &m->u[redo ? m->nu : m->nu - 1];
        if (redo) {
            raw_replace(m, r->pos, r->dlen, r->text + r->dlen, r->ilen);
            sel_a = sel_z = r->pos + r->ilen;
            m->nu++;
        } else {
            raw_replace(m, r->pos, r->ilen, r->text, r->dlen);
            sel_a = r->pos;                     /* what came back is selected */
            sel_z = r->pos + r->dlen;
            m->nu--;
        }
    }
    m->anchor = clampi(sel_a, 0, m->len);
    m->caret = clampi(sel_z, 0, m->len);
    m->armed = m->eol = false;
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
    int c = m->caret, ln = caret_line(m), eol_row = -1;
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
        if (m->want < 0) m->want = vcol_in(m, c, ln);
        int d = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : vk == VK_PRIOR ? -rows(w) : rows(w);
        int to = clampi(ln + d, 0, m->nlines - 1);
        if (vk == VK_PRIOR || vk == VK_NEXT)
            scroll_to(w, m->top + d, m->left);
        c = at_vcol(m, to, m->want);
        eol_row = to;
        vertical = true;
        break;
    }
    case VK_HOME: c = ctrl ? 0 : m->line[ln]; break;
    case VK_END:  c = ctrl ? m->len : line_end(m, ln); eol_row = ctrl ? -1 : ln; break;
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
    m->eol = eol_row >= 0 && eol_row + 1 < m->nlines && c == m->line[eol_row + 1] && wrapped_row(m, eol_row);
    if (!shift) m->anchor = c;
    if (!vertical) m->want = -1;
    m->blink_off = false;
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
    case 26: undo(w, false); return;                                            /* Ctrl+Z */
    case 25: undo(w, true); return;                                             /* Ctrl+Y */
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

/* The character nearest x,y (past the edges: one row or column beyond),
 * with the eol flag for the end of a wrapped row. */
static int index_at(struct wnd *w, int x, int y, bool *eol)
{
    struct medit *m = me(w);
    RECT t = text_rect(w);
    int ln = m->top + (y < t.top ? -1 : (y - t.top) / 16);
    ln = clampi(ln, 0, m->nlines - 1);
    int c = x < t.left ? m->left - 1 : m->left + (x - t.left + 4) / 8;
    int i = at_vcol(m, ln, c < 0 ? 0 : c);
    *eol = ln + 1 < m->nlines && i == m->line[ln + 1] && wrapped_row(m, ln);
    return i;
}

/* The selection follows the pointer; outside the text it scrolls on a
 * timer even while the mouse keeps still. */
static void select_to(struct wnd *w)
{
    struct medit *m = me(w);
    bool eol;
    int c = index_at(w, m->mx, m->my, &eol);
    if (c != m->caret || eol != m->eol) {
        m->caret = c;
        m->eol = eol;
        show_caret(w);
        InvalidateRect(w, NULL);
    }
    RECT t = text_rect(w);
    if (rect_contains(&t, m->mx, m->my)) KillTimer(w, SCROLL_ID);
    else SetTimer(w, SCROLL_ID, 60);
}

static void caret_on(struct wnd *w)               /* visible at once, blinking from now */
{
    struct medit *m = me(w);
    m->blink_off = false;
    if (ui_has_focus(w))
        SetTimer(w, BLINK_ID, BLINK_MS);
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
    uint8_t fg = C_BLACK, bg = C_WHITE;
    ctl_color(w, dc, (w->style & ES_READONLY) ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT, &fg, &bg);
    FillRect(dc, &all, bg);
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
                TextOut(dc, x, y, run, run_sel ? C_WHITE : fg, -1);
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
    if (focus && !m->blink_off) {                   /* the caret */
        int ln = caret_line(m), c = vcol_in(m, m->caret, ln);
        if (ln >= m->top && ln < m->top + r + 1 && c >= m->left)
            DrawLine(dc, t.left + (c - m->left) * 8, t.top + (ln - m->top) * 16, 16, true, fg);
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
        clear_undo(m);
        LocalFree(m->u);
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
        caret_on(w);
        InvalidateRect(h, NULL);
        return 0;
    case WM_KILLFOCUS:
        KillTimer(h, BLINK_ID);
        InvalidateRect(h, NULL);
        return 0;
    case WM_ENABLE:
        InvalidateRect(h, NULL);
        return 0;
    case WM_TIMER:
        if (wp == BLINK_ID) {
            if (!ui_has_focus(w)) {
                KillTimer(h, BLINK_ID);
                m->blink_off = false;
            } else {
                m->blink_off = !m->blink_off;
            }
            int ln = caret_line(m), c = vcol_in(m, m->caret, ln);
            RECT t = text_rect(w);
            RECT r = { t.left + (c - m->left) * 8 - 1, t.top + (ln - m->top) * 16,
                       t.left + (c - m->left) * 8 + 2, t.top + (ln - m->top) * 16 + 16 };
            InvalidateRect(h, &r);
        } else if (wp == SCROLL_ID) {
            if (m->track == 1) select_to(w);
            else KillTimer(h, SCROLL_ID);
        }
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
        caret_on(w);
        return 0;
    case WM_CHAR:
        typed(w, (char)wp);
        caret_on(w);
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        SetFocus(h);
        if (bar_press(w, 2, x, y) || bar_press(w, 3, x, y))
            return 0;
        m->caret = index_at(w, x, y, &m->eol);
        if (!(GetKeyState(VK_SHIFT) < 0))
            m->anchor = m->caret;
        m->want = -1;
        m->track = 1;
        m->mx = x;
        m->my = y;
        moved(m);
        caret_on(w);
        SetCapture(h);
        InvalidateRect(h, NULL);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {                        /* a word */
        bool eol;
        int i = index_at(w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &eol), a = i, z = i;
        while (a > 0 && is_word(m->buf[a - 1])) a--;
        while (z < m->len && is_word(m->buf[z])) z++;
        m->anchor = a;
        m->caret = z;
        m->eol = false;
        moved(m);
        InvalidateRect(h, NULL);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (m->track == 1) {
            m->mx = x;
            m->my = y;
            select_to(w);
        } else if ((m->track == 2 || m->track == 3) && m->part == SB_PART_THUMB) {
            struct sbar s = m->track == 2 ? vbar(w) : hbar(w);
            int v = sbar_thumb_value(&s, s.vert ? y : x, m->grab);
            if (m->track == 2) scroll_to(w, v, m->left);
            else scroll_to(w, m->top, v);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        KillTimer(h, SCROLL_ID);
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
    case EM_UNDO:  undo(w, false); return 1;
    case EM_CANUNDO: return m->nu > 0;
    case EM_CANREDO: return m->nu < m->nall;
    case EM_REDO:    undo(w, true); return 1;
    case EM_EMPTYUNDOBUFFER: clear_undo(m); return 0;
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
        m->eol = false;
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
