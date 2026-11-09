/*
 * msgbox.c -- MessageBox: a modal alert with an icon, wrapped text and the
 * standard button sets (OK, OK/Cancel, Yes/No/Cancel, ...).
 *
 * Like Windows, it is modal to its owner: the owner is disabled (clicks
 * on it bring the box forward) and MessageBox runs a message loop of its
 * own until a button is chosen, so other windows keep working. Like Mac
 * OS, the buttons sit at the bottom right with the default one -- ringed,
 * and pressed by Enter -- rightmost. Esc and the close box mean Cancel
 * (OK in an OK-only box); Yes/No/Abort/Retry/Ignore answer to their
 * initial letters too. Icons: stop sign, caution triangle, note, question.
 */
#include "uipriv.h"

#define MARGIN   16
#define ICON     32
#define WRAP_W   296            /* text wraps at 37 columns */
#define LINE_MAX 12
#define LINE_LEN 40
#define BTN_W    76
#define BTN_H    22
#define BTN_GAP  10
#define DEF_RING 4

struct button { int id; const char *text; char key; };

struct mbox {
    unsigned      type;
    int           nbuttons;
    struct button buttons[3];
    int           cancel;       /* result for Esc / close, 0 if none */
    int           def;          /* default button index */
    char          lines[LINE_MAX][LINE_LEN];
    int           nlines, textw;
    bool          done;
    int           result;
};

/* ---- icons ------------------------------------------------------------- */

static const uint16_t g_bang[16] = { 0, 0x03C0, 0x03C0, 0x03C0, 0x03C0, 0x03C0, 0x03C0, 0x03C0,
                                     0x0180, 0x0180, 0, 0x03C0, 0x03C0, 0x03C0, 0, 0 };
static const uint16_t g_info[16] = { 0, 0x0380, 0x0380, 0x0380, 0, 0, 0x0F80, 0x0380,
                                     0x0380, 0x0380, 0x0380, 0x0380, 0x0380, 0x0FE0, 0, 0 };
static const uint16_t g_quest[16] = { 0, 0x07E0, 0x0FF0, 0x1C38, 0x1838, 0x0038, 0x0070, 0x00E0,
                                      0x01C0, 0x0380, 0x0380, 0, 0x0380, 0x0380, 0, 0 };

static int isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return r;
}

static void octagon(HDC dc, int x, int y, int s, uint8_t c)
{
    int cut = s * 9 / 32;
    for (int i = 0; i < s; i++) {
        int in = cut - i;
        if (i - (s - 1 - cut) > in) in = i - (s - 1 - cut);
        if (in < 0) in = 0;
        DrawLine(dc, x + in, y + i, s - 2 * in, false, c);
    }
}

static void circle(HDC dc, int x, int y, int d, uint8_t c)
{
    for (int i = 0; i < d; i++) {
        int dy = 2 * i - (d - 1);
        int half = (isqrt((d - 1) * (d - 1) - dy * dy) + 1) / 2;
        DrawLine(dc, x + d / 2 - half, y + i, 2 * half, false, c);
    }
}

static void triangle(HDC dc, int x, int y, int rows, int grow, uint8_t c)
{
    for (int i = 0; i < rows; i++) {
        int half = i / 2 + grow;
        if (half > 0)
            DrawLine(dc, x + 16 - half, y + i, 2 * half, false, c);
    }
}

static void paint_icon(HDC dc, int x, int y, unsigned icon)
{
    switch (icon) {
    case MB_ICONHAND: {                             /* a stop sign with a white X */
        uint16_t cross[16];
        for (int r = 0; r < 16; r++) {
            cross[r] = 0;
            if (r >= 3 && r <= 12)
                for (int col = 3; col <= 12; col++)
                    if ((col - r) * (col - r) <= 1 || (col - (15 - r)) * (col - (15 - r)) <= 1)
                        cross[r] |= (uint16_t)(0x8000 >> col);
        }
        octagon(dc, x, y, ICON, C_BLACK);
        octagon(dc, x + 1, y + 1, ICON - 2, C_RED);
        DrawBitmap(dc, x + 8, y + 8, cross, 16, C_WHITE);
        break;
    }
    case MB_ICONEXCLAMATION:                        /* a caution triangle */
        triangle(dc, x, y, ICON, 1, C_BLACK);
        triangle(dc, x, y + 3, ICON - 4, -1, C_YELLOW);
        DrawBitmap(dc, x + 8, y + 12, g_bang, 16, C_BLACK);
        break;
    case MB_ICONQUESTION:
    case MB_ICONASTERISK:                           /* a note bubble */
        circle(dc, x, y, ICON, C_BLACK);
        circle(dc, x + 2, y + 2, ICON - 4, C_ACCENT);
        DrawBitmap(dc, x + 8, y + 8, icon == MB_ICONQUESTION ? g_quest : g_info, 16, C_WHITE);
        break;
    }
}

/* ---- layout ---------------------------------------------------------------- */

static void wrap(struct mbox *mb, const char *s)
{
    int cols = WRAP_W / 8;
    mb->nlines = 0;
    mb->textw = 0;
    while (*s && mb->nlines < LINE_MAX) {
        int n = 0, brk = -1;
        while (s[n] && s[n] != '\n' && n < cols) {
            if (s[n] == ' ') brk = n;
            n++;
        }
        if (s[n] && s[n] != '\n' && s[n] != ' ' && brk > 0)
            n = brk;                                /* break at the last space */
        if (n >= LINE_LEN) n = LINE_LEN - 1;
        memcpy(mb->lines[mb->nlines], s, (size_t)n);
        mb->lines[mb->nlines][n] = '\0';
        if (n * 8 > mb->textw) mb->textw = n * 8;
        mb->nlines++;
        s += n;
        if (*s == '\n' || *s == ' ')
            s++;
    }
}

static void set_buttons(struct mbox *mb)
{
    static const struct button ok = { IDOK, "OK", 0 }, cancel = { IDCANCEL, "Cancel", 0 },
        yes = { IDYES, "Yes", 'y' }, no = { IDNO, "No", 'n' }, abort_ = { IDABORT, "Abort", 'a' },
        retry = { IDRETRY, "Retry", 'r' }, ignore = { IDIGNORE, "Ignore", 'i' };
    struct button *b = mb->buttons;
    switch (mb->type & MB_TYPEMASK) {
    case MB_OKCANCEL:         b[0] = ok; b[1] = cancel; mb->nbuttons = 2; mb->cancel = IDCANCEL; break;
    case MB_ABORTRETRYIGNORE: b[0] = abort_; b[1] = retry; b[2] = ignore; mb->nbuttons = 3; break;
    case MB_YESNOCANCEL:      b[0] = yes; b[1] = no; b[2] = cancel; mb->nbuttons = 3; mb->cancel = IDCANCEL; break;
    case MB_YESNO:            b[0] = yes; b[1] = no; mb->nbuttons = 2; break;
    case MB_RETRYCANCEL:      b[0] = retry; b[1] = cancel; mb->nbuttons = 2; mb->cancel = IDCANCEL; break;
    default:                  b[0] = ok; mb->nbuttons = 1; mb->cancel = IDOK; break;
    }
    mb->def = (int)((mb->type & MB_DEFMASK) >> 8);
    if (mb->def >= mb->nbuttons)
        mb->def = 0;
}

static int text_x(struct mbox *mb) { return (mb->type & MB_ICONMASK) ? MARGIN + ICON + MARGIN : MARGIN; }

static int text_h(struct mbox *mb)
{
    int h = mb->nlines * 16;
    return (mb->type & MB_ICONMASK) && h < ICON ? ICON : h;
}

static void client_size(struct mbox *mb, int *w, int *h)
{
    int bw = mb->nbuttons * BTN_W + (mb->nbuttons - 1) * BTN_GAP + 2 * DEF_RING;
    *w = text_x(mb) + mb->textw + MARGIN;
    if (*w < bw + 2 * MARGIN) *w = bw + 2 * MARGIN;
    if (*w < 200) *w = 200;
    *h = MARGIN + text_h(mb) + MARGIN + DEF_RING + BTN_H + DEF_RING + MARGIN - 4;
}

/* ---- the window ------------------------------------------------------------ */

static struct mbox *mbox_of(HWND h) { return (struct mbox *)(uintptr_t)GetWindowLong(h, GWL_USERDATA); }

static void create_buttons(HWND h, struct mbox *mb)
{
    RECT c;
    GetClientRect(h, &c);
    int x = c.right - MARGIN - DEF_RING - BTN_W;          /* first button rightmost */
    int y = MARGIN + text_h(mb) + MARGIN + DEF_RING - 4;
    for (int i = 0; i < mb->nbuttons; i++, x -= BTN_W + BTN_GAP) {
        bool def = i == mb->def;
        HWND b = CreateWindow(WC_BUTTON, mb->buttons[i].text,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | (def ? BS_DEFPUSHBUTTON : 0),
                              def ? x - DEF_RING : x, def ? y - DEF_RING : y,
                              def ? BTN_W + 2 * DEF_RING : BTN_W, def ? BTN_H + 2 * DEF_RING : BTN_H,
                              h, mb->buttons[i].id, NULL);
        if (def)
            wnd_from(h)->focus_child = wnd_from(b);
    }
}

LRESULT msgbox_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct mbox *mb = mbox_of(h);
    switch (msg) {
    case WM_CREATE:
        SetWindowLong(h, GWL_USERDATA, (int32_t)lp);
        create_buttons(h, (struct mbox *)(uintptr_t)lp);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        int ty = MARGIN + (text_h(mb) - mb->nlines * 16) / 2;
        if (mb->type & MB_ICONMASK)
            paint_icon(dc, MARGIN, MARGIN, mb->type & MB_ICONMASK);
        for (int i = 0; i < mb->nlines; i++)
            TextOut(dc, text_x(mb), ty + i * 16, mb->lines[i], C_BLACK, -1);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDCANCEL && !lp)                  /* Esc */
            id = mb->cancel;
        for (int i = 0; id && i < mb->nbuttons; i++)
            if (mb->buttons[i].id == id) {
                mb->result = id;
                mb->done = true;
            }
        return 0;
    }
    case WM_CLOSE:                                  /* close box, Alt+F4 */
        if (mb->cancel) {
            mb->result = mb->cancel;
            mb->done = true;
        }
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

int MessageBox(HWND owner, const char *text, const char *caption, unsigned type)
{
    struct mbox mb;
    memset(&mb, 0, sizeof mb);
    mb.type = type;
    set_buttons(&mb);
    wrap(&mb, text ? text : "");

    int cw, ch;
    client_size(&mb, &cw, &ch);
    int w = cw + 2 * FRAME, h = ch + CLIENT_TOP + FRAME;
    struct wnd *ow = owner ? wnd_from(owner) : NULL;
    if (ow)
        ow = wnd_top(ow);
    RECT area = { 0, 0, 640, 480 };
    if (ow && ow->visible && ow->state != WS_STATE_MIN)
        frame_outer(ow, &area);
    int x = clampi(area.left + (area.right - area.left - w) / 2, 0, 640 - w);
    int y = clampi(area.top + (area.bottom - area.top - h) / 3, 0, 480 - h);

    bool reenable = ow && ow->enabled;
    if (ow)
        EnableWindow(ow, false);
    HWND box = CreateWindow("#MSGBOX", caption ? caption : "Error", WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                            x, y, w, h, ow, 0, &mb);
    if (!box) {
        if (reenable) EnableWindow(ow, true);
        return 0;
    }
    if (!mb.cancel)                                 /* nothing to cancel with */
        EnableMenuItem(GetSystemMenu(box, false), SC_CLOSE, MF_GRAYED);

    MSG m;
    bool quit = false;
    int quit_code = 0;
    while (!mb.done && IsWindow(box)) {
        if (!GetMessage(&m)) {
            quit = true;
            quit_code = (int)m.wParam;
            break;
        }
        if (m.message == WM_CHAR && GetActiveWindow() == box) {
            char c = (char)m.wParam;
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            for (int i = 0; i < mb.nbuttons; i++)
                if (mb.buttons[i].key && mb.buttons[i].key == c) {
                    SendMessage(GetDlgItem(box, mb.buttons[i].id), BM_CLICK, 0, 0);
                    break;
                }
            if (mb.done)
                break;
        }
        if (!IsDialogMessage(box, &m)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
    }
    if (reenable && wnd_from(ow))
        EnableWindow(ow, true);
    if (IsWindow(box))
        DestroyWindow(box);
    if (ow && wnd_from(ow))
        SetActiveWindow(ow);
    if (quit)
        PostQuitMessage(quit_code);
    return mb.result;
}
