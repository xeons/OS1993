/*
 * user/apps/explorer.c -- EXPLORER.BIN, the file explorer.
 *
 * A tree of My Computer, its drives and their folders on the left (folders
 * filled in as they are opened), the folder chosen on the right in a list
 * view -- large icons, small icons, a list or details (name, size, type,
 * modified; a click on a header sorts by it) -- an address field and an Up
 * button above, a status bar below; the bar between the panes moves.
 *
 * Enter or a double-click opens: a folder or drive goes there, a program
 * (.BIN) runs, a text file opens in Notepad. Backspace goes up, Tab moves
 * between the panes, Delete deletes (asking first; folders with all they
 * hold), F2 renames in place, F5 reads the folder again, File > New Folder
 * makes one and lets you name it, Properties tells about it. EXPLORER.BIN
 * path starts there; without it, at My Computer.
 *
 * The toolbar has Back and Forward (Alt+Left, Alt+Right: the folders
 * visited), Up, Cut, Copy, Paste, Delete, Properties and the views. Cut and
 * Copy put the selected files on the clipboard (cut ones fade until they
 * are pasted, which moves them); Paste copies or moves them here. Items
 * dragged onto a folder or drive (in either pane) move there -- or are
 * copied, to another drive or with Ctrl held. What a file is called, its
 * icon and what opens it come from FILETYPES.INI ([Types] EXT=Name,icon,
 * PROGRAM.BIN), written with the usual ones if it isn't there.
 */
#include <stdio.h>
#include <string.h>
#include "os1993.h"

enum { ID_TREE = 1, ID_LIST, ID_ADDR, ID_UP, ID_TOOLBAR, ID_BACK, ID_FORWARD, ID_CUT, ID_COPY, ID_PASTE,
       ID_OPEN = 100, ID_NEWFOLDER, ID_DELETE, ID_RENAME, ID_PROPS, ID_CLOSE, ID_SELALL, ID_INVERT,
       ID_LARGE, ID_SMALL, ID_LISTV, ID_DETAILS, ID_BYNAME, ID_BYTYPE, ID_BYSIZE, ID_BYDATE, ID_REFRESH,
       ID_GOUP, ID_GOCOMPUTER, ID_ABOUT, ID_FOCUSADDR };

enum { IMG_COMPUTER, IMG_DRIVE, IMG_FLOPPY, IMG_FOLDER, IMG_FOLDER_OPEN, IMG_DOC, IMG_TEXT, IMG_APP,
       IMG_DOS, IMG_CALC, IMG_PAINT, IMG_CLOCK, IMG_NOTEPAD, IMG_DEMO, IMG_EXPLORER };

#define BAR_H     30            /* the address bar */
#define STATUS_H  22
#define SPLIT_W   5
#define NAME_MAX  260

struct entry {
    char     name[NAME_MAX];
    uint64_t size;
    uint32_t attr;
    uint16_t date, time;
    bool     dir;
    char     letter;            /* My Computer: a drive */
    uint64_t total, free_b;     /* ... its size */
    char     type[40];
};

struct node { bool filled; char path[MAX_PATH]; };      /* a tree item's lParam */

static HWND win, tree, list, addr, upbtn, toolbar;
#define HIST_MAX 32
static char back_[HIST_MAX][MAX_PATH], fwd_[HIST_MAX][MAX_PATH];
static int nback, nfwd;
static bool dragging;                           /* items being dragged */
static char drag_from[4096];                    /* their paths, NUL after each */
static char drop_path[MAX_PATH];                /* the folder under the pointer, "" none */
static int drop_item = -1;
static HTREEITEM drop_node;

/* File types: FILETYPES.INI's [Types] (EXT=Name,icon,PROGRAM) */
struct ftype { char ext[8], name[40], prog[16]; int img; };
static struct ftype types[48];
static int ntypes;
static bool started;                            /* past the first folder (no Back to before it) */
static HMENU view_menu, file_menu, edit_menu;
static HIMAGELIST il_small, il_large;
static HTREEITEM computer;
static char cur[MAX_PATH];                  /* "" = My Computer */
static struct entry *ents;
static int nents, cap_ents;
static int sort_col, tree_w = 190;
static bool sort_desc, syncing, splitting;
static WNDPROC addr_old;
static char status_left[64], status_right[96];

/* ---- small things ---------------------------------------------------------------------- */

static char up_(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

static int cmp_nocase(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = up_(*a), y = up_(*b);
        if (x != y || !x)
            return x - y;
    }
}

static const char *ext_of(const char *name)
{
    const char *dot = NULL;
    for (; *name; name++)
        if (*name == '.')
            dot = name + 1;
    return dot ? dot : "";
}

static bool ext_is(const char *name, const char *list_)     /* "TXT INI LOG" */
{
    const char *e = ext_of(name);
    if (!*e)
        return false;
    for (const char *p = list_; *p;) {
        const char *q = p;
        while (*q && *q != ' ')
            q++;
        size_t n = (size_t)(q - p);
        if (strlen(e) == n) {
            size_t k = 0;
            while (k < n && up_(e[k]) == p[k])
                k++;
            if (k == n)
                return true;
        }
        p = *q ? q + 1 : q;
    }
    return false;
}

static void join(char *out, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    strncpy0(out, dir, MAX_PATH);
    if (n && out[n - 1] != '\\' && n + 1 < MAX_PATH)
        out[n++] = '\\', out[n] = '\0';
    strncpy0(out + n, name, MAX_PATH - n);
}

static const char *base_of(const char *path)
{
    const char *b = path;
    for (; *path; path++)
        if (*path == '\\' && path[1])
            b = path + 1;
    return b;
}

static void parent_of(const char *path, char *out)             /* "C:\A\B" -> "C:\A", "C:\" -> "" */
{
    size_t n = strlen(path);
    if (n <= 3) {
        out[0] = '\0';
        return;
    }
    strncpy0(out, path, MAX_PATH);
    char *s = out + strlen(out) - 1;
    if (*s == '\\')
        *s-- = '\0';
    while (s > out && *s != '\\')
        s--;
    if (s - out <= 2)
        s[1] = '\0';                                    /* "C:\" */
    else
        *s = '\0';
}

static void fmt_kb(uint64_t bytes, char *out, int size)    /* "1,234 KB" */
{
    uint64_t kb = (bytes + 1023) / 1024;
    char digits[24], t[32];
    int n = 0, k = 0;
    do {
        digits[n++] = (char)('0' + (int)(kb % 10));
        kb /= 10;
    } while (kb && n < 20);
    for (int i = n - 1; i >= 0; i--) {
        t[k++] = digits[i];
        if (i && i % 3 == 0)
            t[k++] = ',';
    }
    t[k] = '\0';
    snprintf(out, (size_t)size, "%s KB", t);
}

static void fmt_size(uint64_t bytes, char *out, int size)  /* "12.3 MB" */
{
    if (bytes >= 1024u * 1024u * 1024u)
        snprintf(out, (size_t)size, "%u.%u GB", (unsigned)(bytes >> 30), (unsigned)((bytes >> 20) % 1024 * 10 / 1024));
    else if (bytes >= 1024u * 1024u)
        snprintf(out, (size_t)size, "%u.%u MB", (unsigned)(bytes >> 20), (unsigned)((bytes >> 10) % 1024 * 10 / 1024));
    else
        fmt_kb(bytes, out, size);
}

static void fmt_date(uint16_t d, uint16_t t, char *out, int size)
{
    int hour = t >> 11, min = (t >> 5) & 63, h12 = hour % 12 ? hour % 12 : 12;
    snprintf(out, (size_t)size, "%u/%u/%u %u:%02u %s", (d >> 5) & 15, d & 31, 1980 + (d >> 9), h12, min,
             hour < 12 ? "AM" : "PM");
}

static const char *const std_types[] = {
    "TXT=Text Document,text,NOTEPAD.BIN", "LOG=Text Document,text,NOTEPAD.BIN",
    "INI=Configuration Settings,text,NOTEPAD.BIN", "CFG=Configuration Settings,text,NOTEPAD.BIN",
    "SYS=System File,document,NOTEPAD.BIN", "BMP=Bitmap Image,paint,PAINT.BIN",
    "BIN=Application,app,", "COM=MS-DOS Application,dos,", "EXE=MS-DOS Application,dos,",
    "BAT=MS-DOS Batch File,dos,NOTEPAD.BIN", "DLL=Application Extension,document,",
    "C=C Source,text,NOTEPAD.BIN", "H=C Header,text,NOTEPAD.BIN", "ASM=Assembly Source,text,NOTEPAD.BIN",
    "MD=Text Document,text,NOTEPAD.BIN", "DEF=Module Definition,text,NOTEPAD.BIN",
    "LST=Listing,text,NOTEPAD.BIN", "PRN=Printer File,text,NOTEPAD.BIN",
};

static void load_types(void)
{
    char buf[2048];
    if (!GetPrivateProfileSection("Types", buf, sizeof buf, "FILETYPES.INI")) {
        for (unsigned i = 0; i < sizeof std_types / sizeof std_types[0]; i++) {
            char k[8], *eq = strchr(std_types[i], '=');
            int kl = (int)(eq - std_types[i]);
            memcpy(k, std_types[i], (size_t)kl);
            k[kl] = '\0';
            WritePrivateProfileString("Types", k, eq + 1, "FILETYPES.INI");
        }
        if (!GetPrivateProfileSection("Types", buf, sizeof buf, "FILETYPES.INI")) {   /* read-only: built in */
            int k = 0;
            for (unsigned i = 0; i < sizeof std_types / sizeof std_types[0]; i++) {
                int n = (int)strlen(std_types[i]);
                if (k + n + 2 > (int)sizeof buf) break;
                memcpy(buf + k, std_types[i], (size_t)n + 1);
                k += n + 1;
            }
            buf[k] = '\0';
        }
    }
    static const struct { const char *name; int img; } icons[] = {
        { "text", IMG_TEXT }, { "document", IMG_DOC }, { "app", IMG_APP }, { "dos", IMG_DOS },
        { "paint", IMG_PAINT }, { "calc", IMG_CALC }, { "notepad", IMG_NOTEPAD }, { "folder", IMG_FOLDER },
    };
    ntypes = 0;
    for (char *p = buf; *p && ntypes < (int)(sizeof types / sizeof types[0]); p += strlen(p) + 1) {
        char *eq = strchr(p, '=');
        if (!eq) continue;
        struct ftype *t = &types[ntypes++];
        memset(t, 0, sizeof *t);
        int kl = (int)(eq - p) < 7 ? (int)(eq - p) : 7;
        for (int i = 0; i < kl; i++) t->ext[i] = up_(p[i]);
        char *f1 = eq + 1, *f2 = strchr(f1, ','), *f3 = f2 ? strchr(f2 + 1, ',') : NULL;
        int nl = f2 ? (int)(f2 - f1) : (int)strlen(f1);
        if (nl > 39) nl = 39;
        memcpy(t->name, f1, (size_t)nl);
        t->img = IMG_DOC;
        if (f2) {
            int il = f3 ? (int)(f3 - f2 - 1) : (int)strlen(f2 + 1);
            for (unsigned k = 0; k < sizeof icons / sizeof icons[0]; k++)
                if ((int)strlen(icons[k].name) == il && !strncmp(icons[k].name, f2 + 1, (size_t)il))
                    t->img = icons[k].img;
        }
        if (f3) strncpy0(t->prog, f3 + 1, sizeof t->prog);
    }
}

static const struct ftype *type_of(const char *name)
{
    const char *e = ext_of(name);
    if (!*e) return NULL;
    char u[8];
    int k = 0;
    for (; e[k] && k < 7; k++) u[k] = up_(e[k]);
    u[k] = '\0';
    if (e[k]) return NULL;
    for (int i = 0; i < ntypes; i++)
        if (!strcmp(types[i].ext, u))
            return &types[i];
    return NULL;
}

static int icon_for(const struct entry *e)
{
    if (e->letter)
        return e->letter < 'C' ? IMG_FLOPPY : IMG_DRIVE;
    if (e->dir)
        return IMG_FOLDER;
    static const struct { const char *name; int img; } known[] = {
        { "CALC.BIN", IMG_CALC }, { "PAINT.BIN", IMG_PAINT }, { "CLOCK.BIN", IMG_CLOCK },
        { "NOTEPAD.BIN", IMG_NOTEPAD }, { "DEMO.BIN", IMG_DEMO }, { "EXPLORER.BIN", IMG_EXPLORER },
    };
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!cmp_nocase(e->name, known[i].name))
            return known[i].img;
    const struct ftype *t = type_of(e->name);
    return t ? t->img : IMG_DOC;
}

static void type_for(struct entry *e)
{
    if (e->letter) {
        snprintf(e->type, sizeof e->type, e->letter < 'C' ? "Floppy Disk" : "Local Disk (%s)", e->name + 0);
        return;
    }
    if (e->dir)                         { strncpy0(e->type, "File Folder", sizeof e->type); return; }
    const struct ftype *ft = type_of(e->name);
    if (ft && ft->name[0])              { strncpy0(e->type, ft->name, sizeof e->type); return; }
    const char *x = ext_of(e->name);
    if (!*x) {
        strncpy0(e->type, "File", sizeof e->type);
        return;
    }
    char u[16];
    int k = 0;
    for (; x[k] && k < 15; k++)
        u[k] = up_(x[k]);
    u[k] = '\0';
    snprintf(e->type, sizeof e->type, "%s File", u);
}

static bool good_name(const char *s)
{
    if (!*s)
        return false;
    for (; *s; s++)
        if (strchr("\\/:*?\"<>|", *s))
            return false;
    return true;
}

/* ---- the folder's entries ---------------------------------------------------------------- */

static struct entry *add_entry(void)
{
    if (nents == cap_ents) {
        int cap = cap_ents ? cap_ents * 2 : 64;
        struct entry *p = ents ? LocalReAlloc(ents, (uint32_t)cap * sizeof *p, 0)
                               : LocalAlloc(LMEM_FIXED, (uint32_t)cap * sizeof *p);
        if (!p)
            return NULL;
        ents = p;
        cap_ents = cap;
    }
    struct entry *e = &ents[nents++];
    memset(e, 0, sizeof *e);
    return e;
}

static void drive_name(char letter, char *out, int size)   /* "DATAE4 (J:)" */
{
    char root[4] = { letter, ':', '\\', 0 }, label[16] = "";
    if (letter >= 'C')
        GetVolumeInformation(root, label, sizeof label, NULL, NULL, NULL, NULL, 0);
    if (label[0])
        snprintf(out, (size_t)size, "%s (%c:)", label, letter);
    else
        snprintf(out, (size_t)size, "%s (%c:)", letter < 'C' ? "Floppy" : "Disk", letter);
}

static void load_entries(void)
{
    nents = 0;
    if (!cur[0]) {                                      /* My Computer: the drives */
        uint32_t mask = GetLogicalDrives();
        for (int d = 0; d < 26; d++) {
            if (!(mask & (1u << d)))
                continue;
            struct entry *e = add_entry();
            if (!e)
                return;
            e->letter = (char)('A' + d);
            drive_name(e->letter, e->name, sizeof e->name);
            char root[4] = { e->letter, ':', '\\', 0 }, fs[16] = "";
            if (e->letter >= 'C') {
                GetVolumeInformation(root, NULL, 0, NULL, NULL, NULL, fs, sizeof fs);
                uint32_t spc, bps, fr, total;
                if (GetDiskFreeSpace(root, &spc, &bps, &fr, &total)) {
                    e->total = (uint64_t)total * spc * bps;
                    e->free_b = (uint64_t)fr * spc * bps;
                }
            }
            snprintf(e->type, sizeof e->type, e->letter < 'C' ? "Floppy Disk" : "Local Disk (%s)", fs);
            e->dir = true;
        }
        return;
    }
    char pat[MAX_PATH];
    join(pat, cur, "*.*");
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!strcmp(fd.name, ".") || !strcmp(fd.name, "..") || (fd.attrib & 0x08))
            continue;
        struct entry *e = add_entry();
        if (!e)
            break;
        strncpy0(e->name, fd.name, sizeof e->name);
        e->size = fd.size | (uint64_t)fd.size_high << 32;
        e->attr = fd.attrib;
        e->date = fd.date;
        e->time = fd.time;
        e->dir = fd.attrib & FILE_ATTRIBUTE_DIRECTORY;
        type_for(e);
    } while (FindNextFile(f, &fd));
    FindClose(f);
}

/* Folders first, then by the sort column (names break ties). */
static int compare(LPARAM a, LPARAM b, LPARAM sort)
{
    const struct entry *x = &ents[a], *y = &ents[b];
    (void)sort;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    int r = 0;
    switch (cur[0] ? sort_col : (sort_col == 0 ? 0 : -1)) {
    case 1:
        r = x->size < y->size ? -1 : x->size > y->size ? 1 : 0;
        break;
    case 2:
        r = cmp_nocase(x->type, y->type);
        break;
    case 3: {
        uint32_t dx = (uint32_t)x->date << 16 | x->time, dy = (uint32_t)y->date << 16 | y->time;
        r = dx < dy ? -1 : dx > dy ? 1 : 0;
        break;
    }
    case -1:                                            /* drives: by letter */
        r = x->letter - y->letter;
        break;
    }
    if (!r)
        r = cur[0] ? cmp_nocase(x->name, y->name) : x->letter - y->letter;
    return sort_desc ? -r : r;
}

/* ---- the list --------------------------------------------------------------------------- */

static void set_columns(void)
{
    while (SendMessage(list, LVM_DELETECOLUMN, 0, 0))
        ;
    static const char *const folder_cols[] = { "Name", "Size", "Type", "Modified" };
    static const char *const drive_cols[] = { "Name", "Type", "Total Size", "Free Space" };
    static const int widths[] = { 190, 80, 150, 150 };
    for (int c = 0; c < 4; c++) {
        bool right = cur[0] ? c == 1 : c >= 2;
        LVCOLUMN col = { LVCF_TEXT | LVCF_WIDTH | LVCF_FMT, right ? LVCFMT_RIGHT : LVCFMT_LEFT, widths[c],
                         (char *)(cur[0] ? folder_cols[c] : drive_cols[c]), 0, c };
        SendMessage(list, LVM_INSERTCOLUMN, (WPARAM)c, (LPARAM)&col);
    }
}

static void set_sub(int item, int col, const char *text)
{
    LVITEM it = { .mask = LVIF_TEXT, .iItem = item, .iSubItem = col, .pszText = (char *)text };
    SendMessage(list, LVM_SETITEMTEXT, (WPARAM)item, (LPARAM)&it);
}

static void update_status(void)
{
    int sel = (int)SendMessage(list, LVM_GETSELECTEDCOUNT, 0, 0);
    uint64_t bytes = 0;
    for (int i = -1; (i = (int)SendMessage(list, LVM_GETNEXTITEM, (WPARAM)i, sel ? LVNI_SELECTED : LVNI_ALL)) >= 0;) {
        LVITEM it = { .mask = LVIF_PARAM, .iItem = i };
        SendMessage(list, LVM_GETITEM, 0, (LPARAM)&it);
        if (!ents[it.lParam].dir)
            bytes += ents[it.lParam].size;
    }
    if (sel)
        snprintf(status_left, sizeof status_left, "%d object(s) selected", sel);
    else
        snprintf(status_left, sizeof status_left, "%d object(s)", nents);
    status_right[0] = '\0';
    if (cur[0]) {
        char b[24], f[24];
        uint32_t spc, bps, fr, total;
        char root[4] = { cur[0], ':', '\\', 0 };
        fmt_size(bytes, b, sizeof b);
        if (GetDiskFreeSpace(root, &spc, &bps, &fr, &total)) {
            fmt_size((uint64_t)fr * spc * bps, f, sizeof f);
            snprintf(status_right, sizeof status_right, "%s (Free: %s)", b, f);
        } else {
            strncpy0(status_right, b, sizeof status_right);
        }
    }
    RECT r;
    GetClientRect(win, &r);
    r.top = r.bottom - STATUS_H;
    InvalidateRect(win, &r);
}

static void fill_list(const char *select_name)
{
    SendMessage(list, LVM_DELETEALLITEMS, 0, 0);
    set_columns();
    load_entries();
    for (int i = 0; i < nents; i++) {
        struct entry *e = &ents[i];
        LVITEM it = { .mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM, .iItem = i, .pszText = e->name,
                      .iImage = icon_for(e), .lParam = i };
        int at = (int)SendMessage(list, LVM_INSERTITEM, 0, (LPARAM)&it);
        char t[48];
        if (!cur[0]) {
            set_sub(at, 1, e->type);
            if (e->total) {
                fmt_size(e->total, t, sizeof t);
                set_sub(at, 2, t);
                fmt_size(e->free_b, t, sizeof t);
                set_sub(at, 3, t);
            }
            continue;
        }
        if (!e->dir) {
            fmt_kb(e->size, t, sizeof t);
            set_sub(at, 1, t);
        }
        set_sub(at, 2, e->type);
        fmt_date(e->date, e->time, t, sizeof t);
        set_sub(at, 3, t);
    }
    SendMessage(list, LVM_SORTITEMS, 0, (LPARAM)compare);
    int pick = 0;
    if (select_name)
        for (int i = 0; i < nents; i++) {
            LVITEM it = { .mask = LVIF_PARAM, .iItem = i };
            SendMessage(list, LVM_GETITEM, 0, (LPARAM)&it);
            if (!cmp_nocase(ents[it.lParam].name, select_name)) {
                pick = i;
                LVITEM st = { .state = LVIS_SELECTED | LVIS_FOCUSED, .stateMask = LVIS_SELECTED | LVIS_FOCUSED };
                SendMessage(list, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&st);
                break;
            }
        }
    if (!select_name && nents) {
        LVITEM st = { .state = LVIS_FOCUSED, .stateMask = LVIS_FOCUSED };
        SendMessage(list, LVM_SETITEMSTATE, 0, (LPARAM)&st);
    }
    SendMessage(list, LVM_ENSUREVISIBLE, (WPARAM)pick, 0);
    update_status();
}

static int entry_at(int item)
{
    LVITEM it = { .mask = LVIF_PARAM, .iItem = item };
    return SendMessage(list, LVM_GETITEM, 0, (LPARAM)&it) ? (int)it.lParam : -1;
}

/* ---- the tree --------------------------------------------------------------------------- */

static struct node *node_of(HTREEITEM it)
{
    TVITEM t = { .mask = TVIF_PARAM, .hItem = it };
    return SendMessage(tree, TVM_GETITEM, 0, (LPARAM)&t) ? (struct node *)t.lParam : NULL;
}

static HTREEITEM add_node(HTREEITEM parent, const char *text, const char *path, int img, int simg, bool kids)
{
    struct node *n = LocalAlloc(LMEM_ZEROINIT, sizeof *n);
    if (!n)
        return NULL;
    strncpy0(n->path, path, sizeof n->path);
    TVINSERTSTRUCT ins = { parent, parent == computer ? TVI_LAST : TVI_SORT,
                           { TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_CHILDREN, NULL, 0, 0,
                             (char *)text, 0, img, simg, kids ? 1 : 0, (LPARAM)n } };
    HTREEITEM it = (HTREEITEM)(uintptr_t)SendMessage(tree, TVM_INSERTITEM, 0, (LPARAM)&ins);
    if (!it)
        LocalFree(n);
    return it;
}

/* An item's folders, read the first time it opens. */
static void populate(HTREEITEM it)
{
    struct node *n = node_of(it);
    if (!n || n->filled || it == computer)
        return;
    n->filled = true;
    char pat[MAX_PATH], path[MAX_PATH];
    join(pat, n->path, "*.*");
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(fd.attrib & FILE_ATTRIBUTE_DIRECTORY) || !strcmp(fd.name, ".") || !strcmp(fd.name, ".."))
            continue;
        join(path, n->path, fd.name);
        add_node(it, fd.name, path, IMG_FOLDER, IMG_FOLDER_OPEN, true);
    } while (FindNextFile(f, &fd));
    FindClose(f);
}

/* Read an item's folders again (after a change), keeping it open if it was. */
static void repopulate(HTREEITEM it)
{
    struct node *n = node_of(it);
    if (!n || it == computer || !n->filled)
        return;
    TVITEM t = { .mask = TVIF_STATE, .hItem = it, .stateMask = TVIS_EXPANDED };
    SendMessage(tree, TVM_GETITEM, 0, (LPARAM)&t);
    syncing = true;
    HTREEITEM c;
    while ((c = (HTREEITEM)(uintptr_t)SendMessage(tree, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)it)))
        SendMessage(tree, TVM_DELETEITEM, 0, (LPARAM)c);
    n->filled = false;
    populate(it);
    TVITEM k = { .mask = TVIF_CHILDREN, .hItem = it, .cChildren = 1 };
    SendMessage(tree, TVM_SETITEM, 0, (LPARAM)&k);
    if (t.state & TVIS_EXPANDED)
        SendMessage(tree, TVM_EXPAND, TVE_EXPAND, (LPARAM)it);
    syncing = false;
}

/* The tree item for a path, opening its parents on the way (or NULL). */
static HTREEITEM find_node(const char *path)
{
    if (!path[0])
        return computer;
    HTREEITEM it = (HTREEITEM)(uintptr_t)SendMessage(tree, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)computer);
    for (; it; it = (HTREEITEM)(uintptr_t)SendMessage(tree, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)it)) {
        struct node *n = node_of(it);
        if (n && up_(n->path[0]) == up_(path[0]))
            break;
    }
    const char *rest = path + 3;
    while (it && *rest) {
        char comp[NAME_MAX];
        int k = 0;
        while (*rest && *rest != '\\' && k < NAME_MAX - 1)
            comp[k++] = *rest++;
        comp[k] = '\0';
        if (*rest == '\\')
            rest++;
        populate(it);
        SendMessage(tree, TVM_EXPAND, TVE_EXPAND, (LPARAM)it);
        HTREEITEM c = (HTREEITEM)(uintptr_t)SendMessage(tree, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)it);
        for (; c; c = (HTREEITEM)(uintptr_t)SendMessage(tree, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)c)) {
            char t[NAME_MAX];
            TVITEM ti = { .mask = TVIF_TEXT, .hItem = c, .pszText = t, .cchTextMax = sizeof t };
            SendMessage(tree, TVM_GETITEM, 0, (LPARAM)&ti);
            if (!cmp_nocase(t, comp))
                break;
        }
        it = c;
    }
    return it;
}

static void fill_tree(void)
{
    SendMessage(tree, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);
    computer = add_node(TVI_ROOT, "My Computer", "", IMG_COMPUTER, IMG_COMPUTER, false);
    uint32_t mask = GetLogicalDrives();
    for (int d = 0; d < 26; d++)
        if (mask & (1u << d)) {
            char name[48], root[4] = { (char)('A' + d), ':', '\\', 0 };
            drive_name((char)('A' + d), name, sizeof name);
            add_node(computer, name, root, d < 2 ? IMG_FLOPPY : IMG_DRIVE, d < 2 ? IMG_FLOPPY : IMG_DRIVE, true);
        }
    SendMessage(tree, TVM_EXPAND, TVE_EXPAND, (LPARAM)computer);
}

/* ---- going places ------------------------------------------------------------------------- */

static void layout(void);

static void nav_buttons(void)
{
    SendMessage(toolbar, TB_ENABLEBUTTON, ID_BACK, MAKELONG(nback > 0, 0));
    SendMessage(toolbar, TB_ENABLEBUTTON, ID_FORWARD, MAKELONG(nfwd > 0, 0));
}

static void push(char (*stack)[MAX_PATH], int *n, const char *path)
{
    if (*n == HIST_MAX) {
        memmove(stack[0], stack[1], (size_t)(HIST_MAX - 1) * MAX_PATH);
        (*n)--;
    }
    strncpy0(stack[(*n)++], path, MAX_PATH);
}

static void go(const char *path, const char *select_name, int how);

static void navigate(const char *path, const char *select_name)
{
    go(path, select_name, 0);
}

/* how: 0 a new place (Forward is forgotten), 1 Back, 2 Forward. */
static void go(const char *path, const char *select_name, int how)
{
    char before[MAX_PATH];
    strncpy0(before, cur, sizeof before);
    char full[MAX_PATH] = "";
    if (path[0]) {
        if (!GetFullPathName(path, sizeof full, full, NULL))
            strncpy0(full, path, sizeof full);
        uint32_t a = GetFileAttributes(full);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            char text[MAX_PATH + 64];
            snprintf(text, sizeof text, "%s\nThis folder can't be opened.", full);
            MessageBox(win, text, "Explorer", MB_OK | MB_ICONERROR);
            return;
        }
        size_t n = strlen(full);
        if (n > 3 && full[n - 1] == '\\')
            full[n - 1] = '\0';
    }
    if (strcmp(before, full) || how) {              /* the history */
        if (how == 1) push(fwd_, &nfwd, before);
        else if (how == 2) push(back_, &nback, before);
        else if (strcmp(before, full) && started) {
            push(back_, &nback, before);
            nfwd = 0;
        }
    }
    strncpy0(cur, full, sizeof cur);
    nav_buttons();
    SetWindowText(addr, cur[0] ? cur : "My Computer");
    char title[MAX_PATH + 16];
    snprintf(title, sizeof title, "Exploring - %s", cur[0] ? cur : "My Computer");
    SetWindowText(win, title);
    SendMessage(toolbar, TB_ENABLEBUTTON, ID_UP, MAKELONG(cur[0] != 0, 0));
    SendMessage(toolbar, TB_ENABLEBUTTON, ID_CUT, MAKELONG(cur[0] != 0, 0));
    SendMessage(toolbar, TB_ENABLEBUTTON, ID_COPY, MAKELONG(cur[0] != 0, 0));
    SendMessage(toolbar, TB_ENABLEBUTTON, ID_PASTE, MAKELONG(cur[0] != 0, 0));
    SendMessage(toolbar, TB_ENABLEBUTTON, ID_DELETE, MAKELONG(cur[0] != 0, 0));
    fill_list(select_name);
    syncing = true;
    HTREEITEM it = find_node(cur);
    if (it)
        SendMessage(tree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)it);
    syncing = false;
    DebugLog("explorer at %s (%d items)", cur[0] ? cur : "My Computer", nents);
}

static void go_back(bool forward)
{
    char (*stack)[MAX_PATH] = forward ? fwd_ : back_;
    int *n = forward ? &nfwd : &nback;
    if (!*n) return;
    char to[MAX_PATH], from[NAME_MAX] = "";
    strncpy0(to, stack[--*n], sizeof to);
    if (cur[0] && !strncmp(cur, to, strlen(to)))    /* back up into a parent: the child selected */
        strncpy0(from, base_of(cur), sizeof from);
    go(to, from[0] ? from : NULL, forward ? 2 : 1);
    DebugLog("explorer %s to %s", forward ? "forward" : "back", cur[0] ? cur : "My Computer");
}

static void go_up(void)
{
    if (!cur[0])
        return;
    char parent[MAX_PATH], from[NAME_MAX];
    parent_of(cur, parent);
    if (!parent[0])
        drive_name(cur[0], from, sizeof from);
    else
        strncpy0(from, base_of(cur), sizeof from);
    navigate(parent, from);
}

static void open_entry(int e)
{
    if (e < 0 || e >= nents)
        return;
    struct entry *x = &ents[e];
    char path[MAX_PATH];
    if (x->letter) {
        char root[4] = { x->letter, ':', '\\', 0 };
        navigate(root, NULL);
        return;
    }
    join(path, cur, x->name);
    if (x->dir) {
        navigate(path, NULL);
        return;
    }
    int r;
    const struct ftype *ft = type_of(x->name);
    if (ext_is(x->name, "BIN"))
        r = ShellExecute(win, "open", path, NULL, cur, SW_SHOW);
    else if (ext_is(x->name, "COM EXE") || (ft && !ft->prog[0] && ext_is(x->name, "BAT"))) {
        MessageBox(win, "MS-DOS programs run in the MS-DOS Prompt.", "Explorer", MB_OK | MB_ICONINFORMATION);
        return;
    } else if (ft && !ft->prog[0]) {
        char text[MAX_PATH + 64];
        snprintf(text, sizeof text, "%s\nThere is no program for %s files (FILETYPES.INI).", path, ft->ext);
        MessageBox(win, text, "Explorer", MB_OK | MB_ICONINFORMATION);
        return;
    } else
        r = ShellExecute(win, "open", ft ? ft->prog : "NOTEPAD.BIN", path, cur, SW_SHOW);
    DebugLog("explorer opened %s", path);
    if (r <= 32) {
        char text[MAX_PATH + 32];
        snprintf(text, sizeof text, "Cannot open %s", path);
        MessageBox(win, text, "Explorer", MB_OK | MB_ICONERROR);
    }
}

static void open_selected(void)
{
    int i = (int)SendMessage(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED);
    if (i < 0 || !(SendMessage(list, LVM_GETITEMSTATE, (WPARAM)i, LVIS_SELECTED) & LVIS_SELECTED))
        i = (int)SendMessage(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    open_entry(entry_at(i));
}

static void refresh(void)
{
    int f = (int)SendMessage(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED);
    char keep[NAME_MAX] = "";
    int e = entry_at(f);
    if (e >= 0)
        strncpy0(keep, ents[e].name, sizeof keep);
    HTREEITEM it = find_node(cur);
    if (it)
        repopulate(it);
    fill_list(keep[0] ? keep : NULL);
}

/* ---- changing things ---------------------------------------------------------------------- */

static bool remove_tree(const char *path)
{
    char pat[MAX_PATH], sub[MAX_PATH];
    join(pat, path, "*.*");
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile(pat, &fd);
    bool ok = true;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.name, ".") || !strcmp(fd.name, ".."))
                continue;
            join(sub, path, fd.name);
            if (fd.attrib & FILE_ATTRIBUTE_DIRECTORY)
                ok = remove_tree(sub) && ok;
            else {
                if (fd.attrib & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributes(sub, fd.attrib & ~(uint32_t)FILE_ATTRIBUTE_READONLY);
                ok = DeleteFile(sub) && ok;
            }
        } while (FindNextFile(f, &fd));
        FindClose(f);
    }
    return RemoveDirectory(path) && ok;
}

static void delete_selected(void)
{
    if (!cur[0])
        return;
    int n = (int)SendMessage(list, LVM_GETSELECTEDCOUNT, 0, 0);
    if (!n)
        return;
    char text[NAME_MAX + 80];
    int first = entry_at((int)SendMessage(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED));
    if (n == 1)
        snprintf(text, sizeof text, "Are you sure you want to delete '%s'%s?", ents[first].name,
                 ents[first].dir ? " and all of its contents" : "");
    else
        snprintf(text, sizeof text, "Are you sure you want to delete these %d items?", n);
    if (MessageBox(win, text, n == 1 ? (ents[first].dir ? "Confirm Folder Delete" : "Confirm File Delete")
                                     : "Confirm Multiple File Delete", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    int done = 0, failed = 0;
    for (int i = -1; (i = (int)SendMessage(list, LVM_GETNEXTITEM, (WPARAM)i, LVNI_SELECTED)) >= 0;) {
        int e = entry_at(i);
        char path[MAX_PATH];
        join(path, cur, ents[e].name);
        bool ok = ents[e].dir ? remove_tree(path) : DeleteFile(path);
        if (ok) done++; else failed++;
    }
    DebugLog("explorer deleted %d", done);
    if (failed) {
        snprintf(text, sizeof text, "%d item(s) could not be deleted.", failed);
        MessageBox(win, text, "Explorer", MB_OK | MB_ICONWARNING);
    }
    refresh();
}

static void new_folder(void)
{
    if (!cur[0])
        return;
    char name[32] = "New Folder", path[MAX_PATH];
    join(path, cur, name);
    for (int k = 2; GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES && k < 100; k++) {
        snprintf(name, sizeof name, "New Folder (%d)", k);
        join(path, cur, name);
    }
    if (!CreateDirectory(path)) {
        MessageBox(win, "Unable to create the folder.", "Explorer", MB_OK | MB_ICONERROR);
        return;
    }
    DebugLog("explorer made %s", path);
    refresh();
    for (int i = 0; i < nents; i++) {
        int e = entry_at(i);
        if (e >= 0 && !strcmp(ents[e].name, name)) {
            LVITEM st = { .state = LVIS_SELECTED | LVIS_FOCUSED, .stateMask = LVIS_SELECTED | LVIS_FOCUSED };
            SendMessage(list, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&(LVITEM){ .state = 0, .stateMask = LVIS_SELECTED });
            SendMessage(list, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&st);
            SetFocus(list);
            SendMessage(list, LVM_EDITLABEL, (WPARAM)i, 0);
            break;
        }
    }
}

static void folder_size(const char *path, uint64_t *bytes, int *files, int *dirs)
{
    char pat[MAX_PATH], sub[MAX_PATH];
    join(pat, path, "*.*");
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!strcmp(fd.name, ".") || !strcmp(fd.name, ".."))
            continue;
        if (fd.attrib & FILE_ATTRIBUTE_DIRECTORY) {
            (*dirs)++;
            join(sub, path, fd.name);
            folder_size(sub, bytes, files, dirs);
        } else {
            (*files)++;
            *bytes += fd.size | (uint64_t)fd.size_high << 32;
        }
    } while (FindNextFile(f, &fd));
    FindClose(f);
}

static void properties(void)
{
    int e = entry_at((int)SendMessage(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED));
    char text[512], a[32], b[32], c[48];
    if (e < 0) {
        if (!cur[0])
            return;
        uint64_t bytes = 0;
        int files = 0, dirs = 0;
        folder_size(cur, &bytes, &files, &dirs);
        fmt_size(bytes, a, sizeof a);
        snprintf(text, sizeof text, "%s\n\nType: File Folder\nSize: %s\nContains: %d files, %d folders", cur, a,
                 files, dirs);
    } else if (ents[e].letter) {
        struct entry *x = &ents[e];
        fmt_size(x->total - x->free_b, a, sizeof a);
        fmt_size(x->free_b, b, sizeof b);
        fmt_size(x->total, c, sizeof c);
        snprintf(text, sizeof text, "%s\n\nType: %s\nUsed space: %s\nFree space: %s\nCapacity: %s", x->name,
                 x->type, a, b, c);
    } else {
        struct entry *x = &ents[e];
        char path[MAX_PATH];
        join(path, cur, x->name);
        uint64_t bytes = x->size;
        int files = 0, dirs = 0;
        if (x->dir)
            folder_size(path, &bytes, &files, &dirs);
        fmt_size(bytes, a, sizeof a);
        fmt_date(x->date, x->time, c, sizeof c);
        int n = snprintf(text, sizeof text, "%s\n\nType: %s\nLocation: %s\nSize: %s (%u bytes)\nModified: %s",
                         x->name, x->type, cur, a, (unsigned)bytes, c);
        if (x->dir && n > 0 && n < (int)sizeof text)
            snprintf(text + n, sizeof text - (size_t)n, "\nContains: %d files, %d folders", files, dirs);
        else if (n > 0 && n < (int)sizeof text)
            snprintf(text + n, sizeof text - (size_t)n, "\nAttributes:%s%s%s%s", x->attr & 1 ? " Read-only" : "",
                     x->attr & 2 ? " Hidden" : "", x->attr & 4 ? " System" : "", x->attr & 0x20 ? " Archive" : "");
    }
    MessageBox(win, text, "Properties", MB_OK | MB_ICONINFORMATION);
}

static LRESULT end_rename(NMLVDISPINFO *di)
{
    if (!di->item.pszText || !cur[0])
        return 0;
    int e = (int)di->item.lParam;
    if (e < 0 || e >= nents || !strcmp(di->item.pszText, ents[e].name))
        return 0;
    if (!good_name(di->item.pszText)) {
        MessageBox(win, "A file name can't contain any of these characters:\n\\ / : * ? \" < > |", "Rename",
                   MB_OK | MB_ICONERROR);
        return 0;
    }
    char from[MAX_PATH], to[MAX_PATH];
    join(from, cur, ents[e].name);
    join(to, cur, di->item.pszText);
    if (!MoveFile(from, to)) {
        char text[MAX_PATH + 48];
        snprintf(text, sizeof text, "Cannot rename %s: a file with that name may exist.", ents[e].name);
        MessageBox(win, text, "Error Renaming File", MB_OK | MB_ICONERROR);
        return 0;
    }
    DebugLog("explorer renamed %s to %s", ents[e].name, di->item.pszText);
    strncpy0(ents[e].name, di->item.pszText, sizeof ents[e].name);
    if (!ents[e].dir) {
        type_for(&ents[e]);
        LVITEM it = { .mask = LVIF_IMAGE, .iItem = di->item.iItem, .iImage = icon_for(&ents[e]) };
        SendMessage(list, LVM_SETITEM, 0, (LPARAM)&it);
        set_sub(di->item.iItem, 2, ents[e].type);
    } else {
        HTREEITEM it = find_node(cur);
        if (it)
            repopulate(it);
    }
    return 1;
}

static void set_view(int view)
{
    SendMessage(list, LVM_SETVIEW, (WPARAM)view, 0);
    int ids[4] = { ID_LARGE, ID_DETAILS, ID_SMALL, ID_LISTV };
    for (int k = 0; k < 4; k++)
        SendMessage(toolbar, TB_CHECKBUTTON, (WPARAM)ids[k], MAKELONG(k == (view & 3), 0));
    CheckMenuRadioItem(view_menu, ID_LARGE, ID_DETAILS,
                       view == LV_VIEW_ICON ? ID_LARGE : view == LV_VIEW_SMALLICON ? ID_SMALL
                       : view == LV_VIEW_LIST ? ID_LISTV : ID_DETAILS, MF_BYCOMMAND);
    static const char *const names[] = { "large icons", "details", "small icons", "list" };
    DebugLog("explorer view %s", names[view & 3]);
}

static void sort_by(int col)
{
    if (sort_col == col)
        sort_desc = !sort_desc;
    else
        sort_col = col, sort_desc = false;
    SendMessage(list, LVM_SORTITEMS, 0, (LPARAM)compare);
    for (int c = 0; c < 4; c++) {                       /* the arrow on the sorted column */
        LVCOLUMN lc = { .mask = LVCF_FMT };
        if (!SendMessage(list, LVM_GETCOLUMN, (WPARAM)c, (LPARAM)&lc)) break;
        lc.fmt = (lc.fmt & LVCFMT_JUSTIFYMASK) | (c == col ? (sort_desc ? LVCFMT_SORTDOWN : LVCFMT_SORTUP) : 0);
        SendMessage(list, LVM_SETCOLUMN, (WPARAM)c, (LPARAM)&lc);
    }
    static const char *const names[] = { "name", "size", "type", "date" };
    DebugLog("explorer sorted by %s%s", names[col & 3], sort_desc ? ", descending" : "");
}

/* ---- the window -------------------------------------------------------------------------- */

static void layout(void)
{
    RECT r;
    GetClientRect(win, &r);
    int w = r.right, h = r.bottom - BAR_H - STATUS_H;
    tree_w = tree_w < 60 ? 60 : tree_w > w - 120 ? (w - 120 > 60 ? w - 120 : 60) : tree_w;
    int tbw = 15 * 24 + 3 * 8 - 4 * 24 + 8;            /* the buttons up to the views */
    RECT last;
    if (SendMessage(toolbar, TB_GETITEMRECT, 14, (LPARAM)&last)) tbw = last.right + 4;
    MoveWindow(toolbar, 0, 0, tbw, BAR_H, true);
    MoveWindow(GetDlgItem(win, ID_ADDR + 100), tbw + 4, 7, 56, 16, true);
    int ax = tbw + 60;
    MoveWindow(addr, ax, 3, w - ax - 4 > 60 ? w - ax - 4 : 60, 24, true);
    MoveWindow(tree, 2, BAR_H, tree_w, h, true);
    MoveWindow(list, 2 + tree_w + SPLIT_W, BAR_H, w - tree_w - SPLIT_W - 4, h, true);
}

/* ---- copying and moving ------------------------------------------------------------------- */

/* The selected items' full paths, NUL after each, a NUL at the end; -> how many. */
static int selected_paths(char *out, int size)
{
    int k = 0, n = 0;
    for (int i = -1; (i = (int)SendMessage(list, LVM_GETNEXTITEM, (WPARAM)i, LVNI_SELECTED)) >= 0;) {
        int e = entry_at(i);
        if (e < 0 || ents[e].letter) continue;
        char path[MAX_PATH];
        join(path, cur, ents[e].name);
        int len = (int)strlen(path);
        if (k + len + 2 > size) break;
        memcpy(out + k, path, (size_t)len + 1);
        k += len + 1;
        n++;
    }
    out[k] = '\0';
    return n;
}

static bool copy_tree(const char *from, const char *to)        /* a folder and all it holds */
{
    if (!CreateDirectory(to) && GetFileAttributes(to) == INVALID_FILE_ATTRIBUTES)
        return false;
    char pat[MAX_PATH];
    join(pat, from, "*.*");
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile(pat, &fd);
    bool ok = true;
    if (f == INVALID_HANDLE_VALUE)
        return true;
    do {
        if (!strcmp(fd.name, ".") || !strcmp(fd.name, ".."))
            continue;
        char a[MAX_PATH], b[MAX_PATH];
        join(a, from, fd.name);
        join(b, to, fd.name);
        if (fd.attrib & FILE_ATTRIBUTE_DIRECTORY) ok = copy_tree(a, b) && ok;
        else ok = CopyFile(a, b, false) && ok;
    } while (FindNextFile(f, &fd));
    FindClose(f);
    return ok;
}

static bool same_drive(const char *a, const char *b) { return up_(a[0]) == up_(b[0]) && a[1] == ':' && b[1] == ':'; }

/* Copy or move the files to the folder `to`; -> how many went. */
static int transfer(const char *paths, const char *to, bool move)
{
    int done = 0, failed = 0;
    for (const char *p = paths; *p; p += strlen(p) + 1) {
        char parent[MAX_PATH], dst[MAX_PATH], name[NAME_MAX];
        parent_of(p, parent);
        strncpy0(name, base_of(p), sizeof name);
        size_t tl = strlen(to);
        if (!cmp_nocase(p, to) || (!strncmp(to, p, strlen(p)) && (to[strlen(p)] == '\\' || !to[strlen(p)]) &&
                                    tl > strlen(p))) {
            failed++;                                   /* not into itself */
            continue;
        }
        join(dst, to, name);
        if (!cmp_nocase(parent, to)) {                  /* the same folder */
            if (move) continue;
            char copy[NAME_MAX + 16];
            snprintf(copy, sizeof copy, "Copy of %s", name);
            join(dst, to, copy);
            for (int k = 2; GetFileAttributes(dst) != INVALID_FILE_ATTRIBUTES && k < 100; k++) {
                snprintf(copy, sizeof copy, "Copy (%d) of %s", k, name);
                join(dst, to, copy);
            }
        } else if (GetFileAttributes(dst) != INVALID_FILE_ATTRIBUTES) {
            char text[MAX_PATH + 80];
            snprintf(text, sizeof text, "%s already exists in %s.\nReplace it?", name, to);
            if (MessageBox(win, text, move ? "Move" : "Copy", MB_YESNO | MB_ICONWARNING) != IDYES)
                continue;
            uint32_t a = GetFileAttributes(dst);
            if (a & FILE_ATTRIBUTE_DIRECTORY) remove_tree(dst); else DeleteFile(dst);
        }
        uint32_t a = GetFileAttributes(p);
        bool dir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY), ok;
        if (move && same_drive(p, dst))
            ok = MoveFile(p, dst);
        else {
            ok = dir ? copy_tree(p, dst) : CopyFile(p, dst, false);
            if (ok && move)
                ok = dir ? remove_tree(p) : DeleteFile(p);
        }
        if (ok) done++; else failed++;
    }
    DebugLog("explorer %s %d to %s", move ? "moved" : "copied", done, to);
    if (failed) {
        char text[96];
        snprintf(text, sizeof text, "%d item(s) could not be %s.", failed, move ? "moved" : "copied");
        MessageBox(win, text, "Explorer", MB_OK | MB_ICONWARNING);
    }
    return done;
}

static void mark_cut(bool on)                           /* fade the cut items */
{
    LVITEM st = { .state = 0, .stateMask = LVIS_CUT };
    SendMessage(list, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&st);
    if (!on) return;
    st.state = LVIS_CUT;
    for (int i = -1; (i = (int)SendMessage(list, LVM_GETNEXTITEM, (WPARAM)i, LVNI_SELECTED)) >= 0;)
        SendMessage(list, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&st);
}

static void clip_files(bool cut)
{
    static char buf[4096];
    int n = cur[0] ? selected_paths(buf, sizeof buf) : 0;
    if (!n) return;
    SetClipboardFiles(buf, cut);
    mark_cut(cut);
    DebugLog("explorer %s %d", cut ? "cut" : "copied to the clipboard", n);
}

static void paste_files(void)
{
    static char buf[4096];
    bool cut;
    int n = GetClipboardFiles(buf, sizeof buf, &cut);
    if (!cur[0] || n <= 1)
        return;
    transfer(buf, cur, cut);
    if (cut) EmptyClipboard();
    refresh();
}

/* ---- dragging -------------------------------------------------------------------------- */

static void begin_drag(const char *paths)
{
    int n = 0;
    while (paths[n]) n += (int)strlen(paths + n) + 1;
    if (!n || n + 1 > (int)sizeof drag_from) return;
    memcpy(drag_from, paths, (size_t)n + 1);
    dragging = true;
    drop_path[0] = '\0';
    drop_item = -1;
    drop_node = NULL;
    SetCapture(win);
    DebugLog("explorer dragging %s", drag_from);
}

static void drop_mark(int item, HTREEITEM node)
{
    if (item != drop_item) {
        LVITEM st = { .state = 0, .stateMask = LVIS_DROPHILITED };
        if (drop_item >= 0) SendMessage(list, LVM_SETITEMSTATE, (WPARAM)drop_item, (LPARAM)&st);
        st.state = LVIS_DROPHILITED;
        if (item >= 0) SendMessage(list, LVM_SETITEMSTATE, (WPARAM)item, (LPARAM)&st);
        drop_item = item;
    }
    if (node != drop_node) {
        SendMessage(tree, TVM_SELECTITEM, TVGN_DROPHILITE, (LPARAM)node);
        drop_node = node;
    }
}

/* Where the pointer is (screen coordinates): a folder to drop into? */
static void drag_over(int sx, int sy)
{
    POINT p = { sx, sy };
    HWND under = WindowFromPoint(p);
    int item = -1;
    HTREEITEM node = NULL;
    drop_path[0] = '\0';
    if (under == list) {
        ScreenToClient(list, &p);
        LVHITTESTINFO ht = { p, 0, -1, 0 };
        int i = (int)SendMessage(list, LVM_HITTEST, 0, (LPARAM)&ht), e = entry_at(i);
        if (e >= 0 && (ents[e].dir || ents[e].letter)) {
            item = i;
            if (ents[e].letter) snprintf(drop_path, sizeof drop_path, "%c:\\", ents[e].letter);
            else join(drop_path, cur, ents[e].name);
        } else if (cur[0]) {
            strncpy0(drop_path, cur, sizeof drop_path);  /* this folder */
        }
    } else if (under == tree) {
        ScreenToClient(tree, &p);
        TVHITTESTINFO ht = { p, 0, NULL };
        HTREEITEM it = (HTREEITEM)SendMessage(tree, TVM_HITTEST, 0, (LPARAM)&ht);
        struct node *nd = it && (ht.flags & TVHT_ONITEM) ? node_of(it) : NULL;
        if (nd && nd->path[0]) {
            node = it;
            strncpy0(drop_path, nd->path, sizeof drop_path);
        }
    }
    char parent[MAX_PATH];                              /* where they are already: nowhere to go */
    parent_of(drag_from, parent);
    if (drop_path[0] && !cmp_nocase(parent, drop_path) && item < 0 && !node)
        drop_path[0] = '\0';
    drop_mark(item, node);
    SetCursor(LoadCursor(NULL, drop_path[0] ? IDC_ARROW : IDC_NO));
}

static void end_drag(bool drop)
{
    if (!dragging) return;
    dragging = false;
    ReleaseCapture();
    drop_mark(-1, NULL);
    if (!drop || !drop_path[0])
        return;
    bool copy = GetKeyState(VK_CONTROL) < 0 || !same_drive(drag_from, drop_path);
    if (GetKeyState(VK_SHIFT) < 0) copy = false;
    char to[MAX_PATH];
    strncpy0(to, drop_path, sizeof to);
    int n = transfer(drag_from, to, !copy);
    if (n) {
        HTREEITEM a = find_node(to);
        if (a) repopulate(a);
        refresh();
    }
}

static void command(int id)
{
    switch (id) {
    case ID_OPEN:       open_selected(); break;
    case ID_NEWFOLDER:  new_folder(); break;
    case ID_DELETE:     delete_selected(); break;
    case ID_RENAME: {
        int i = (int)SendMessage(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
        if (i >= 0 && cur[0]) {
            SetFocus(list);
            SendMessage(list, LVM_EDITLABEL, (WPARAM)i, 0);
        }
        break;
    }
    case ID_PROPS:      properties(); break;
    case ID_CLOSE:      SendMessage(win, WM_CLOSE, 0, 0); break;
    case ID_SELALL:
    case ID_INVERT:
        for (int i = 0; i < nents; i++) {
            unsigned now = (unsigned)SendMessage(list, LVM_GETITEMSTATE, (WPARAM)i, LVIS_SELECTED);
            LVITEM st = { .state = id == ID_SELALL || !now ? LVIS_SELECTED : 0, .stateMask = LVIS_SELECTED };
            SendMessage(list, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&st);
        }
        SetFocus(list);
        break;
    case ID_LARGE:      set_view(LV_VIEW_ICON); break;
    case ID_SMALL:      set_view(LV_VIEW_SMALLICON); break;
    case ID_LISTV:      set_view(LV_VIEW_LIST); break;
    case ID_DETAILS:    set_view(LV_VIEW_DETAILS); break;
    case ID_BYNAME:     sort_col = -9; sort_by(0); break;
    case ID_BYSIZE:     sort_col = -9; sort_by(1); break;
    case ID_BYTYPE:     sort_col = -9; sort_by(2); break;
    case ID_BYDATE:     sort_col = -9; sort_by(3); break;
    case ID_REFRESH:    refresh(); break;
    case ID_GOUP:
    case ID_UP:         go_up(); SetFocus(list); break;
    case ID_BACK:       go_back(false); SetFocus(list); break;
    case ID_FORWARD:    go_back(true); SetFocus(list); break;
    case ID_CUT:        clip_files(true); break;
    case ID_COPY:       clip_files(false); break;
    case ID_PASTE:      paste_files(); break;
    case ID_GOCOMPUTER: navigate("", NULL); break;
    case ID_FOCUSADDR:  SetFocus(addr); SendMessage(addr, EM_SETSEL, 0, -1); break;
    case ID_ABOUT:      ShellAbout(win, "Explorer", "Browses the drives OS1993 can reach: DOS's,\n"
                                   "and its own FAT, exFAT, ext2/3/4 and btrfs volumes."); break;
    }
}

static LRESULT notify(NMHDR *nm)
{
    if (nm->hwndFrom == tree) {
        NMTREEVIEW *tv = (NMTREEVIEW *)nm;
        switch (nm->code) {
        case TVN_ITEMEXPANDING:
            if (tv->action == TVE_EXPAND)
                populate(tv->itemNew.hItem);
            return 0;
        case TVN_SELCHANGED:
            if (!syncing && tv->itemNew.hItem) {
                struct node *n = (struct node *)tv->itemNew.lParam;
                if (n && strcmp(n->path, cur))
                    navigate(n->path, NULL);
            }
            return 0;
        case TVN_DELETEITEM:
            LocalFree((void *)tv->itemOld.lParam);
            return 0;
        case TVN_BEGINDRAG: {
            struct node *n = (struct node *)tv->itemNew.lParam;
            if (n && n->path[0] && strlen(n->path) > 3) {    /* a folder (not a drive) */
                char buf[MAX_PATH + 2];
                strncpy0(buf, n->path, MAX_PATH);
                buf[strlen(buf) + 1] = '\0';
                begin_drag(buf);
            }
            return 0;
        }
        case TVN_KEYDOWN: {
            unsigned vk = ((NMLVKEYDOWN *)nm)->wVKey;
            if (vk == VK_TAB)
                SetFocus(list);
            else if (vk == VK_BACK)
                go_up();
            return 0;
        }
        }
        return 0;
    }
    if (nm->hwndFrom == list) {
        switch (nm->code) {
        case LVN_ITEMACTIVATE:
            open_selected();
            return 0;
        case LVN_BEGINDRAG: {
            static char buf[4096];
            if (cur[0] && selected_paths(buf, sizeof buf))
                begin_drag(buf);
            return 1;                                   /* ours: no icons moving about */
        }
        case LVN_ITEMCHANGED:
            update_status();
            return 0;
        case LVN_COLUMNCLICK:
            sort_by(((NMLISTVIEW *)nm)->iSubItem);
            return 0;
        case LVN_BEGINLABELEDIT:
            return !cur[0];                             /* drives keep their names */
        case LVN_ENDLABELEDIT:
            return end_rename((NMLVDISPINFO *)nm);
        case LVN_KEYDOWN: {
            unsigned vk = ((NMLVKEYDOWN *)nm)->wVKey;
            if (vk == VK_TAB)
                SetFocus(tree);
            else if (vk == VK_BACK)
                go_up();
            else if (vk == VK_DELETE)
                delete_selected();
            return 0;
        }
        case NM_RCLICK: {
            HMENU m = CreatePopupMenu();
            int i = (int)SendMessage(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
            if (i >= 0) {
                AppendMenu(m, MF_STRING, ID_OPEN, "&Open");
                AppendMenu(m, MF_SEPARATOR, 0, NULL);
                AppendMenu(m, cur[0] ? MF_STRING : MF_GRAYED, ID_CUT, "Cu&t");
                AppendMenu(m, cur[0] ? MF_STRING : MF_GRAYED, ID_COPY, "&Copy");
                AppendMenu(m, MF_SEPARATOR, 0, NULL);
                AppendMenu(m, cur[0] ? MF_STRING : MF_GRAYED, ID_DELETE, "&Delete");
                AppendMenu(m, cur[0] ? MF_STRING : MF_GRAYED, ID_RENAME, "Rena&me");
            } else {
                AppendMenu(m, MF_STRING, ID_LARGE, "Lar&ge Icons");
                AppendMenu(m, MF_STRING, ID_SMALL, "S&mall Icons");
                AppendMenu(m, MF_STRING, ID_LISTV, "&List");
                AppendMenu(m, MF_STRING, ID_DETAILS, "&Details");
                AppendMenu(m, MF_SEPARATOR, 0, NULL);
                AppendMenu(m, cur[0] ? MF_STRING : MF_GRAYED, ID_NEWFOLDER, "New &Folder");
                AppendMenu(m, cur[0] && IsClipboardFormatAvailable(CF_FILES) ? MF_STRING : MF_GRAYED, ID_PASTE,
                           "&Paste");
                AppendMenu(m, MF_STRING, ID_REFRESH, "R&efresh");
            }
            AppendMenu(m, MF_SEPARATOR, 0, NULL);
            AppendMenu(m, MF_STRING, ID_PROPS, "P&roperties");
            NMITEMACTIVATE *ia = (NMITEMACTIVATE *)nm;
            RECT lr;
            GetWindowRect(list, &lr);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD, lr.left + ia->ptAction.x, lr.top + ia->ptAction.y, win);
            DestroyMenu(m);
            if (cmd)
                command(cmd);
            return 0;
        }
        }
    }
    return 0;
}

static LRESULT addr_sub(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        char t[MAX_PATH];
        GetWindowText(h, t, sizeof t);
        if (!t[0] || !cmp_nocase(t, "My Computer"))
            navigate("", NULL);
        else
            navigate(t, NULL);
        SetFocus(list);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        SetWindowText(h, cur[0] ? cur : "My Computer");
        SetFocus(list);
        return 0;
    }
    if (msg == WM_CHAR && wp == '\r')
        return 0;
    return CallWindowProc(addr_old, h, msg, wp, lp);
}

static LRESULT win_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        toolbar = CreateWindow(TOOLBARCLASSNAME, "", WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
                               CCS_NOPARENTALIGN, 0, 0, 10, 10, h, ID_TOOLBAR, NULL);
        upbtn = toolbar;
        CreateWindow(WC_STATIC, "Address", WS_CHILD | WS_VISIBLE, 0, 7, 56, 16, h, ID_ADDR + 100, NULL);
        addr = CreateWindow(WC_EDIT, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, h, ID_ADDR, NULL);
        tree = CreateWindow(WC_TREEVIEW, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
                            TVS_LINESATROOT | TVS_SHOWSELALWAYS, 0, 0, 10, 10, h, ID_TREE, NULL);
        list = CreateWindow(WC_LISTVIEW, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS |
                            LVS_EDITLABELS, 0, 0, 10, 10, h, ID_LIST, NULL);
        if (!upbtn || !addr || !tree || !list)
            return -1;
        addr_old = (WNDPROC)(uintptr_t)SetWindowLong(addr, GWL_WNDPROC, (int32_t)(uintptr_t)addr_sub);
        return 0;
    case WM_SIZE:
        layout();
        InvalidateRect(h, NULL);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE)
            SetFocus(list);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        RECT sb = { 2, r.bottom - STATUS_H + 2, r.right - 2, r.bottom - 2 };
        int split = r.right * 3 / 5 > 160 ? r.right * 3 / 5 : 160;
        RECT left = { sb.left, sb.top, split - 2, sb.bottom }, right = { split, sb.top, sb.right, sb.bottom };
        DrawBevel(dc, &left, false);
        DrawBevel(dc, &right, false);
        TextOut(dc, left.left + 4, left.top + 2, status_left, C_BLACK, -1);
        TextOut(dc, right.left + 4, right.top + 2, status_right, C_BLACK, -1);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (dragging) return 0;
        if (x >= 2 + tree_w && x < 2 + tree_w + SPLIT_W && y >= BAR_H) {
            splitting = true;
            SetCapture(h);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (dragging) {
            POINT p = { x, y };
            ClientToScreen(h, &p);
            drag_over(p.x, p.y);
            return 0;
        }
        bool on = splitting || (x >= 2 + tree_w && x < 2 + tree_w + SPLIT_W && y >= BAR_H);
        SetCursor(LoadCursor(NULL, on ? IDC_SIZEWE : IDC_ARROW));
        if (splitting) {
            tree_w = x - 2 - SPLIT_W / 2;
            layout();
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (dragging) {
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(h, &p);
            drag_over(p.x, p.y);
            end_drag(true);
            return 0;
        }
        if (splitting) {
            splitting = false;
            ReleaseCapture();
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (dragging) end_drag(false);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_ADDR)
            return 0;
        command(LOWORD(wp));
        return 0;
    case WM_NOTIFY:
        return notify((NMHDR *)lp);
    case WM_INITMENUPOPUP:
        if ((HMENU)wp == edit_menu) {
            unsigned sel = cur[0] && SendMessage(list, LVM_GETSELECTEDCOUNT, 0, 0) ? MF_ENABLED : MF_GRAYED;
            EnableMenuItem(edit_menu, ID_CUT, sel);
            EnableMenuItem(edit_menu, ID_COPY, sel);
            EnableMenuItem(edit_menu, ID_PASTE, cur[0] && IsClipboardFormatAvailable(CF_FILES) ? MF_ENABLED : MF_GRAYED);
        }
        if ((HMENU)wp == file_menu) {
            unsigned in_folder = cur[0] ? MF_ENABLED : MF_GRAYED;
            unsigned sel = SendMessage(list, LVM_GETSELECTEDCOUNT, 0, 0) ? MF_ENABLED : MF_GRAYED;
            EnableMenuItem(file_menu, ID_OPEN, sel);
            EnableMenuItem(file_menu, ID_NEWFOLDER, in_folder);
            EnableMenuItem(file_menu, ID_DELETE, cur[0] ? sel : MF_GRAYED);
            EnableMenuItem(file_menu, ID_RENAME, cur[0] ? sel : MF_GRAYED);
        }
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* The toolbar's pictures ('.' clear, k black, w white, 1-5 greys, f face,
 * a accent, A pale accent, d dark accent, r red, g green, y yellow). */
static const char *const tb_art[][16] = {
    {   "................", "................", "......k.........", ".....kk.........", "....kak.........",
        "...kaakkkkkkk...", "..kaaaaaaaaaak..", ".kaaaaaaaaaaak..", "..kaaaaaaaaaak..", "...kaakkkkkkk...",
        "....kak.........", ".....kk.........", "......k.........", "................", "................",
        "................" },                                       /* back */
    {   "................", "................", ".........k......", ".........kk.....", ".........kak....",
        "...kkkkkkkaak...", "..kaaaaaaaaaak..", "..kaaaaaaaaaaak.", "..kaaaaaaaaaak..", "...kkkkkkkaak...",
        ".........kak....", ".........kk.....", ".........k......", "................", "................",
        "................" },                                       /* forward */
    {   "................", "........k.......", ".......kgk......", "......kgggk.....", ".....kgggggk....",
        "....kkkgggkkk...", ".kkkk.kgggk.....", "kyyyykkgggkkkk..", "kyyyyyyyyyyyyyk.", "kyyyyyyyyyyyyyk.",
        "kyyyyyyyyyyyyyk.", "kyyyyyyyyyyyyyk.", "kyyyyyyyyyyyyyk.", "kkkkkkkkkkkkkkk.", "................",
        "................" },                                       /* up */
    {   "................", "...k.....k......", "...k.....k......", "...kk...kk......", "....k...k.......",
        "....kk.kk.......", ".....kkk........", "......k.........", ".....kkk........", "...rrr.rrr......",
        "..r..r.r..r.....", "..r..r.r..r.....", "..r..r.r..r.....", "...rr...rr......", "................",
        "................" },                                       /* cut */
    {   "kkkkkkk.........", "kwwwwwkk........", "kwkkkwkwk.......", "kwwwwwkkkkkkk...", "kwkkkkkwwwwwkk..",
        "kwwwwwkwkkkwkwk.", "kwkkkkkwwwwwkkkk", "kwwwwwkwkkkkkwwk", "kwkkkkkwwwwwwwwk", "kwwwwwkwkkkkkkwk",
        "kkkkkkkwwwwwwwwk", ".......wkkkkkkwk", ".......kwwwwwwwk", ".......kkkkkkkkk", "................",
        "................" },                                       /* copy */
    {   "......kkkk......", "..kkkk2222kkkk..", ".k3333kkkk3333k.", ".k33kkkkkkkk33k.", ".k33kwwwwwwk33k.",
        ".k33kwkkkkwk33k.", ".k33kwwwwwwk33k.", ".k33kwkkkkwk33k.", ".k33kwwwwwwk33k.", ".k33kwkkkwwk33k.",
        ".k33kwwwwwwk33k.", ".k33kkkkkkkk33k.", ".k333333333333k.", "..kkkkkkkkkkkk..", "................",
        "................" },                                       /* paste */
    {   "................", "................", "..rr.......rr...", "..rrr.....rrr...", "...rrr...rrr....",
        "....rrr.rrr.....", ".....rrrrr......", "......rrr.......", ".....rrrrr......", "....rrr.rrr.....",
        "...rrr...rrr....", "..rrr.....rrr...", "..rr.......rr...", "................", "................",
        "................" },                                       /* delete */
    {   "..kkkkkkkkk.....", "..kwwwwwwwkk....", "..kwwwwwwwkwk...", "..kwkkkkwwkkkk..", "..kwwwwwwwwwwk..",
        "..kwkkkkkkkkwk..", "..kwwwwwwwwwwk..", "..kwkkkkkkkkwk..", "..kwwwwwwkkkkk..", "..kwkkkkka.....",
        "..kwwwwwwaa.a...", "..kwkkkkkaaaaa..", "..kwwwwwwaaaaa..", "..kkkkkkkkaaa...", "................",
        "................" },                                       /* properties */
    {   "................", ".kkkkk...kkkkk..", ".kaaak...kaaak..", ".kaaak...kaaak..", ".kaaak...kaaak..",
        ".kkkkk...kkkkk..", "................", "................", "................", ".kkkkk...kkkkk..",
        ".kaaak...kaaak..", ".kaaak...kaaak..", ".kaaak...kaaak..", ".kkkkk...kkkkk..", "................",
        "................" },                                       /* large icons */
    {   "................", "................", ".kk.kkk..kk.kkk.", ".kk......kk.....", "................",
        ".kk.kkk..kk.kkk.", ".kk......kk.....", "................", ".kk.kkk..kk.kkk.", ".kk......kk.....",
        "................", ".kk.kkk..kk.kkk.", ".kk......kk.....", "................", "................",
        "................" },                                       /* small icons */
    {   "................", "................", ".kk.kkkkk.......", "................", ".kk.kkkkk.......",
        "................", ".kk.kkkkk.......", "................", ".kk.kkkkk.......", "................",
        ".kk.kkkkk.......", "................", ".kk.kkkkk.......", "................", "................",
        "................" },                                       /* list */
    {   "................", ".kkkkkkkkkkkkkk.", "................", ".kk.kkkk.kk.kk..", "................",
        ".kk.kkkk.kk.kk..", "................", ".kk.kkkk.kk.kk..", "................", ".kk.kkkk.kk.kk..",
        "................", ".kk.kkkk.kk.kk..", "................", "................", "................",
        "................" },                                       /* details */
};

static uint8_t art_colour(char c)
{
    switch (c) {
    case 'k': return C_BLACK;  case 'w': return C_WHITE;  case '1': return C_GRAY1;  case '2': return C_GRAY2;
    case '3': return C_GRAY3;  case '4': return C_GRAY4;  case '5': return C_GRAY5;  case 'f': return C_FACE;
    case 'a': return C_ACCENT; case 'A': return C_ACCENT_LT; case 'd': return C_ACCENT_DK;
    case 'r': return C_RED;    case 'g': return C_GREEN;  case 'y': return C_YELLOW;
    }
    return 0xFF;
}

static void make_toolbar(void)
{
    HIMAGELIST il = ImageList_Create(16, 16, 0, 12, 4);
    for (unsigned i = 0; i < sizeof tb_art / sizeof tb_art[0]; i++) {
        uint8_t px[256];
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 16; c++)
                px[r * 16 + c] = art_colour(tb_art[i][r][c]);
        ImageList_AddPixels(il, px);
    }
    SendMessage(toolbar, TB_SETIMAGELIST, 0, (LPARAM)il);
    static const struct { int img, id; uint8_t style; const char *tip; } btn[] = {
        { 0, ID_BACK, TBSTYLE_BUTTON, "Back" }, { 1, ID_FORWARD, TBSTYLE_BUTTON, "Forward" },
        { 2, ID_UP, TBSTYLE_BUTTON, "Up One Level" }, { -1, 0, TBSTYLE_SEP, NULL },
        { 3, ID_CUT, TBSTYLE_BUTTON, "Cut" }, { 4, ID_COPY, TBSTYLE_BUTTON, "Copy" },
        { 5, ID_PASTE, TBSTYLE_BUTTON, "Paste" }, { -1, 0, TBSTYLE_SEP, NULL },
        { 6, ID_DELETE, TBSTYLE_BUTTON, "Delete" }, { 7, ID_PROPS, TBSTYLE_BUTTON, "Properties" },
        { -1, 0, TBSTYLE_SEP, NULL },
        { 8, ID_LARGE, TBSTYLE_CHECKGROUP, "Large Icons" }, { 9, ID_SMALL, TBSTYLE_CHECKGROUP, "Small Icons" },
        { 10, ID_LISTV, TBSTYLE_CHECKGROUP, "List" }, { 11, ID_DETAILS, TBSTYLE_CHECKGROUP, "Details" },
    };
    TBBUTTON b[sizeof btn / sizeof btn[0]];
    for (unsigned i = 0; i < sizeof btn / sizeof btn[0]; i++)
        b[i] = (TBBUTTON){ btn[i].img, btn[i].id, TBSTATE_ENABLED, btn[i].style, 0, (intptr_t)btn[i].tip };
    SendMessage(toolbar, TB_ADDBUTTONS, sizeof btn / sizeof btn[0], (LPARAM)b);
}

static void make_images(void)
{
    static const int icons[] = { ICON_COMPUTER, ICON_DRIVE, ICON_FLOPPY, ICON_FOLDER, ICON_FOLDER_OPEN, ICON_DOCUMENT,
                                 ICON_TEXT, ICON_APP, ICON_DOS, ICON_CALC, ICON_PAINT, ICON_CLOCK, ICON_NOTEPAD,
                                 ICON_DEMO, ICON_EXPLORER };
    il_small = ImageList_Create(16, 16, 0, 16, 4);
    il_large = ImageList_Create(32, 32, 0, 16, 4);
    for (unsigned i = 0; i < sizeof icons / sizeof icons[0]; i++) {
        ImageList_AddIcon(il_small, icons[i]);
        ImageList_AddIcon(il_large, icons[i]);
    }
}

int main(void)
{
    InitUI();
    WNDCLASS wc = { .name = "ExplorerWindow", .proc = win_proc, .background = C_FACE, .icon = ICON_EXPLORER,
                    .style = CS_DBLCLKS };
    RegisterClass(&wc);
    file_menu = CreatePopupMenu();
    HMENU edit = edit_menu = CreatePopupMenu(), arrange = CreatePopupMenu(), go = CreatePopupMenu(),
          help = CreatePopupMenu();
    HMENU bar = CreateMenu();
    view_menu = CreatePopupMenu();
    AppendMenu(file_menu, MF_STRING, ID_OPEN, "&Open\tEnter");
    AppendMenu(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(file_menu, MF_STRING, ID_NEWFOLDER, "New &Folder");
    AppendMenu(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(file_menu, MF_STRING, ID_DELETE, "&Delete\tDel");
    AppendMenu(file_menu, MF_STRING, ID_RENAME, "Rena&me\tF2");
    AppendMenu(file_menu, MF_STRING, ID_PROPS, "P&roperties\tAlt+Enter");
    AppendMenu(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(file_menu, MF_STRING, ID_CLOSE, "&Close");
    AppendMenu(edit, MF_STRING, ID_CUT, "Cu&t\tCtrl+X");
    AppendMenu(edit, MF_STRING, ID_COPY, "&Copy\tCtrl+C");
    AppendMenu(edit, MF_STRING, ID_PASTE, "&Paste\tCtrl+V");
    AppendMenu(edit, MF_SEPARATOR, 0, NULL);
    AppendMenu(edit, MF_STRING, ID_SELALL, "Select &All\tCtrl+A");
    AppendMenu(edit, MF_STRING, ID_INVERT, "&Invert Selection");
    AppendMenu(arrange, MF_STRING, ID_BYNAME, "by &Name");
    AppendMenu(arrange, MF_STRING, ID_BYTYPE, "by &Type");
    AppendMenu(arrange, MF_STRING, ID_BYSIZE, "by &Size");
    AppendMenu(arrange, MF_STRING, ID_BYDATE, "by &Date");
    AppendMenu(view_menu, MF_STRING, ID_LARGE, "Lar&ge Icons");
    AppendMenu(view_menu, MF_STRING, ID_SMALL, "S&mall Icons");
    AppendMenu(view_menu, MF_STRING, ID_LISTV, "&List");
    AppendMenu(view_menu, MF_STRING, ID_DETAILS, "&Details");
    AppendMenu(view_menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(view_menu, MF_POPUP, (uintptr_t)arrange, "Arrange &Icons");
    AppendMenu(view_menu, MF_STRING, ID_REFRESH, "&Refresh\tF5");
    AppendMenu(go, MF_STRING, ID_BACK, "&Back\tAlt+Left");
    AppendMenu(go, MF_STRING, ID_FORWARD, "&Forward\tAlt+Right");
    AppendMenu(go, MF_STRING, ID_GOUP, "&Up One Level\tBackspace");
    AppendMenu(go, MF_STRING, ID_GOCOMPUTER, "&My Computer");
    AppendMenu(go, MF_STRING, ID_FOCUSADDR, "&Address\tAlt+D");
    AppendMenu(help, MF_STRING, ID_ABOUT, "&About Explorer");
    AppendMenu(bar, MF_POPUP, (uintptr_t)file_menu, "&File");
    AppendMenu(bar, MF_POPUP, (uintptr_t)edit, "&Edit");
    AppendMenu(bar, MF_POPUP, (uintptr_t)view_menu, "&View");
    AppendMenu(bar, MF_POPUP, (uintptr_t)go, "&Go");
    AppendMenu(bar, MF_POPUP, (uintptr_t)help, "&Help");
    make_images();
    load_types();
    RECT work;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
    int ww = work.right - work.left - 40, wh = work.bottom - work.top - 30;
    win = CreateWindow("ExplorerWindow", "Exploring", WS_OVERLAPPEDWINDOW, work.left + 20, work.top + 12,
                       ww > 760 ? 760 : ww, wh > 540 ? 540 : wh, NULL, 0, NULL);
    if (!win)
        return 1;
    SetMenu(win, bar);
    SendMessage(tree, TVM_SETIMAGELIST, 0, (LPARAM)il_small);
    SendMessage(list, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)il_small);
    SendMessage(list, LVM_SETIMAGELIST, LVSIL_NORMAL, (LPARAM)il_large);
    make_toolbar();
    SendMessage(toolbar, TB_CHECKBUTTON, ID_DETAILS, MAKELONG(1, 0));
    CheckMenuRadioItem(view_menu, ID_LARGE, ID_DETAILS, ID_DETAILS, MF_BYCOMMAND);
    layout();
    fill_tree();
    const char *arg = GetStartupInfo()->args;
    navigate(arg[0] ? arg : "", NULL);
    started = true;
    ShowWindow(win, SW_SHOW);
    SetFocus(list);
    static const ACCEL keys[] = {
        { FVIRTKEY | FCONTROL, 'A', ID_SELALL }, { FVIRTKEY, VK_F1 + 4, ID_REFRESH },
        { FVIRTKEY | FALT, VK_RETURN, ID_PROPS }, { FVIRTKEY | FALT, 'D', ID_FOCUSADDR },
        { FVIRTKEY | FCONTROL, 'X', ID_CUT }, { FVIRTKEY | FCONTROL, 'C', ID_COPY },
        { FVIRTKEY | FCONTROL, 'V', ID_PASTE }, { FVIRTKEY | FALT, VK_LEFT, ID_BACK },
        { FVIRTKEY | FALT, VK_RIGHT, ID_FORWARD },
    };
    HACCEL acc = CreateAcceleratorTable(keys, sizeof keys / sizeof keys[0]);
    DebugLog("explorer ready");

    MSG msg;
    while (GetMessage(&msg)) {
        if (dragging && msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            end_drag(false);
            continue;
        }
        HWND f = GetFocus();
        bool typing = f == addr || (f && GetParent(f) == list);     /* (keys are text there) */
        if (!typing && TranslateAccelerator(win, acc, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
