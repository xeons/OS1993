/*
 * xms.c -- XMS 3.0 and INT 15h's extended memory calls for DOS sessions,
 * in the monitor (as Windows' enhanced mode virtualized HIMEM per VM).
 *
 * INT 2Fh AX=4300h/4310h tell a session's programs about an XMS driver
 * whose entry point is a monitor breakpoint (BRK_XMS): each VM has its own
 * extended memory blocks, set.xms_kb of them, in its extended memory window
 * (vmx.c). Moves copy between the blocks and the VM's conventional memory;
 * a lock gives the block's linear address, which a DPMI client of the same
 * VM can use directly and INT 15h AH=87h accepts. The HMA is DOS's (DOS=
 * HIGH), A20 is always on, and there are no UMBs to hand out.
 *
 * INT 15h AH=88h says there is no extended memory outside XMS (as with
 * HIMEM loaded); AH=87h block moves work between conventional memory and
 * locked blocks, without ever switching the processor's mode.
 */
#include "kernel.h"
#include "v86.h"
#include "vm.h"

#define HANDLES 32
#define SET16(reg, v) ((reg) = ((reg) & 0xFFFF0000u) | (uint16_t)(v))
#define AX(r) ((uint16_t)(r)->eax)

static uint32_t lin_of(uint16_t seg, uint16_t off) { return ((uint32_t)seg << 4) + off; }

static bool fail(struct regs *r, uint8_t code)
{
    SET16(r->eax, 0);
    r->ebx = (r->ebx & ~0xFFu) | code;
    return false;
}

static void ok(struct regs *r) { SET16(r->eax, 1); }

static uint32_t block(struct vm *vm, uint16_t h)
{
    return h >= 1 && h <= HANDLES ? vm->xms_handle[h - 1] : 0;
}

static int free_handles(struct vm *vm)
{
    int n = 0;
    for (int i = 0; i < HANDLES; i++)
        if (!vm->xms_handle[i]) n++;
    return n;
}

/* Bytes of memory at a VM address: conventional memory (below 1 MB + 64
 * KB), or its extended memory window. */
static void *mem(struct vm *vm, uint32_t lin, uint32_t len)
{
    if (lin < 0x110000 && len <= 0x110000 - lin)
        return (void *)(uintptr_t)lin;          /* the VM's own low memory (PDE 0 is its) */
    return vmx_ptr(vm, lin, len);
}

/* Function 0Bh: move. DS:SI -> { len, src handle, src offset, dst handle, dst offset } */
static void move(struct vm *vm, struct regs *r)
{
    const uint8_t *m = mem(vm, lin_of((uint16_t)r->v86_ds, (uint16_t)r->esi), 16);
    if (!m) { fail(r, 0x82); return; }
    uint32_t len = *(const uint32_t *)m;
    uint16_t sh = *(const uint16_t *)(m + 4), dh = *(const uint16_t *)(m + 10);
    uint32_t so = *(const uint32_t *)(m + 6), dof = *(const uint32_t *)(m + 12);
    if (len & 1) { fail(r, 0xA7); return; }
    uint32_t src, dst;
    if (sh) {
        uint32_t b = block(vm, sh);
        if (!b) { fail(r, 0xA3); return; }
        if (so > vmx_size(vm, b) || len > vmx_size(vm, b) - so) { fail(r, 0xA4); return; }
        src = b + so;
    } else {
        src = lin_of((uint16_t)(so >> 16), (uint16_t)so);
    }
    if (dh) {
        uint32_t b = block(vm, dh);
        if (!b) { fail(r, 0xA5); return; }
        if (dof > vmx_size(vm, b) || len > vmx_size(vm, b) - dof) { fail(r, 0xA6); return; }
        dst = b + dof;
    } else {
        dst = lin_of((uint16_t)(dof >> 16), (uint16_t)dof);
    }
    void *s = mem(vm, src, len), *d = mem(vm, dst, len);
    if (!s) { fail(r, 0xA4); return; }
    if (!d) { fail(r, 0xA6); return; }
    memmove(d, s, len);
    ok(r);
}

static void xms_call(struct vm *vm, struct regs *r)
{
    uint8_t fn = (uint8_t)(r->eax >> 8);
    uint16_t dx = (uint16_t)r->edx;
    uint32_t b;
    r->ebx &= ~0xFFu;                           /* BL = 0: no error */
    switch (fn) {
    case 0x00:                                  /* version */
        SET16(r->eax, 0x0300);
        SET16(r->ebx, 0x0100);
        SET16(r->edx, 1);                       /* an HMA exists (DOS has it) */
        break;
    case 0x01: fail(r, 0x91); break;            /* HMA: in use */
    case 0x02: fail(r, 0x93); break;            /* HMA: not ours */
    case 0x03: case 0x04: case 0x05: case 0x06: /* A20: always on */
        ok(r);
        break;
    case 0x07:                                  /* query A20 */
        ok(r);
        break;
    case 0x08: case 0x88: {                     /* free memory */
        uint32_t kb = vmx_avail(vm, VMX_XMS) / 1024;
        if (fn == 0x88) {
            r->eax = kb;
            r->edx = kb;
            r->ecx = VMX_BASE + VMX_SIZE - 1;
        } else {
            SET16(r->eax, kb > 0xFFFF ? 0xFFFF : kb);
            SET16(r->edx, kb > 0xFFFF ? 0xFFFF : kb);
        }
        if (!kb) r->ebx |= 0xA0;
        break;
    }
    case 0x09: case 0x89: {                     /* allocate */
        uint32_t kb = fn == 0x89 ? r->edx : dx;
        int h = -1;
        for (int i = 0; i < HANDLES && h < 0; i++)
            if (!vm->xms_handle[i]) h = i;
        if (h < 0) { fail(r, 0xA1); break; }
        b = vmx_alloc(vm, kb * 1024, VMX_XMS);
        if (!b) { fail(r, 0xA0); break; }
        vm->xms_handle[h] = b;
        ok(r);
        SET16(r->edx, h + 1);
        break;
    }
    case 0x0A:                                  /* free */
        b = block(vm, dx);
        if (!b) { fail(r, 0xA2); break; }
        if (vmx_lock(vm, b, 0) > 0) { fail(r, 0xAB); break; }
        vmx_free(vm, b);
        vm->xms_handle[dx - 1] = 0;
        ok(r);
        break;
    case 0x0B:
        move(vm, r);
        break;
    case 0x0C:                                  /* lock: its 32-bit address */
        b = block(vm, dx);
        if (!b) { fail(r, 0xA2); break; }
        if (vmx_lock(vm, b, 1) < 0) { fail(r, 0xAC); break; }
        ok(r);
        SET16(r->edx, b >> 16);
        SET16(r->ebx, b);
        break;
    case 0x0D:                                  /* unlock */
        b = block(vm, dx);
        if (!b) { fail(r, 0xA2); break; }
        if (vmx_lock(vm, b, -1) < 0) { fail(r, 0xAA); break; }
        ok(r);
        break;
    case 0x0E: case 0x8E:                       /* handle information */
        b = block(vm, dx);
        if (!b) { fail(r, 0xA2); break; }
        ok(r);
        r->ebx = (r->ebx & 0xFFFF0000u) | (uint32_t)(vmx_lock(vm, b, 0) & 0xFF) << 8 |
                 (uint32_t)(fn == 0x0E ? free_handles(vm) : 0);
        if (fn == 0x8E) {
            SET16(r->ecx, free_handles(vm));
            r->edx = vmx_size(vm, b) / 1024;
        } else {
            SET16(r->edx, vmx_size(vm, b) / 1024);
        }
        break;
    case 0x0F: case 0x8F: {                     /* reallocate */
        uint32_t kb = fn == 0x8F ? r->ebx : (uint16_t)r->ebx;
        b = block(vm, dx);
        if (!b) { fail(r, 0xA2); break; }
        if (vmx_lock(vm, b, 0) > 0) { fail(r, 0xAB); break; }
        uint32_t nb = vmx_resize(vm, b, kb * 1024);
        if (!nb) { fail(r, 0xA0); break; }
        vm->xms_handle[dx - 1] = nb;
        ok(r);
        break;
    }
    case 0x10: fail(r, 0xB1); break;            /* UMB: none */
    case 0x11: case 0x12: fail(r, 0xB2); break;
    default:
        fail(r, 0x80);                          /* not implemented */
    }
}

/* The far call to the entry point landed on its breakpoint. */
static void entry(struct vm *vm, struct regs *r, int n)
{
    (void)n;
    xms_call(vm, r);
    r->eip = v86_pop16(r);                      /* RETF */
    r->cs = v86_pop16(r);
}

bool xms_int2f(struct vm *vm, struct regs *r)
{
    if (vm == system_vm)
        return false;                           /* the System VM keeps HIMEM */
    if (AX(r) == 0x4300) {                      /* installed (XMS=0: not at all) */
        r->eax = (r->eax & ~0xFFu) | (vm->set.xms_kb ? 0x80 : 0x00);
        return true;
    }
    if (!vm->set.xms_kb)
        return false;
    if (AX(r) == 0x4310) {
        r->v86_es = v86_buf_seg();
        SET16(r->ebx, v86_brk_off(BRK_XMS));
        return true;
    }
    return false;
}

static uint32_t desc_base(const uint8_t *d)
{
    return d[2] | (uint32_t)d[3] << 8 | (uint32_t)d[4] << 16 | (uint32_t)d[7] << 24;
}

/* INT 15h AH=87h/88h. */
bool xms_int15(struct vm *vm, struct regs *r)
{
    uint8_t ah = (uint8_t)(r->eax >> 8);
    if (vm == system_vm || (ah != 0x87 && ah != 0x88))
        return false;
    if (ah == 0x88) {                           /* extended memory outside XMS: none */
        SET16(r->eax, 0);
        r->eflags &= ~EFLAGS_CF;
        return true;
    }
    /* ES:SI -> a GDT: [2] source, [3] destination; CX words */
    const uint8_t *gdt = mem(vm, lin_of((uint16_t)r->v86_es, (uint16_t)r->esi), 48);
    uint32_t len = (uint32_t)(uint16_t)r->ecx * 2;
    void *s = gdt ? mem(vm, desc_base(gdt + 0x10), len) : NULL;
    void *d = gdt ? mem(vm, desc_base(gdt + 0x18), len) : NULL;
    if (!s || !d || (uint16_t)r->ecx > 0x8000) {
        klog("XMS: INT 15h AH=87h refused: %08X -> %08X, %u bytes\n",
             gdt ? desc_base(gdt + 0x10) : 0, gdt ? desc_base(gdt + 0x18) : 0, len);
        r->eax = (r->eax & 0xFFFF00FFu) | 0x0300;       /* AH=03h: address error */
        r->eflags |= EFLAGS_CF;
        return true;
    }
    memmove(d, s, len);
    r->eax &= 0xFFFF00FFu;                      /* AH=0 */
    r->eflags &= ~EFLAGS_CF;
    return true;
}

void xms_init(void)
{
    v86_set_brk(BRK_XMS, entry);
}
