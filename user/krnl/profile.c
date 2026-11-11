/*
 * profile.c -- INI files, as Win32's GetPrivateProfileString and friends.
 *
 * "[section]" lines start sections; "key=value" lines are entries; names
 * match without regard to case. A file is read whole for every call and
 * written whole for every change (they are small), keeping the other
 * lines -- comments too -- as they were. A relative name is in the
 * current folder (Windows would look in the Windows folder).
 */
#include "kpriv.h"

#define PROFILE_MAX 16384

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static bool same(const char *a, int n, const char *b)  /* a[0..n) is b, any case */
{
    int i = 0;
    for (; i < n && b[i]; i++)
        if (lower(a[i]) != lower(b[i])) return false;
    return i == n && !b[i];
}

static char *load(const char *file, int *len)
{
    char *t = LocalAlloc(0, PROFILE_MAX + 1);
    if (!t) return NULL;
    *len = 0;
    HANDLE f = CreateFile(file, GENERIC_READ, OPEN_EXISTING);
    if (f != INVALID_HANDLE_VALUE) {
        uint32_t got = 0;
        ReadFile(f, t, PROFILE_MAX, &got);
        CloseHandle(f);
        *len = (int)got;
    }
    t[*len] = '\0';
    return t;
}

/* One line: [s, e) without its line end; next: where the following starts. */
static int line_end(const char *t, int len, int s, int *next)
{
    int e = s;
    while (e < len && t[e] != '\n') e++;
    *next = e < len ? e + 1 : len;
    if (e > s && t[e - 1] == '\r') e--;
    return e;
}

static int trim_l(const char *t, int s, int e) { while (s < e && (t[s] == ' ' || t[s] == '\t')) s++; return s; }
static int trim_r(const char *t, int s, int e) { while (e > s && (t[e - 1] == ' ' || t[e - 1] == '\t')) e--; return e; }

/* Is the line [s, e) "[section]"? -> its name's bounds. */
static bool is_section(const char *t, int s, int e, int *ns, int *ne)
{
    s = trim_l(t, s, e);
    e = trim_r(t, s, e);
    if (s >= e || t[s] != '[' || t[e - 1] != ']')
        return false;
    *ns = trim_l(t, s + 1, e - 1);
    *ne = trim_r(t, *ns, e - 1);
    return true;
}

/* Walk the entries of a section: calls each(key, klen, val, vlen, line start, next line). */
struct entry { int ks, ke, vs, ve, line, next; };

static int entries(const char *t, int len, const char *section, struct entry *out, int max,
                   int *sec_line, int *sec_end)
{
    int pos = 0, n = 0;
    bool in = false;
    *sec_line = *sec_end = -1;
    while (pos < len) {
        int next, e = line_end(t, len, pos, &next), ns, ne;
        if (is_section(t, pos, e, &ns, &ne)) {
            if (in) { *sec_end = pos; return n; }
            in = same(t + ns, ne - ns, section);
            if (in) *sec_line = pos;
        } else if (in) {
            int s = trim_l(t, pos, e), eq = s;
            while (eq < e && t[eq] != '=') eq++;
            if (s < e && t[s] != ';' && eq < e && n < max) {
                int ke = trim_r(t, s, eq), vs = trim_l(t, eq + 1, e), ve = trim_r(t, vs, e);
                if (ve - vs >= 2 && t[vs] == '"' && t[ve - 1] == '"') { vs++; ve--; }
                out[n++] = (struct entry){ s, ke, vs, ve, pos, next };
            }
        }
        pos = next;
    }
    if (in) *sec_end = len;
    return n;
}

int GetPrivateProfileString(const char *section, const char *key, const char *def, char *out, int size,
                            const char *file)
{
    if (!out || size <= 0)
        return 0;
    out[0] = '\0';
    int len, sl, se, k = 0;
    char *t = section && file ? load(file, &len) : NULL;
    struct entry e[128];
    int n = t ? entries(t, len, section, e, 128, &sl, &se) : 0;
    if (!key) {                                     /* the keys, each NUL-ended, then a NUL */
        for (int i = 0; i < n; i++) {
            int kl = e[i].ke - e[i].ks;
            if (k + kl + 2 > size) break;
            memcpy(out + k, t + e[i].ks, (size_t)kl);
            k += kl;
            out[k++] = '\0';
        }
        out[k] = '\0';
        LocalFree(t);
        return k;
    }
    for (int i = 0; i < n; i++)
        if (same(t + e[i].ks, e[i].ke - e[i].ks, key)) {
            int vl = e[i].ve - e[i].vs;
            if (vl > size - 1) vl = size - 1;
            memcpy(out, t + e[i].vs, (size_t)vl);
            out[vl] = '\0';
            LocalFree(t);
            return vl;
        }
    LocalFree(t);
    strncpy0(out, def ? def : "", (size_t)size);
    return (int)strlen(out);
}

int GetPrivateProfileInt(const char *section, const char *key, int def, const char *file)
{
    char v[24];
    if (!GetPrivateProfileString(section, key, "", v, sizeof v, file))
        return def;
    const char *p = v;
    bool neg = *p == '-';
    if (neg || *p == '+') p++;
    if (*p < '0' || *p > '9')
        return def;
    int n = 0;
    while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    return neg ? -n : n;
}

/* "key=value\0key=value\0\0" for a whole section. */
int GetPrivateProfileSection(const char *section, char *out, int size, const char *file)
{
    if (!out || size < 2)
        return 0;
    int len, sl, se, k = 0;
    char *t = load(file, &len);
    struct entry e[128];
    int n = t ? entries(t, len, section, e, 128, &sl, &se) : 0;
    for (int i = 0; i < n; i++) {
        int kl = e[i].ke - e[i].ks, vl = e[i].ve - e[i].vs;
        if (k + kl + vl + 3 > size) break;
        memcpy(out + k, t + e[i].ks, (size_t)kl);
        k += kl;
        out[k++] = '=';
        memcpy(out + k, t + e[i].vs, (size_t)vl);
        k += vl;
        out[k++] = '\0';
    }
    out[k] = '\0';
    LocalFree(t);
    return k;
}

/* Set key in section to value (NULL: take the key out; key NULL: the
 * whole section). */
bool WritePrivateProfileString(const char *section, const char *key, const char *value, const char *file)
{
    if (!section || !file)
        return false;
    int len, sl, se;
    char *t = load(file, &len);
    if (!t) return false;
    struct entry e[128];
    int n = entries(t, len, section, e, 128, &sl, &se);
    char *o = LocalAlloc(0, PROFILE_MAX + 512);
    if (!o) { LocalFree(t); return false; }
    int k = 0;
    #define PUT(p, c) do { if (k + (c) < PROFILE_MAX + 500) { memcpy(o + k, (p), (size_t)(c)); k += (c); } } while (0)
    #define PUTS(s) PUT((s), (int)strlen(s))
    if (!key) {                                     /* the section goes */
        if (sl < 0) { LocalFree(t); LocalFree(o); return true; }
        PUT(t, sl);
        PUT(t + se, len - se);
    } else {
        int found = -1;
        for (int i = 0; i < n; i++)
            if (same(t + e[i].ks, e[i].ke - e[i].ks, key)) { found = i; break; }
        if (found >= 0) {                           /* replace or drop that line */
            PUT(t, e[found].line);
            if (value) { PUTS(key); PUTS("="); PUTS(value); PUTS("\r\n"); }
            PUT(t + e[found].next, len - e[found].next);
        } else if (!value) {
            LocalFree(t);
            LocalFree(o);
            return true;
        } else if (sl >= 0) {                       /* at the end of its section */
            int at = n ? e[n - 1].next : se;
            if (!n) {                               /* just after the "[section]" line */
                int next;
                line_end(t, len, sl, &next);
                at = next;
            }
            PUT(t, at);
            if (at > 0 && t[at - 1] != '\n') PUTS("\r\n");
            PUTS(key); PUTS("="); PUTS(value); PUTS("\r\n");
            PUT(t + at, len - at);
        } else {                                    /* a new section at the end */
            PUT(t, len);
            if (len && t[len - 1] != '\n') PUTS("\r\n");
            PUTS("["); PUTS(section); PUTS("]\r\n");
            PUTS(key); PUTS("="); PUTS(value); PUTS("\r\n");
        }
    }
    #undef PUT
    #undef PUTS
    LocalFree(t);
    HANDLE f = CreateFile(file, GENERIC_WRITE, CREATE_ALWAYS);
    bool ok = f != INVALID_HANDLE_VALUE;
    if (ok) {
        uint32_t done = 0;
        ok = WriteFile(f, o, (uint32_t)k, &done) && done == (uint32_t)k;
        CloseHandle(f);
    }
    LocalFree(o);
    return ok;
}
