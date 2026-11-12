/*
 * vmouse.c -- a mouse for DOS sessions: the INT 33h driver, in the monitor.
 *
 * The kernel owns the PS/2 mouse (mouse.c). While a DOS session has the
 * focus, its movements and buttons go to that VM's virtual mouse, and the
 * VM's INT 33h calls are answered here (Microsoft Mouse driver functions
 * 00h-26h, as far as text-mode programs use them), never reaching DOS. The
 * text-mode pointer is the usual software cursor: the cell under it is
 * ANDed with the screen mask and XORed with the cursor mask.
 *
 * An event handler (function 0Ch/14h) is called the way a real driver calls
 * it, as a far call with AX = events, BX = buttons, CX/DX = position,
 * SI/DI = mickeys: when the VM can take an interrupt, its state is saved,
 * the handler runs with a return address at a monitor breakpoint
 * (BRK_MOUSE_RET), and the state comes back when it returns.
 */
#include "kernel.h"
#include "devices.h"
#include "io.h"
#include "mm.h"
#include "sched.h"
#include "v86.h"
#include "vm.h"

static inline uint8_t bda8(struct vm *vm, uint32_t off)   { return *(uint8_t *)vm_ptr(vm, 0x400 + off); }
static inline uint16_t bda16(struct vm *vm, uint32_t off) { return *(uint16_t *)vm_ptr(vm, 0x400 + off); }

static bool text_mode(struct vm *vm)
{
    uint8_t mode = bda8(vm, 0x49);
    return mode <= 3 || mode == 7;
}

static int cols(struct vm *vm)
{
    int c = bda16(vm, 0x4A);
    return c >= 40 && c <= 132 ? c : 80;
}

static int rows(struct vm *vm)
{
    int r = bda8(vm, 0x84) + 1;
    return r >= 25 && r <= 60 ? r : 25;
}

static int cell_w(struct vm *vm) { return cols(vm) == 40 ? 16 : 8; }

static void defaults(struct vm *vm)
{
    struct vmouse *m = &vm->mouse;
    uint32_t h = m->handler, mask = m->mask;
    bool keep = m->in_handler;
    memset(m, 0, sizeof *m);
    if (keep) { m->handler = h; m->mask = mask; m->in_handler = true; }
    m->shown = -1;
    m->max_x = (int16_t)(text_mode(vm) ? cols(vm) * cell_w(vm) - 1 : 639);
    m->max_y = (int16_t)(text_mode(vm) ? rows(vm) * 8 - 1 : 199);
    m->x = (int16_t)((m->max_x + 1) / 2);
    m->y = (int16_t)((m->max_y + 1) / 2);
    m->screen_mask = 0x77FF;
    m->cursor_mask = 0x7700;
    m->ratio_x = 8;
    m->ratio_y = 16;
    m->saved_cell = -1;
}

/* ---- the text cursor ---------------------------------------------------- */

static uint16_t *cell_ptr(struct vm *vm, int x, int y)
{
    int cx = x / cell_w(vm), cy = y / 8;
    if (cx >= cols(vm)) cx = cols(vm) - 1;
    if (cy >= rows(vm)) cy = rows(vm) - 1;
    uint32_t base = bda8(vm, 0x49) == 7 ? 0xB0000 : 0xB8000;
    uint32_t off = bda16(vm, 0x4E) + (uint32_t)(cy * cols(vm) + cx) * 2;
    if (off >= 0x8000 - 1)
        return NULL;
    return vm_ptr(vm, base + off);
}

static void erase(struct vm *vm)
{
    struct vmouse *m = &vm->mouse;
    if (m->saved_cell < 0)
        return;
    uint16_t *c = m->saved_ptr ? vm_ptr(vm, m->saved_ptr) : NULL;
    if (c && *c == m->drawn)            /* the program didn't write over it */
        *c = (uint16_t)m->saved_cell;
    m->saved_cell = -1;
}

static void draw(struct vm *vm)
{
    struct vmouse *m = &vm->mouse;
    if (m->shown < 0 || !text_mode(vm))
        return;
    uint16_t *c = cell_ptr(vm, m->x, m->y);
    if (!c)
        return;
    m->saved_cell = *c;
    m->drawn = (uint16_t)((*c & m->screen_mask) ^ m->cursor_mask);
    *c = m->drawn;
    /* remember where, as a VM address, for erase() */
    int cx = m->x / cell_w(vm), cy = m->y / 8;
    if (cx >= cols(vm)) cx = cols(vm) - 1;
    if (cy >= rows(vm)) cy = rows(vm) - 1;
    m->saved_ptr = (bda8(vm, 0x49) == 7 ? 0xB0000 : 0xB8000) + bda16(vm, 0x4E) + (uint32_t)(cy * cols(vm) + cx) * 2;
}

/* Focus moves: take the cursor off a VM's screen before it is copied away,
 * and put it back on the one that gets the screen. */
void vmouse_focus(struct vm *vm, bool on)
{
    if (!vm || !vm->set.mouse)
        return;
    uint32_t f = irq_save();
    if (on) draw(vm);
    else    erase(vm);
    irq_restore(f);
}

/* ---- input (the mouse interrupt, while vm has the focus) ---------------- */

static void clamp(struct vmouse *m)
{
    if (m->x < m->min_x) m->x = m->min_x;
    if (m->x > m->max_x) m->x = m->max_x;
    if (m->y < m->min_y) m->y = m->min_y;
    if (m->y > m->max_y) m->y = m->max_y;
}

/* dx, dy: relative motion (PS/2 counts, y down), or abs: 0..65535 each. */
void vmouse_input(struct vm *vm, int dx, int dy, bool abs, uint8_t buttons)
{
    struct vmouse *m = &vm->mouse;
    if (!vm->set.mouse || !m->reset)
        return;
    int ox = m->x, oy = m->y;
    if (abs) {
        m->x = (int16_t)(m->min_x + (int)((uint32_t)dx * (uint32_t)(m->max_x - m->min_x + 1) / 65536));
        m->y = (int16_t)(m->min_y + (int)((uint32_t)dy * (uint32_t)(m->max_y - m->min_y + 1) / 65536));
        dx = (m->x - ox) * m->ratio_x / 8;
        dy = (m->y - oy) * m->ratio_y / 8;
    } else {
        m->frac_x += dx * 8;
        m->frac_y += dy * 8;
        int px = m->frac_x / m->ratio_x, py = m->frac_y / m->ratio_y;
        m->frac_x -= px * m->ratio_x;
        m->frac_y -= py * m->ratio_y;
        m->x = (int16_t)(m->x + px);
        m->y = (int16_t)(m->y + py);
    }
    clamp(m);
    m->mick_x = (int16_t)(m->mick_x + dx);
    m->mick_y = (int16_t)(m->mick_y + dy);

    uint16_t ev = 0;
    if (m->x != ox || m->y != oy) ev |= 0x01;
    for (int b = 0; b < 3; b++) {
        bool was = m->buttons & (1u << b), now = buttons & (1u << b);
        if (now && !was) {
            ev |= (uint16_t)(b == 0 ? 0x02 : b == 1 ? 0x08 : 0x20);
            m->press[b]++;
            m->press_x[b] = m->x;
            m->press_y[b] = m->y;
        } else if (!now && was) {
            ev |= (uint16_t)(b == 0 ? 0x04 : b == 1 ? 0x10 : 0x40);
            m->release[b]++;
            m->release_x[b] = m->x;
            m->release_y[b] = m->y;
        }
    }
    m->buttons = buttons & 7;
    if (ev & 0x01 && (m->x / cell_w(vm) != ox / cell_w(vm) || m->y / 8 != oy / 8)) {
        erase(vm);
        draw(vm);
    }
    if (ev & m->mask) {
        m->pending |= ev & m->mask;
        thread_wake(vm->thread);
    }
}

/* ---- calling the event handler ----------------------------------------- */

void vmouse_deliver(struct vm *vm, struct regs *r)
{
    struct vmouse *m = &vm->mouse;
    if (!m->pending || m->in_handler || !m->handler || !vm->vif)
        return;
    struct vmouse_saved *s = &m->saved;
    s->eax = r->eax; s->ebx = r->ebx; s->ecx = r->ecx; s->edx = r->edx;
    s->esi = r->esi; s->edi = r->edi; s->ebp = r->ebp;
    s->ds = r->v86_ds; s->es = r->v86_es;
    s->cs = r->cs; s->eip = r->eip; s->ss = r->user_ss; s->esp = r->user_esp;
    s->flags = v86_vm_flags(vm, r);
    r->eax = m->pending;
    r->ebx = m->buttons;
    r->ecx = (uint16_t)m->x;
    r->edx = (uint16_t)m->y;
    r->esi = (uint16_t)m->mick_x;
    r->edi = (uint16_t)m->mick_y;
    m->pending = 0;
    m->in_handler = true;
    v86_push16(r, v86_buf_seg());               /* RETF lands on the breakpoint */
    v86_push16(r, v86_brk_off(BRK_MOUSE_RET));
    r->cs = m->handler >> 16;
    r->eip = m->handler & 0xFFFF;
    vm->vif = false;                            /* as from the mouse interrupt */
}

static void handler_returned(struct vm *vm, struct regs *r, int n)
{
    (void)n;
    struct vmouse *m = &vm->mouse;
    struct vmouse_saved *s = &m->saved;
    r->eax = s->eax; r->ebx = s->ebx; r->ecx = s->ecx; r->edx = s->edx;
    r->esi = s->esi; r->edi = s->edi; r->ebp = s->ebp;
    r->v86_ds = s->ds; r->v86_es = s->es;
    r->cs = s->cs; r->eip = s->eip; r->user_ss = s->ss; r->user_esp = s->esp;
    v86_set_vm_flags(vm, r, s->flags);
    m->in_handler = false;
}

/* ---- INT 33h ------------------------------------------------------------ */

#define AX(r) ((uint16_t)(r)->eax)
#define SET16(reg, v) ((reg) = ((reg) & 0xFFFF0000u) | (uint16_t)(v))

/* An INT 33h from the VM: true if answered here. */
bool vmouse_int33(struct vm *vm, struct regs *r)
{
    if (!vm->set.mouse || !mouse_present())
        return false;
    struct vmouse *m = &vm->mouse;
    uint32_t f = irq_save();
    int b;
    switch (AX(r)) {
    case 0x00:                                  /* reset */
    case 0x21:                                  /* software reset */
        erase(vm);
        m->handler = 0;
        m->mask = 0;
        m->in_handler = false;
        defaults(vm);
        m->reset = true;
        SET16(r->eax, 0xFFFF);
        SET16(r->ebx, 3);
        break;
    case 0x01:                                  /* show */
        if (m->shown < 0 && ++m->shown == 0)
            draw(vm);
        break;
    case 0x02:                                  /* hide */
        if (m->shown-- == 0)
            erase(vm);
        break;
    case 0x03:                                  /* position, buttons */
        SET16(r->ebx, m->buttons);
        SET16(r->ecx, m->x);
        SET16(r->edx, m->y);
        break;
    case 0x04:                                  /* set position */
        erase(vm);
        m->x = (int16_t)r->ecx;
        m->y = (int16_t)r->edx;
        clamp(m);
        draw(vm);
        break;
    case 0x05:                                  /* press information */
    case 0x06: {                                /* release information */
        bool press = AX(r) == 0x05;
        b = (int)(uint16_t)r->ebx;
        if (b > 2) b = 0;
        SET16(r->eax, m->buttons);
        SET16(r->ebx, press ? m->press[b] : m->release[b]);
        SET16(r->ecx, press ? m->press_x[b] : m->release_x[b]);
        SET16(r->edx, press ? m->press_y[b] : m->release_y[b]);
        if (press) m->press[b] = 0;
        else       m->release[b] = 0;
        break;
    }
    case 0x07:                                  /* horizontal range */
    case 0x08: {                                /* vertical range */
        int16_t lo = (int16_t)r->ecx, hi = (int16_t)r->edx;
        if (lo > hi) { int16_t t = lo; lo = hi; hi = t; }
        if (AX(r) == 7) { m->min_x = lo; m->max_x = hi; }
        else            { m->min_y = lo; m->max_y = hi; }
        erase(vm);
        clamp(m);
        draw(vm);
        break;
    }
    case 0x09:                                  /* graphics cursor: none drawn */
        break;
    case 0x0A:                                  /* text cursor */
        erase(vm);
        if (!(uint16_t)r->ebx) {
            m->screen_mask = (uint16_t)r->ecx;
            m->cursor_mask = (uint16_t)r->edx;
        }
        draw(vm);
        break;
    case 0x0B:                                  /* motion counters */
        SET16(r->ecx, m->mick_x);
        SET16(r->edx, m->mick_y);
        m->mick_x = m->mick_y = 0;
        break;
    case 0x0C:                                  /* event handler */
        m->mask = (uint16_t)r->ecx;
        m->handler = (uint32_t)(uint16_t)r->v86_es << 16 | (uint16_t)r->edx;
        m->pending = 0;
        break;
    case 0x14: {                                /* swap event handlers */
        uint32_t oh = m->handler;
        uint16_t om = m->mask;
        m->mask = (uint16_t)r->ecx;
        m->handler = (uint32_t)(uint16_t)r->v86_es << 16 | (uint16_t)r->edx;
        m->pending = 0;
        SET16(r->ecx, om);
        SET16(r->edx, oh);
        r->v86_es = oh >> 16;
        break;
    }
    case 0x0F:                                  /* mickeys per 8 pixels */
        m->ratio_x = (int16_t)((uint16_t)r->ecx ? (uint16_t)r->ecx : 8);
        m->ratio_y = (int16_t)((uint16_t)r->edx ? (uint16_t)r->edx : 16);
        break;
    case 0x10:                                  /* conditional off */
        if (m->shown-- == 0)
            erase(vm);
        break;
    case 0x15:                                  /* state size */
        SET16(r->ebx, 16);
        break;
    case 0x1B:                                  /* sensitivity */
        SET16(r->ebx, 50);
        SET16(r->ecx, 50);
        SET16(r->edx, 50);
        break;
    case 0x1D: case 0x1E:                       /* display page */
        SET16(r->ebx, 0);
        break;
    case 0x1F:                                  /* disable the driver */
        erase(vm);
        m->reset = false;
        SET16(r->eax, 0x1F);
        break;
    case 0x20:                                  /* enable it */
        m->reset = true;
        break;
    case 0x24:                                  /* version, type, IRQ */
        SET16(r->ebx, 0x0626);
        SET16(r->ecx, 0x040C);                  /* PS/2, IRQ 12 */
        break;
    case 0x26:                                  /* maximum coordinates */
        SET16(r->ebx, 0);
        SET16(r->ecx, m->max_x);
        SET16(r->edx, m->max_y);
        break;
    default:                                    /* 13h, 16h-1Ah, 1Ch, ...: accepted */
        break;
    }
    irq_restore(f);
    return true;
}

void vmouse_init(void)
{
    v86_set_brk(BRK_MOUSE_RET, handler_returned);
}
