/*
 * vbe.c -- VESA BIOS Extensions: the modes beyond VGA's.
 *
 * The VGA BIOS's VBE functions (INT 10h AX=4Fxx) run in the System VM like
 * any BIOS call, so this is console-thread code. At boot the controller
 * and each mode are described (4F00h, 4F01h); the modes kept are the
 * graphics modes of at least 640x480 with a linear framebuffer, in 256
 * colours (8 bits, through the DAC) or direct colour (15, 16, 24, 32 bits).
 * vbe_set() switches with 4F02h (bit 14: linear framebuffer).
 */
#include "kernel.h"
#include "v86.h"
#include "vbe.h"

static struct vbe_mode modes[VBE_MODE_MAX];
static int nmodes;
static uint16_t version;
static uint32_t memory_kb;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }

void vbe_init(void)
{
    uint16_t seg = v86_buf_seg(), off = v86_buf_off();
    uint8_t *info = v86_ptr(seg, off);
    memset(info, 0, 512);
    memcpy(info, "VBE2", 4);                         /* ask for the VBE 2.0 fields */
    struct v86_regs r = { .eax = 0x4F00, .es = seg, .edi = off };
    if (v86_int(0x10, &r) != 0 || (r.eax & 0xFFFF) != 0x004F || memcmp(info, "VESA", 4)) {
        klog("VBE: none\n");
        return;
    }
    version = rd16(info + 4);
    memory_kb = (uint32_t)rd16(info + 18) * 64;
    uint16_t list_off = rd16(info + 14), list_seg = rd16(info + 16);

    uint16_t numbers[128];                           /* the list may live in the buffer */
    int count = 0;
    for (; count < 128; count++) {
        uint16_t m = *(const uint16_t *)v86_ptr(list_seg, (uint16_t)(list_off + count * 2));
        if (m == 0xFFFF)
            break;
        numbers[count] = m;
    }
    for (int i = 0; i < count && nmodes < VBE_MODE_MAX; i++) {
        uint8_t *mi = v86_ptr(seg, off);
        memset(mi, 0, 256);
        struct v86_regs q = { .eax = 0x4F01, .ecx = numbers[i], .es = seg, .edi = off };
        if (v86_int(0x10, &q) != 0 || (q.eax & 0xFFFF) != 0x004F)
            continue;
        uint16_t attr = rd16(mi);
        uint8_t model = mi[27], bpp = mi[25];
        uint32_t lfb = rd32(mi + 40);
        struct vbe_mode v = {
            .number = numbers[i], .width = rd16(mi + 18), .height = rd16(mi + 20), .bpp = bpp,
            .pitch = version >= 0x300 && rd16(mi + 50) ? rd16(mi + 50) : rd16(mi + 16),
            .lfb = lfb,
            .red_size = mi[31], .red_pos = mi[32], .green_size = mi[33], .green_pos = mi[34],
            .blue_size = mi[35], .blue_pos = mi[36],
        };
        bool ok = (attr & 0x91) == 0x91 && lfb &&      /* supported, graphics, linear */
                  v.width >= 640 && v.height >= 480 &&
                  ((model == 4 && bpp == 8) || (model == 6 && (bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32)));
        if (ok && bpp == 15) v.bpp = 15;
        if (ok && model == 6 && bpp == 16 && v.green_size == 5) v.bpp = 15;
        if (ok)
            modes[nmodes++] = v;
    }
    klog("VBE: version %u.%u, %u KB, %d usable modes\n", version >> 8, version & 0xFF,
         memory_kb, nmodes);
    for (int i = 0; i < nmodes; i++)
        klog("VBE:   %03X  %ux%u  %u bits  pitch %u  at %08X\n", modes[i].number, modes[i].width,
             modes[i].height, modes[i].bpp, modes[i].pitch, modes[i].lfb);
}

int vbe_mode_count(void) { return nmodes; }
const struct vbe_mode *vbe_mode(int i) { return i >= 0 && i < nmodes ? &modes[i] : NULL; }

const struct vbe_mode *vbe_find(int w, int h, int bpp)
{
    for (int i = 0; i < nmodes; i++)
        if (modes[i].width == w && modes[i].height == h && modes[i].bpp == bpp)
            return &modes[i];
    return NULL;
}

bool vbe_set(const struct vbe_mode *m)
{
    struct v86_regs r = { .eax = 0x4F02, .ebx = (uint32_t)m->number | 0x4000 };
    return v86_int(0x10, &r) == 0 && (r.eax & 0xFFFF) == 0x004F;
}
