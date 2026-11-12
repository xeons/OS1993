/*
 * vmset.c -- what each DOS session may have: memory, devices, the mouse,
 * idle detection. Like a PIF file's settings in Windows; here they come
 * from OS1993.INI (next to VMM.BIN), read again for every new session:
 *
 *   [DOS]
 *   Memory=640          conventional memory, KB (0 or absent: all there is)
 *   XMS=2048            XMS for the session's programs, KB (0: no XMS driver)
 *   DPMI=4096           memory for DPMI clients, KB (0: no DPMI host)
 *   Devices=COM2 COM3 COM4 LPT1 LPT2 LPT3   ports it may own ("all", "none")
 *   Idle=16             empty keyboard polls per tick before it sleeps (0: never)
 *   Mouse=1             the INT 33h driver
 *
 * The GUI can pass settings of its own for one session (SYS_NEW_SESSION).
 */
#include "kernel.h"
#include "v86.h"
#include "vm.h"

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static bool word_is(const char *s, int n, const char *w)
{
    int i = 0;
    for (; i < n && w[i]; i++)
        if (lower(s[i]) != lower(w[i])) return false;
    return i == n && !w[i];
}

static int number(const char *s, int n, int def)
{
    int v = 0, i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i == n || s[i] < '0' || s[i] > '9')
        return def;
    while (i < n && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
    return v;
}

static uint8_t devices(const char *s, int n)
{
    static const char *const names[] = { "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3" };
    uint8_t d = 0;
    for (int i = 0; i < n;) {
        while (i < n && (s[i] == ' ' || s[i] == ',' || s[i] == '\t')) i++;
        int j = i;
        while (j < n && s[j] != ' ' && s[j] != ',' && s[j] != '\t') j++;
        if (word_is(s + i, j - i, "all")) d = VMDEV_ALL;
        for (unsigned k = 0; k < ARRAY_SIZE(names); k++)
            if (word_is(s + i, j - i, names[k])) d |= (uint8_t)(1u << k);
        i = j;
    }
    return d;
}

static void apply(struct vm_settings *set, const char *key, int kn, const char *val, int vn)
{
    if (word_is(key, kn, "memory"))       set->mem_kb = (uint16_t)number(val, vn, set->mem_kb);
    else if (word_is(key, kn, "xms"))     set->xms_kb = (uint16_t)number(val, vn, set->xms_kb);
    else if (word_is(key, kn, "dpmi"))    set->dpmi_kb = (uint16_t)number(val, vn, set->dpmi_kb);
    else if (word_is(key, kn, "devices")) set->devices = devices(val, vn);
    else if (word_is(key, kn, "idle"))    set->idle = (uint8_t)number(val, vn, set->idle);
    else if (word_is(key, kn, "mouse"))   set->mouse = number(val, vn, set->mouse) != 0;
}

/* OS1993.INI, through DOS, into the transfer buffer. */
static int read_ini(const char **text)
{
    const char *kpath = v86_ptr(v86_buf_seg(), g_bootinfo->kpath_off);
    char path[128];
    size_t dir = 0, n = strlen(kpath);
    for (size_t i = 0; i < n; i++)
        if (kpath[i] == '\\' || kpath[i] == ':') dir = i + 1;
    if (dir + 11 > sizeof path) return -1;
    memcpy(path, kpath, dir);
    memcpy(path + dir, "OS1993.INI", 11);

    uint16_t seg = v86_buf_seg(), buf = v86_buf_off();
    memcpy(v86_ptr(seg, buf), path, strlen(path) + 1);
    struct v86_regs r = { .eax = 0x3D00, .edx = buf, .ds = seg };
    if (v86_int(0x21, &r) != 0 || (r.flags & EFLAGS_CF))
        return -1;
    uint32_t h = r.eax & 0xFFFF;
    uint16_t max = v86_buf_size() > 8192 ? 8192 : (uint16_t)(v86_buf_size() - 1);
    r = (struct v86_regs){ .eax = 0x3F00, .ebx = h, .ecx = max, .edx = buf, .ds = seg };
    int got = (v86_int(0x21, &r) == 0 && !(r.flags & EFLAGS_CF)) ? (int)(r.eax & 0xFFFF) : -1;
    r = (struct v86_regs){ .eax = 0x3E00, .ebx = h };
    v86_int(0x21, &r);
    *text = v86_ptr(seg, buf);
    return got;
}

/* Each key=value of section in the text: fn(key, klen, value, vlen). */
static void each(const char *t, int len, const char *section,
                 void (*fn)(void *ctx, const char *k, int kn, const char *v, int vn), void *ctx)
{
    bool in = false;
    for (int pos = 0; pos < len;) {
        int e = pos;
        while (e < len && t[e] != '\n' && t[e] != '\r') e++;
        int s = pos;
        while (s < e && (t[s] == ' ' || t[s] == '\t')) s++;
        if (s < e && t[s] == '[') {
            int ne = s + 1;
            while (ne < e && t[ne] != ']') ne++;
            in = word_is(t + s + 1, ne - s - 1, section);
        } else if (in && s < e && t[s] != ';') {
            int eq = s;
            while (eq < e && t[eq] != '=') eq++;
            if (eq < e) {
                int ke = eq;
                while (ke > s && (t[ke - 1] == ' ' || t[ke - 1] == '\t')) ke--;
                fn(ctx, t + s, ke - s, t + eq + 1, e - eq - 1);
            }
        }
        pos = e;
        while (pos < len && (t[pos] == '\n' || t[pos] == '\r')) pos++;
    }
}

static void apply_cb(void *ctx, const char *k, int kn, const char *v, int vn)
{
    apply(ctx, k, kn, v, vn);
}

struct int_ctx { const char *key; int value; };

static void int_cb(void *ctx, const char *k, int kn, const char *v, int vn)
{
    struct int_ctx *c = ctx;
    if (word_is(k, kn, c->key))
        c->value = number(v, vn, c->value);
}

/* A number from OS1993.INI (System VM thread: it reads through DOS). */
int vmset_ini_int(const char *section, const char *key, int def)
{
    const char *t;
    int len = read_ini(&t);
    struct int_ctx c = { key, def };
    if (len > 0)
        each(t, len, section, int_cb, &c);
    return c.value;
}

void vmset_load(struct vm_settings *set)
{
    *set = vm_defaults;
    const char *t;
    int len = read_ini(&t);
    if (len <= 0) {
        klog("VMM: no OS1993.INI (%d): the default DOS session settings\n", len);
        return;
    }
    each(t, len, "dos", apply_cb, set);
    if (set->mem_kb && set->mem_kb < 64) set->mem_kb = 64;
    if (set->xms_kb > 32768) set->xms_kb = 32768;
    if (set->dpmi_kb > 61440) set->dpmi_kb = 61440;
    klog("VMM: DOS session settings: memory %u KB, XMS %u KB, DPMI %u KB, devices %02X, idle %u, mouse %u\n",
         set->mem_kb, set->xms_kb, set->dpmi_kb, set->devices, set->idle, set->mouse);
}

bool vmset_valid(const struct vm_settings *s)
{
    return (!s->mem_kb || (s->mem_kb >= 64 && s->mem_kb <= 640)) &&
           s->xms_kb <= 32768 && s->dpmi_kb <= 61440 && !(s->devices & ~VMDEV_ALL);
}

/*
 * Memory=: the free block of the VM's DOS memory chain that spans the
 * limit (in its private memory) is split there, and what lies above goes
 * to a block DOS owns itself (owner 0008h), so nothing can allocate it;
 * the BIOS data area says the same (INT 12h).
 */
void vmset_apply_memory(struct vm *vm)
{
    uint16_t top = (uint16_t)(vm->set.mem_kb * 64);     /* paragraphs */
    if (!vm->set.mem_kb || vm->set.mem_kb >= 640 || !dosmgr_first_mcb())
        return;
    uint16_t seg = dosmgr_first_mcb();
    for (int guard = 0; guard < 1000 && seg < 0xA000; guard++) {
        uint8_t *mcb = vm_ptr(vm, (uint32_t)seg << 4);
        if (mcb[0] != 'M' && mcb[0] != 'Z')
            return;
        uint16_t size = *(uint16_t *)(mcb + 3), owner = *(uint16_t *)(mcb + 1);
        uint32_t end = (uint32_t)seg + 1 + size;
        if (end > top) {
            if (owner || seg + 2u >= top || ((uint32_t)seg << 4) < g_bootinfo->loader_base)
                return;                                 /* not a free block we may cut */
            uint8_t type = mcb[0];
            *(uint16_t *)(mcb + 3) = (uint16_t)(top - seg - 1);
            mcb[0] = 'M';
            uint8_t *rest = vm_ptr(vm, (uint32_t)top << 4);
            rest[0] = type;
            *(uint16_t *)(rest + 1) = 0x0008;           /* DOS's own */
            *(uint16_t *)(rest + 3) = (uint16_t)(end - top - 1);
            memset(rest + 5, 0, 3);
            memcpy(rest + 8, "SC\0\0\0\0\0\0", 8);
            *(uint16_t *)vm_ptr(vm, 0x413) = vm->set.mem_kb;
            klog("VMM: VM %d has %u KB of conventional memory\n", vm->id, vm->set.mem_kb);
            return;
        }
        if (mcb[0] == 'Z')
            return;
        seg = (uint16_t)end;
    }
}
