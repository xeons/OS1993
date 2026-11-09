/*
 * user/apps/clock.c -- CLOCK.BIN, an analog or digital clock.
 *
 * The time comes from the real-time clock (GetLocalTime). The analog face
 * scales with the window; its hands use a sine table (no FPU). The digital
 * face draws seven-segment digits. Settings: analog or digital, seconds.
 */
#include "os1993.h"

enum { ID_ANALOG = 100, ID_DIGITAL, ID_SECONDS, ID_ABOUT };

static HWND clock_wnd;
static HMENU settings;
static bool digital, seconds = true;
static SYSTEMTIME now;

/* sin(i * 6 degrees) * 1000, for a quarter turn */
static const int16_t sine[16] = { 0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914,
                                  951, 978, 995, 1000 };

static int sin60(int i)
{
    i = ((i % 60) + 60) % 60;
    if (i <= 15) return sine[i];
    if (i <= 30) return sine[30 - i];
    if (i <= 45) return -sine[i - 30];
    return -sine[60 - i];
}

static int cos60(int i) { return sin60(i + 15); }

/* ---- analog ---------------------------------------------------------------- */

static void hand(HDC dc, int cx, int cy, int pos, int len, int thick, uint8_t c)
{
    int x = cx + len * sin60(pos) / 1000, y = cy - len * cos60(pos) / 1000;
    for (int d = -(thick / 2); d <= thick / 2; d++) {
        DrawLineEx(dc, cx + d, cy, x + d, y, c);
        DrawLineEx(dc, cx, cy + d, x, y + d, c);
    }
}

static void analog(HDC dc, RECT c)
{
    int w = c.right, h = c.bottom, r = (w < h ? w : h) / 2 - 8;
    if (r < 16)
        return;
    int cx = w / 2, cy = h / 2;
    RECT face = { cx - r, cy - r, cx + r + 1, cy + r + 1 };
    DrawEllipse(dc, &face, C_WHITE, true);
    DrawEllipse(dc, &face, C_GRAY2, false);
    for (int i = 0; i < 60; i++) {                      /* the dial */
        int mr = r - 6, x = cx + mr * sin60(i) / 1000, y = cy - mr * cos60(i) / 1000;
        if (i % 5 == 0) {
            int s = i % 15 == 0 ? 3 : 2;
            RECT m = { x - s, y - s, x + s + 1, y + s + 1 };
            FillRect(dc, &m, i % 15 == 0 ? C_ACCENT : C_GRAY2);
        } else {
            SetPixel(dc, x, y, C_GRAY4);
        }
    }
    int hpos = (now.hour % 12) * 5 + now.minute / 12;
    hand(dc, cx, cy, hpos, r * 5 / 10, 3, C_BLACK);
    hand(dc, cx, cy, now.minute, r * 8 / 10, 2, C_BLACK);
    if (seconds)
        hand(dc, cx, cy, now.second, r * 9 / 10, 0, C_RED);
    RECT hub = { cx - 2, cy - 2, cx + 3, cy + 3 };
    FillRect(dc, &hub, C_BLACK);
}

/* ---- digital ------------------------------------------------------------------ */

/* Segments a-g of 0-9 (bit 0 = a ... bit 6 = g) */
static const uint8_t seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

static void digit(HDC dc, int x, int y, int w, int h, int d)
{
    int t = w / 5 > 2 ? w / 5 : 2, half = h / 2;
    RECT s[7] = {
        { x + t, y, x + w - t, y + t },                         /* a */
        { x + w - t, y + t, x + w, y + half },                  /* b */
        { x + w - t, y + half, x + w, y + h - t },              /* c */
        { x + t, y + h - t, x + w - t, y + h },                 /* d */
        { x, y + half, x + t, y + h - t },                      /* e */
        { x, y + t, x + t, y + half },                          /* f */
        { x + t, y + half - t / 2, x + w - t, y + half + t - t / 2 },  /* g */
    };
    for (int i = 0; i < 7; i++)
        FillRect(dc, &s[i], (seg[d] >> i) & 1 ? C_ACCENT_DK : C_LIGHT);
}

static void digital_face(HDC dc, RECT c)
{
    int n = seconds ? 6 : 4, gaps = seconds ? 2 : 1;
    int dw = (c.right - 16) / (n + gaps), dh = dw * 2;
    if (dh > c.bottom - 40) { dh = c.bottom - 40; dw = dh / 2; }
    if (dw < 6)
        return;
    int total = n * dw + (n - 1) * (dw / 4) + gaps * (dw / 2);
    int x = (c.right - total) / 2, y = (c.bottom - dh - 20) / 2;
    int v[6] = { now.hour / 10, now.hour % 10, now.minute / 10, now.minute % 10,
                 now.second / 10, now.second % 10 };
    for (int i = 0; i < n; i++) {
        if (i == 2 || i == 4) {                         /* a colon */
            int cx = x + dw / 8, s = dw / 5 > 2 ? dw / 5 : 2;
            RECT a = { cx, y + dh / 3 - s / 2, cx + s, y + dh / 3 + s - s / 2 };
            RECT b = { cx, y + 2 * dh / 3 - s / 2, cx + s, y + 2 * dh / 3 + s - s / 2 };
            FillRect(dc, &a, C_ACCENT_DK);
            FillRect(dc, &b, C_ACCENT_DK);
            x += dw / 2;
        }
        digit(dc, x, y, dw, dh, v[i]);
        x += dw + dw / 4;
    }
    static const char *const days[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                         "Friday", "Saturday" };
    static const char *const months[12] = { "January", "February", "March", "April", "May", "June",
                                            "July", "August", "September", "October", "November",
                                            "December" };
    char date[48];
    int k = 0;
    for (const char *s = days[now.day_of_week % 7]; *s; ) date[k++] = *s++;
    date[k++] = ' ';
    if (now.day >= 10) date[k++] = (char)('0' + now.day / 10);
    date[k++] = (char)('0' + now.day % 10);
    date[k++] = ' ';
    for (const char *s = months[(now.month + 11) % 12]; *s; ) date[k++] = *s++;
    date[k] = '\0';
    TextOut(dc, (c.right - TextWidth(date)) / 2, y + dh + 8, date, C_BLACK, -1);
}

/* ---- the window ---------------------------------------------------------------- */

static void update_menu(void)
{
    CheckMenuRadioItem(settings, ID_ANALOG, ID_DIGITAL, digital ? ID_DIGITAL : ID_ANALOG, MF_BYCOMMAND);
    CheckMenuItem(settings, ID_SECONDS, seconds ? MF_CHECKED : MF_UNCHECKED);
}

static LRESULT clock_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        GetLocalTime(&now);
        SetTimer(h, 1, 500);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT c;
        GetClientRect(h, &c);
        if (digital) digital_face(dc, c);
        else analog(dc, c);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER: {
        SYSTEMTIME t;
        GetLocalTime(&t);
        if (t.second != now.second || t.minute != now.minute) {
            bool minute = t.minute != now.minute;
            now = t;
            if (seconds || minute)
                InvalidateRect(h, NULL);
        }
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(h, NULL);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ANALOG:  digital = false; break;
        case ID_DIGITAL: digital = true; break;
        case ID_SECONDS: seconds = !seconds; break;
        case ID_ABOUT:   ShellAbout(h, "Clock", NULL); return 0;
        }
        DebugLog("clock %s%s", digital ? "digital" : "analog", seconds ? " with seconds" : "");
        update_menu();
        InvalidateRect(h, NULL);
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

int main(void)
{
    InitUI();
    WNDCLASS wc = { .name = "ClockWindow", .proc = clock_proc, .background = C_FACE, .icon = ICON_CLOCK };
    RegisterClass(&wc);
    HMENU help = CreatePopupMenu(), bar = CreateMenu();
    settings = CreatePopupMenu();
    AppendMenu(settings, MF_STRING, ID_ANALOG, "&Analog");
    AppendMenu(settings, MF_STRING, ID_DIGITAL, "&Digital");
    AppendMenu(settings, MF_SEPARATOR, 0, NULL);
    AppendMenu(settings, MF_STRING, ID_SECONDS, "&Seconds");
    AppendMenu(help, MF_STRING, ID_ABOUT, "&About Clock");
    AppendMenu(bar, MF_POPUP, (uintptr_t)settings, "&Settings");
    AppendMenu(bar, MF_POPUP, (uintptr_t)help, "&Help");
    update_menu();
    clock_wnd = CreateWindow("ClockWindow", "Clock", WS_OVERLAPPEDWINDOW, 410, 50, 200, 232,
                             NULL, 0, NULL);
    SetMenu(clock_wnd, bar);
    ShowWindow(clock_wnd, SW_SHOW);
    DebugLog("clock ready");

    MSG msg;
    while (GetMessage(&msg)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
