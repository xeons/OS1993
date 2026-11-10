/*
 * display.c -- the GUI's display modes: VGA's 640x480x16, and the higher
 * resolutions and colour depths of the Bochs/QEMU VBE adapter.
 *
 * VGA mode 12h is always there (planar, 16 colours). Beyond it, this is a
 * display driver for one card, as Windows' were: the VBE adapter that
 * Bochs and QEMU emulate, which their VBE BIOS drives through two ports
 * (1CEh index, 1CFh data). Programming it directly gives a linear frame-
 * buffer (at E0000000h; QEMU's ISA machine's BIOS only offers banked VBE
 * modes) and mode switches without the BIOS, so they also work where the
 * BIOS can't be called -- switching to a DOS session from the keyboard
 * interrupt, the emergency screen. (A driver for other cards would use
 * the VBE BIOS on the console thread: see vbe.c, which lists its modes.)
 *
 * The framebuffer and the off-screen copy of the screen (the "back
 * buffer", in the screen's format) are mapped in the shared region, the
 * same in every process. Modes are offered in 8 (palette), 16 (5:6:5) and
 * 32 bits (8:8:8) at sizes that fit the adapter's memory.
 */
#include "kernel.h"
#include "devices.h"
#include "display.h"
#include "io.h"
#include "mm.h"
#include "user.h"
#include "vga.h"

#define DISPI_INDEX 0x01CE
#define DISPI_DATA  0x01CF
#define DISPI_LFB   0xE0000000u
enum { D_ID, D_XRES, D_YRES, D_BPP, D_ENABLE, D_BANK, D_VWIDTH, D_VHEIGHT, D_XOFF, D_YOFF, D_VRAM64K };
#define D_ENABLED   0x01
#define D_LFB       0x40

#define PLANAR_BACK (4 * VGA_PLANE_BYTES)

static bool dispi;
static uint32_t vram;
static bool linear;                     /* the adapter's mode is on */
static struct display_info cur;
static uint32_t back_mapped, fb_mapped;
static uint8_t dac[768];

static void dw(int i, uint16_t v) { outw(DISPI_INDEX, (uint16_t)i); outw(DISPI_DATA, v); }
static uint16_t dr(int i)         { outw(DISPI_INDEX, (uint16_t)i); return inw(DISPI_DATA); }

void display_init(void)
{
    dw(D_ID, 0xB0C5);
    uint16_t id = dr(D_ID);
    if (id < 0xB0C0 || id > 0xB0C5) {
        klog("DISPLAY: VGA (no Bochs/QEMU VBE adapter; id %04X)\n", id);
        return;
    }
    dispi = true;
    vram = id >= 0xB0C2 && dr(D_VRAM64K) ? (uint32_t)dr(D_VRAM64K) * 65536 : 4u << 20;
    if (vram > USER_FB_MAX)
        vram = USER_FB_MAX;
    klog("DISPLAY: Bochs/QEMU VBE adapter %04X, %u KB, linear framebuffer at %08X\n",
         id, vram / 1024, DISPI_LFB);
}

static const struct { uint16_t w, h; } sizes[] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 1024 } };
static const uint8_t depths[] = { 8, 16, 32 };

static bool fits(uint32_t w, uint32_t h, uint32_t bpp)
{
    uint32_t bytes = w * h * (bpp / 8);
    return bytes <= vram && bytes <= USER_BACK_MAX;
}

int display_modes(struct display_mode *out, int max)
{
    int n = 0;
    if (n < max)
        out[n++] = (struct display_mode){ 640, 480, 4, { 0 } };
    for (unsigned s = 0; dispi && s < sizeof sizes / sizeof sizes[0]; s++)
        for (unsigned d = 0; d < sizeof depths; d++)
            if (n < max && fits(sizes[s].w, sizes[s].h, depths[d]))
                out[n++] = (struct display_mode){ sizes[s].w, sizes[s].h, depths[d], { 0 } };
    return n;
}

const struct display_info *display_current(void) { return &cur; }
bool display_linear(void) { return linear; }

/* ---- mapping (in a process's address space: the shared page tables) ------ */

static void unmap_range(uint32_t va, uint32_t bytes, bool free)
{
    for (uint32_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint32_t frame = vmm_unmap(va + off);
        if (free && frame)
            pmm_free(frame);
    }
}

static bool map_back(uint32_t bytes)
{
    unmap_range(USER_BACK, back_mapped, true);
    back_mapped = 0;
    for (uint32_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint32_t frame = pmm_alloc();
        if (!frame || vmm_map(USER_BACK + off, frame, PTE_W | PTE_U) != 0) {
            if (frame) pmm_free(frame);
            return false;
        }
        memset((void *)(uintptr_t)(USER_BACK + off), 0, PAGE_SIZE);
        back_mapped = off + PAGE_SIZE;
    }
    return true;
}

static void planar_info(void)
{
    uint32_t gen = cur.gen;
    cur = (struct display_info){ .width = VGA_GFX_W, .height = VGA_GFX_H, .bpp = 4,
                                 .pitch = VGA_GFX_W / 8, .fb = USER_VGA, .back = USER_BACK,
                                 .back_pitch = VGA_GFX_W / 8, .gen = gen };
}

/* A graphics session starts in VGA mode 12h (the first process's context). */
bool display_session_start(void)
{
    linear = false;
    planar_info();
    cur.gen++;
    mouse_set_screen(VGA_GFX_W, VGA_GFX_H);
    return map_back(PLANAR_BACK);
}

void display_session_end(void)                      /* the last process's context */
{
    if (linear)
        dw(D_ENABLE, 0);
    linear = false;
    unmap_range(USER_FB, fb_mapped, false);
    unmap_range(USER_BACK, back_mapped, true);
    fb_mapped = back_mapped = 0;
    planar_info();
    mouse_set_screen(VGA_GFX_W, VGA_GFX_H);
}

/* ---- switching -------------------------------------------------------------- */

static void adapter_on(void)
{
    dw(D_ENABLE, 0);
    dw(D_XRES, cur.width);
    dw(D_YRES, cur.height);
    dw(D_BPP, cur.bpp);
    dw(D_VWIDTH, cur.width);
    dw(D_XOFF, 0);
    dw(D_YOFF, 0);
    dw(D_ENABLE, D_ENABLED | D_LFB);
}

int display_set(int w, int h, int bpp)
{
    bool ok = false;
    struct display_mode list[16];
    int n = display_modes(list, 16);
    for (int i = 0; i < n; i++)
        if (list[i].width == w && list[i].height == h && list[i].bpp == bpp)
            ok = true;
    if (!ok)
        return -1;
    uint32_t bytes = bpp == 4 ? PLANAR_BACK : (uint32_t)w * (uint32_t)h * (uint32_t)(bpp / 8);
    if (linear) {
        unmap_range(USER_FB, fb_mapped, false);
        fb_mapped = 0;
    }
    if (!map_back(bytes)) {                         /* out of memory: back to VGA */
        bpp = 4;
        map_back(PLANAR_BACK);
    }
    if (bpp == 4) {
        if (linear) {
            dw(D_ENABLE, 0);
            vga_planar_restore();
        }
        linear = false;
        planar_info();
    } else {
        uint32_t gen = cur.gen;
        cur = (struct display_info){ .width = (uint16_t)w, .height = (uint16_t)h, .bpp = (uint8_t)bpp,
                                     .pitch = (uint32_t)w * (uint32_t)(bpp / 8), .fb = USER_FB,
                                     .back = USER_BACK, .back_pitch = (uint32_t)w * (uint32_t)(bpp / 8),
                                     .gen = gen };
        if (bpp == 16) {
            cur.red_pos = 11; cur.red_size = 5; cur.green_pos = 5; cur.green_size = 6;
            cur.blue_pos = 0; cur.blue_size = 5;
        } else if (bpp == 32) {
            cur.red_pos = 16; cur.red_size = 8; cur.green_pos = 8; cur.green_size = 8;
            cur.blue_pos = 0; cur.blue_size = 8;
        }
        for (uint32_t off = 0; off < bytes; off += PAGE_SIZE) {
            if (vmm_map(USER_FB + off, DISPI_LFB + off, PTE_W | PTE_U | PTE_PCD) != 0)
                break;
            fb_mapped = off + PAGE_SIZE;
        }
        adapter_on();
        linear = true;
    }
    cur.gen++;
    mouse_set_screen(cur.width, cur.height);
    klog("DISPLAY: %ux%u, %u bits\n", cur.width, cur.height, cur.bpp);
    return 0;
}

/* The screen goes to text mode (a DOS session, the emergency screen) and
 * comes back. The GUI repaints everything when it's back; in 256 colours
 * the palette is kept. Callable from interrupt handlers: ports only. */
void display_suspend(void)
{
    if (!linear)
        return;
    if (cur.bpp == 8) {
        outb(0x3C7, 0);
        for (int i = 0; i < 768; i++)
            dac[i] = inb(0x3C9);
    }
    dw(D_ENABLE, 0);
}

void display_resume(void)
{
    if (!linear)
        return;
    adapter_on();
    if (cur.bpp == 8) {
        outb(0x3C8, 0);
        for (int i = 0; i < 768; i++)
            outb(0x3C9, dac[i]);
    }
}

void display_force_off(void)                        /* panic: no questions */
{
    if (dispi)
        dw(D_ENABLE, 0);
    linear = false;
}
