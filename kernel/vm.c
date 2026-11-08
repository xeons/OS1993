/*
 * vm.c -- creating, switching and destroying virtual machines.
 */
#include "kernel.h"
#include "console.h"
#include "devices.h"
#include "gdt.h"
#include "io.h"
#include "mm.h"
#include "sched.h"
#include "v86.h"
#include "vm.h"

#define PTE_V86   (PTE_P | PTE_W | PTE_U)
#define VIDEO_BYTES (VM_VIDEO_PAGES * PAGE_SIZE)

static struct vm vms[VM_MAX];
struct vm *const system_vm = &vms[0];

static bool     ready;
static struct vm *focus;                    /* NULL = VMM console */
static uint32_t snapshot[VM_PRIV_PAGES];    /* boot-time copy of private pages */
static uint32_t pt0_template[1024];         /* System VM's PT 0 at boot */
static uint8_t  dos_mask1, dos_mask2;

static uint32_t priv_page_index(uint32_t page)
{
    if (page < VM_LOW_PAGES)
        return page;
    if (page >= VM_HMA_PAGE && page < VM_HMA_PAGE + VM_HMA_PAGES)
        return VM_LOW_PAGES + (page - VM_HMA_PAGE);
    return (uint32_t)-1;
}

static uint32_t priv_page_number(uint32_t i)
{
    return i < VM_LOW_PAGES ? i : VM_HMA_PAGE + (i - VM_LOW_PAGES);
}

static volatile uint32_t *vm_pt(struct vm *vm)
{
    return phys_to_virt(vm->pt0_phys);
}

/* Point a VM's video pages at its buffer, or at the real video RAM. */
static void map_video(struct vm *vm, bool real)
{
    volatile uint32_t *pt = vm_pt(vm);
    for (unsigned i = 0; i < VM_VIDEO_PAGES; i++) {
        uint32_t frame = real ? (VM_VIDEO_PAGE + i) * PAGE_SIZE : vm->video[i];
        pt[VM_VIDEO_PAGE + i] = frame | PTE_V86;
    }
}

static bool alloc_frames(uint32_t *out, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        out[i] = pmm_alloc();
        if (!out[i]) {
            while (i--) { pmm_free(out[i]); out[i] = 0; }
            return false;
        }
    }
    return true;
}

static void free_frames(uint32_t *f, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (f[i]) { pmm_free(f[i]); f[i] = 0; }
}

static void blank_video(struct vm *vm)
{
    for (unsigned i = 0; i < VM_VIDEO_PAGES; i++) {
        uint16_t *p = phys_to_virt(vm->video[i]);
        for (unsigned j = 0; j < PAGE_SIZE / 2; j++)
            p[j] = 0x0720;
    }
}

void vm_system_init(const struct bootinfo *bi)
{
    dos_mask1 = bi->pic1_mask;
    dos_mask2 = bi->pic2_mask;

    struct vm *vm = system_vm;
    vm->id = 0;
    vm->state = VM_RUNNING;
    memcpy(vm->name, "System VM", 10);
    vm->pt0_phys = ((volatile uint32_t *)PD_WINDOW)[0] & PAGE_MASK;
    for (unsigned i = 0; i < VM_PRIV_PAGES; i++)
        vm->priv[i] = priv_page_number(i) * PAGE_SIZE;     /* identity */
    vpic_init(&vm->pic, dos_mask1, dos_mask2);

    /* Template page table and boot snapshot for new VMs */
    memcpy(pt0_template, (const void *)vm_pt(vm), sizeof pt0_template);
    if (!alloc_frames(snapshot, VM_PRIV_PAGES) || !alloc_frames(vm->video, VM_VIDEO_PAGES))
        panic("Not enough memory for the VM snapshot.");
    for (unsigned i = 0; i < VM_PRIV_PAGES; i++)
        memcpy(phys_to_virt(snapshot[i]), KLOW(priv_page_number(i) * PAGE_SIZE), PAGE_SIZE);

    /* The console has the screen; the System VM's own video goes to a buffer */
    blank_video(vm);
    map_video(vm, false);
    tlb_flush();
    focus = NULL;
    ready = true;
}

static void vm_thread_main(void *arg)
{
    struct vm *vm = arg;
    vm->start_tick = timer_ticks();
    vm->body(vm);
    vm->end_tick = timer_ticks();
    uint32_t f = irq_save();
    if (focus == vm)
        vm_set_focus(NULL);
    vm->state = VM_FINISHED;
    irq_restore(f);
}

struct vm *vm_create(const char *name, void (*body)(struct vm *), void *arg)
{
    struct vm *vm = NULL;
    for (int i = 1; i < VM_MAX; i++)
        if (vms[i].state == VM_FREE) { vm = &vms[i]; break; }
    if (!vm)
        return NULL;

    memset(vm, 0, sizeof *vm);
    vm->id = (int)(vm - vms);
    size_t n = strlen(name);
    memcpy(vm->name, name, n < sizeof vm->name ? n + 1 : sizeof vm->name);
    vm->name[sizeof vm->name - 1] = '\0';

    if (!alloc_frames(vm->priv, VM_PRIV_PAGES))
        return NULL;
    if (!alloc_frames(vm->video, VM_VIDEO_PAGES)) {
        free_frames(vm->priv, VM_PRIV_PAGES);
        return NULL;
    }
    vm->pt0_phys = pmm_alloc();
    if (!vm->pt0_phys) {
        free_frames(vm->priv, VM_PRIV_PAGES);
        free_frames(vm->video, VM_VIDEO_PAGES);
        return NULL;
    }

    for (unsigned i = 0; i < VM_PRIV_PAGES; i++)
        memcpy(phys_to_virt(vm->priv[i]), phys_to_virt(snapshot[i]), PAGE_SIZE);
    blank_video(vm);

    volatile uint32_t *pt = vm_pt(vm);
    for (unsigned i = 0; i < 1024; i++)
        pt[i] = pt0_template[i];
    for (unsigned i = 0; i < VM_PRIV_PAGES; i++)
        pt[priv_page_number(i)] = vm->priv[i] | PTE_V86;
    map_video(vm, false);

    vpic_init(&vm->pic, dos_mask1, dos_mask2);
    vm->body = body;
    vm->arg = arg;
    vm->thread = thread_create(vm->name, vm_thread_main, vm, vm);
    if (!vm->thread) {
        free_frames(vm->priv, VM_PRIV_PAGES);
        free_frames(vm->video, VM_VIDEO_PAGES);
        pmm_free(vm->pt0_phys);
        return NULL;
    }
    vm->state = VM_READY;
    return vm;
}

void vm_start(struct vm *vm)
{
    vm->state = VM_RUNNING;
    thread_wake(vm->thread);
}

void vm_reap(void)
{
    for (int i = 1; i < VM_MAX; i++) {
        struct vm *vm = &vms[i];
        if (vm->state != VM_FINISHED || !thread_reap(vm->thread))
            continue;
        free_frames(vm->priv, VM_PRIV_PAGES);
        free_frames(vm->video, VM_VIDEO_PAGES);
        pmm_free(vm->pt0_phys);
        memset(vm, 0, sizeof *vm);
    }
}

int vm_count(void)
{
    int n = 0;
    for (int i = 1; i < VM_MAX; i++)
        if (vms[i].state == VM_READY || vms[i].state == VM_RUNNING)
            n++;
    return n;
}

struct vm *vm_get(int id)
{
    return (id >= 0 && id < VM_MAX && vms[id].state != VM_FREE) ? &vms[id] : NULL;
}

struct vm *vm_current(void)
{
    return current ? current->vm : system_vm;
}

void vm_switch_to(struct vm *vm)
{
    if (!vm)
        return;
    volatile uint32_t *pd = (volatile uint32_t *)PD_WINDOW;
    if ((pd[0] & PAGE_MASK) != vm->pt0_phys) {
        pd[0] = vm->pt0_phys | PTE_V86;
        tlb_flush();
    }
    if (vm->active)
        kernel_tss.esp0 = vm->kernel_esp;
}

void *vm_ptr(struct vm *vm, uint32_t linear)
{
    uint32_t page = linear >> PAGE_SHIFT, frame;
    uint32_t idx = priv_page_index(page);
    if (idx != (uint32_t)-1)
        frame = vm->priv[idx];
    else if (page >= VM_VIDEO_PAGE && page < VM_VIDEO_PAGE + VM_VIDEO_PAGES && vm != focus)
        frame = vm->video[page - VM_VIDEO_PAGE];
    else
        frame = page * PAGE_SIZE;                   /* shared */
    return (uint8_t *)phys_to_virt(frame) + (linear & ~PAGE_MASK);
}

/* ---- focus: who owns the screen and keyboard --------------------------- */

static void set_cursor_from_bda(struct vm *vm)
{
    const uint8_t *bda = vm_ptr(vm, 0x400);
    uint8_t col = bda[0x50], row = bda[0x51];       /* page 0 cursor */
    uint8_t end = bda[0x60], start = bda[0x61];     /* cursor shape */
    uint16_t pos = (uint16_t)(row * 80 + col);
    outb(0x3D4, 0x0A); outb(0x3D5, start);
    outb(0x3D4, 0x0B); outb(0x3D5, end);
    outb(0x3D4, 0x0E); outb(0x3D5, pos >> 8);
    outb(0x3D4, 0x0F); outb(0x3D5, pos & 0xFF);
}

void vm_set_focus(struct vm *target)
{
    uint32_t f = irq_save();
    struct vm *old = focus;
    if (old == target || !ready) {
        irq_restore(f);
        return;
    }
    uint8_t *real = KLOW(VM_VIDEO_PAGE * PAGE_SIZE);

    if (old) {                                      /* old VM to its buffer */
        for (unsigned i = 0; i < VM_VIDEO_PAGES; i++)
            memcpy(phys_to_virt(old->video[i]), real + i * PAGE_SIZE, PAGE_SIZE);
        map_video(old, false);
    } else {
        con_detach();                               /* console to its shadow */
    }

    if (target) {                                   /* new VM onto the screen */
        for (unsigned i = 0; i < VM_VIDEO_PAGES; i++)
            memcpy(real + i * PAGE_SIZE, phys_to_virt(target->video[i]), PAGE_SIZE);
        map_video(target, true);
        set_cursor_from_bda(target);
    } else {
        con_attach();
    }
    tlb_flush();
    focus = target;
    irq_restore(f);
}

void vm_focus_next(void)
{
    int start = focus ? focus->id : 0;
    for (int n = 1; n <= VM_MAX; n++) {
        int id = (start + n) % VM_MAX;
        if (id == 0) { vm_set_focus(NULL); return; }        /* the console */
        if (vms[id].state == VM_RUNNING) { vm_set_focus(&vms[id]); return; }
    }
}

struct vm *vm_focus(void) { return focus; }

/* ---- interrupt routing ------------------------------------------------- */

void vm_raise_irq(struct vm *vm, unsigned irq)
{
    vpic_raise(&vm->pic, irq);
    thread_wake(vm->thread);
}

/* Device IRQs nobody virtualizes go to the System VM, which loaded the
 * drivers for them. */
void vm_hw_irq(unsigned irq)
{
    if (!ready)
        return;
    vpic_hw_irq(&system_vm->pic, irq);
    thread_wake(system_vm->thread);
}

/* Share a timer tick with every VM. A VM whose previous tick is still
 * undelivered gets its BIOS tick count advanced directly. */
bool vm_timer_tick(void)
{
    if (!ready)
        return false;
    for (int i = 0; i < VM_MAX; i++) {
        struct vm *vm = &vms[i];
        if (vm->state != VM_RUNNING)
            continue;
        if (!vpic_raise(&vm->pic, 0)) {
            uint32_t *ticks = vm_ptr(vm, 0x46C);
            if (++*ticks >= 0x1800B0) {
                *ticks = 0;
                *(uint8_t *)vm_ptr(vm, 0x470) = 1;
            }
        }
        thread_wake(vm->thread);
    }
    return true;
}

void vm_restore_dos(void)
{
    if (!ready)
        return;
    vm_set_focus(NULL);
    map_video(system_vm, true);                     /* identity again */
    vm_switch_to(system_vm);
    tlb_flush();
}
