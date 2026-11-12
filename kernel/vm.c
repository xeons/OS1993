/*
 * vm.c -- creating, switching and destroying virtual machines.
 */
#include "kernel.h"
#include "fs.h"
#include "console.h"
#include "devices.h"
#include "gdt.h"
#include "io.h"
#include "mm.h"
#include "sched.h"
#include "v86.h"
#include "user.h"
#include "vga.h"
#include "vm.h"

#define PTE_V86   (PTE_P | PTE_W | PTE_U)

static struct vm vms[VM_MAX];
struct vm *const system_vm = &vms[0];

static bool     ready;                      /* snapshot taken: VMs can be created */
static bool     irq_ready;                  /* System VM exists: route IRQs */
static struct vm *focus;                    /* NULL = VMM console */
static uint8_t  dos_mask1, dos_mask2;

/* What a new VM may have, unless it is told otherwise (vmset.c). */
struct vm_settings vm_defaults = {
    .mem_kb = 0, .xms_kb = 2048, .dpmi_kb = 4096,
    .devices = VMDEV_ALL & ~VMDEV_COM1,     /* COM1 is the VMM's debug line */
    .idle = 16, .mouse = true,
};

/* Private pages: [priv_first, VM_LOW_PAGES) and the BIOS RAM pages.
 * priv_page[i] is the page number of private slot i; priv_slot[page] the
 * reverse (-1 for shared pages). */
static unsigned priv_first, priv_count;
static uint16_t priv_page[VM_PRIV_MAX];
static int16_t  priv_slot[VM_V86_PAGES];
static uint32_t snapshot[VM_PRIV_MAX];      /* boot-time copy of private pages */
static uint32_t pt0_template[1024];         /* System VM's PT 0 at boot */

/* Instance data: shared addresses with per-VM contents */
static struct { uint32_t lin, len; } inst[VM_INST_MAX];
static unsigned ninst;
static uint32_t inst_total;
static uint8_t *inst_snapshot;
static struct vm *inst_owner;               /* whose data is live in memory */

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

/* ---- instance data ----------------------------------------------------- */

/* Register shared memory whose contents each VM keeps its own copy of.
 * Overlapping or touching regions are merged. Returns false (and changes
 * nothing) for regions that aren't in shared memory. */
bool vm_add_instance(uint32_t lin, uint32_t len)
{
    if (ready)
        panic("vm_add_instance after the VM snapshot.");
    if (!len || lin + len > KLOW_SIZE)
        return false;
    for (uint32_t p = lin >> PAGE_SHIFT; p <= (lin + len - 1) >> PAGE_SHIFT; p++)
        if (priv_slot[p] >= 0)
            return false;

    uint32_t end = lin + len;
    for (unsigned i = 0; i < ninst; ) {          /* absorb overlapping regions */
        uint32_t s = inst[i].lin, e = s + inst[i].len;
        if (lin <= e && s <= end) {
            if (s < lin) lin = s;
            if (e > end) end = e;
            inst_total -= inst[i].len;
            inst[i] = inst[--ninst];
            i = 0;
            continue;
        }
        i++;
    }
    if (ninst == VM_INST_MAX)
        panic("Too many VM instance data regions.");
    inst[ninst].lin = lin;
    inst[ninst].len = end - lin;
    inst_total += end - lin;
    ninst++;
    return true;
}

static void inst_copy(uint8_t *buf, bool save)
{
    uint32_t off = 0;
    for (unsigned i = 0; i < ninst; i++) {
        uint8_t *live = KLOW(inst[i].lin);
        if (save) memcpy(buf + off, live, inst[i].len);
        else      memcpy(live, buf + off, inst[i].len);
        off += inst[i].len;
    }
}

static void inst_swap_to(struct vm *vm)
{
    if (inst_owner == vm || !ready)
        return;
    inst_copy(inst_owner->inst, true);
    inst_copy(vm->inst, false);
    inst_owner = vm;
}

/* Kernel view of a VM's instance data, or NULL if lin isn't instanced. */
static uint8_t *inst_ptr(struct vm *vm, uint32_t lin)
{
    if (!ready || vm == inst_owner)
        return NULL;
    uint32_t off = 0;
    for (unsigned i = 0; i < ninst; i++) {
        if (lin >= inst[i].lin && lin < inst[i].lin + inst[i].len)
            return vm->inst + off + (lin - inst[i].lin);
        off += inst[i].len;
    }
    return NULL;
}

/* ---- the System VM ------------------------------------------------------ */

void vm_system_init(const struct bootinfo *bi)
{
    dos_mask1 = bi->pic1_mask;
    dos_mask2 = bi->pic2_mask;
    priv_first = bi->loader_base >> PAGE_SHIFT;
    for (unsigned p = 0; p < VM_V86_PAGES; p++)
        priv_slot[p] = -1;
    priv_count = 0;
    for (unsigned p = priv_first; p < VM_LOW_PAGES; p++)
        priv_page[priv_count++] = (uint16_t)p;
    for (unsigned p = VM_BIOS_RAM_PAGE; p < VM_BIOS_RAM_PAGE + VM_BIOS_RAM_PAGES; p++)
        priv_page[priv_count++] = (uint16_t)p;
    for (unsigned i = 0; i < priv_count; i++)
        priv_slot[priv_page[i]] = (int16_t)i;

    struct vm *vm = system_vm;
    vm->id = 0;
    vm->state = VM_RUNNING;
    memcpy(vm->name, "System VM", 10);
    vm->pt0_phys = ((volatile uint32_t *)PD_WINDOW)[0] & PAGE_MASK;
    for (unsigned i = 0; i < priv_count; i++)
        vm->priv[i] = (uint32_t)priv_page[i] * PAGE_SIZE;   /* identity */
    vpic_init(&vm->pic, dos_mask1, dos_mask2);
    vpit_init(&vm->pit);
    vm->set = vm_defaults;
    inst_owner = vm;
    focus = NULL;
    irq_ready = true;

    vm_add_instance(0x000, 0x600);          /* IVT, BIOS data, DOS comm area */
}

void vm_finish_init(void)
{
    struct vm *vm = system_vm;

    memcpy(pt0_template, (const void *)vm_pt(vm), sizeof pt0_template);
    if (!alloc_frames(snapshot, priv_count) || !alloc_frames(vm->video, VM_VIDEO_PAGES))
        panic("Not enough memory for the VM snapshot.");
    for (unsigned i = 0; i < priv_count; i++)
        memcpy(phys_to_virt(snapshot[i]), KLOW((uint32_t)priv_page[i] * PAGE_SIZE), PAGE_SIZE);

    vm->inst = kmalloc(inst_total);
    inst_snapshot = kmalloc(inst_total);
    if (!vm->inst || !inst_snapshot)
        panic("Not enough memory for VM instance data.");
    inst_copy(inst_snapshot, true);

    /* The console has the screen; the System VM's own video goes to a buffer */
    blank_video(vm);
    map_video(vm, false);
    tlb_flush();
    ready = true;
    klog("VMM: VMs share memory below %05X; %u private pages (incl. BIOS RAM at %05X), "
         "%u bytes instance data\n",
         priv_first * PAGE_SIZE, priv_count, VM_BIOS_RAM_PAGE * PAGE_SIZE, inst_total);
}

/* ---- VM lifetime ------------------------------------------------------- */

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

static void vm_free_resources(struct vm *vm)
{
    redir_vm_end(vm);
    vdev_vm_gone(vm);
    dpmi_vm_gone(vm);
    vmx_vm_gone(vm);
    for (unsigned irq = 0; irq < 16; irq++)     /* IRQs it never acknowledged */
        if (vm->pic.inflight & (1u << irq))
            pic_unmask(irq);
    free_frames(vm->priv, priv_count);
    free_frames(vm->video, VM_VIDEO_PAGES);
    if (vm->pt0_phys) pmm_free(vm->pt0_phys);
    kfree(vm->inst);
    vm->pt0_phys = 0;
    vm->inst = NULL;
}

struct vm *vm_create(const char *name, void (*body)(struct vm *), void *arg)
{
    return vm_create_with(name, body, arg, &vm_defaults);
}

struct vm *vm_create_with(const char *name, void (*body)(struct vm *), void *arg,
                          const struct vm_settings *set)
{
    if (!ready)
        return NULL;
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

    vm->pt0_phys = pmm_alloc();
    vm->inst = kmalloc(inst_total);
    if (!vm->pt0_phys || !vm->inst ||
        !alloc_frames(vm->priv, priv_count) || !alloc_frames(vm->video, VM_VIDEO_PAGES)) {
        vm_free_resources(vm);
        return NULL;
    }

    for (unsigned i = 0; i < priv_count; i++)
        memcpy(phys_to_virt(vm->priv[i]), phys_to_virt(snapshot[i]), PAGE_SIZE);
    memcpy(vm->inst, inst_snapshot, inst_total);
    blank_video(vm);

    volatile uint32_t *pt = vm_pt(vm);
    for (unsigned i = 0; i < 1024; i++)
        pt[i] = pt0_template[i];
    for (unsigned i = 0; i < priv_count; i++)
        pt[priv_page[i]] = vm->priv[i] | PTE_V86;
    map_video(vm, false);

    vpic_init(&vm->pic, dos_mask1, dos_mask2);
    vpit_init(&vm->pit);
    vm->set = *set;
    vmset_apply_memory(vm);
    vm->body = body;
    vm->arg = arg;
    vm->thread = thread_create(vm->name, vm_thread_main, vm, vm);
    if (!vm->thread) {
        vm_free_resources(vm);
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
        uint32_t f = irq_save();
        if (inst_owner == vm)               /* never leave a dead VM's data live */
            inst_swap_to(vm_current());
        irq_restore(f);
        vm_free_resources(vm);
        memset(vm, 0, sizeof *vm);
        vpit_vm_gone();                     /* the real timer may slow down */
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

/* Ring-3 threads (the GUI) have no VM of their own; they belong to the
 * System VM, as Windows' protected-mode code did. */
struct vm *vm_current(void)
{
    return current && current->vm ? current->vm : system_vm;
}

void vm_switch_to(struct vm *vm)
{
    vmx_install(vm);                    /* its extended memory, or none (ring-3 threads) */
    if (!vm) {
        dpmi_install_ldt(NULL);
        return;
    }
    volatile uint32_t *pd = (volatile uint32_t *)PD_WINDOW;
    if ((pd[0] & PAGE_MASK) != vm->pt0_phys) {
        pd[0] = vm->pt0_phys | PTE_V86;
        tlb_flush();
    }
    inst_swap_to(vm);
    vdev_switch(vm);                    /* its devices' ports open, others' trapped */
    dpmi_install_ldt(vm);               /* its DPMI client's LDT, or none */
    if (vm->active)
        kernel_tss.esp0 = vm->kernel_esp - 16;
}

void *vm_ptr(struct vm *vm, uint32_t linear)
{
    uint8_t *ip = inst_ptr(vm, linear);
    if (ip)
        return ip;

    uint32_t page = linear >> PAGE_SHIFT, frame;
    if (page < VM_V86_PAGES && priv_slot[page] >= 0)
        frame = vm->priv[priv_slot[page]];
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

    vmouse_focus(old, false);                       /* the mouse cursor off its screen */
    if (old) {                                      /* old VM to its buffer */
        for (unsigned i = 0; i < VM_VIDEO_PAGES; i++)
            memcpy(phys_to_virt(old->video[i]), real + i * PAGE_SIZE, PAGE_SIZE);
        map_video(old, false);
    } else if (vga_console_gfx()) {
        vga_console_suspend();                      /* save the desktop, text mode */
    } else {
        con_detach();                               /* console to its shadow */
    }

    focus = target;
    if (target) {                                   /* new VM onto the screen */
        for (unsigned i = 0; i < VM_VIDEO_PAGES; i++)
            memcpy(real + i * PAGE_SIZE, phys_to_virt(target->video[i]), PAGE_SIZE);
        map_video(target, true);
        set_cursor_from_bda(target);
        vmouse_focus(target, true);
    } else if (vga_console_gfx()) {
        vga_console_resume();                       /* the desktop, exactly */
    } else {
        con_attach();
    }
    tlb_flush();
    user_screen(target == NULL);
    klog("VMM: focus -> %s\n", target ? target->name : "console");
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

/* The machine state V86 code runs in must belong to the VM of the current
 * thread. Checked on every trap/IRQ out of V86 mode and before returning. */
void vm_check(const char *where)
{
    struct vm *vm = vm_current();
    uint32_t pde0 = ((volatile uint32_t *)PD_WINDOW)[0] & PAGE_MASK;
    bool bad_inst = ready && inst_owner != vm;
    bool bad_pt   = pde0 != vm->pt0_phys;
    bool bad_esp0 = vm->active && kernel_tss.esp0 != vm->kernel_esp - 16;
    if (bad_inst || bad_pt || bad_esp0)
        panic("VM state mismatch %s: thread %s, VM %d; instance owner VM %d, "
              "PDE0 %08X (VM %08X), ESP0 %08X (VM %08X)",
              where, current ? current->name : "?", vm->id,
              inst_owner ? inst_owner->id : -1, pde0, vm->pt0_phys,
              kernel_tss.esp0, vm->kernel_esp);
}

/* ---- interrupt routing ------------------------------------------------- */

void vm_raise_irq(struct vm *vm, unsigned irq)
{
    vpic_raise(&vm->pic, irq);
    thread_wake(vm->thread);
}

/* Device IRQs nobody virtualizes go to the System VM, which loaded the
 * drivers for them -- unless a VM owns the device (vdev.c). */
void vm_hw_irq(unsigned irq)
{
    if (!irq_ready)
        return;
    struct vm *vm = vdev_irq_owner(irq);    /* a COM or LPT port's owner, or the System VM */
    vpic_hw_irq(&vm->pic, irq);
    thread_wake(vm->thread);
}

/* A real timer interrupt (clocks PIT clocks long; tick: the kernel's
 * 18.2 Hz tick is in it). Each VM whose own timer period ended gets a
 * virtual IRQ 0; a VM at the BIOS rate whose previous one is still
 * undelivered gets its BIOS tick count advanced directly instead. Every
 * kernel tick also wakes every VM, so VMs waiting for DOS re-check. */
bool vm_timer_irq(uint32_t clocks, bool tick)
{
    if (!irq_ready)
        return false;
    for (int i = 0; i < VM_MAX; i++) {
        struct vm *vm = &vms[i];
        if (vm->state != VM_RUNNING)
            continue;
        unsigned n = vpit_due(&vm->pit, clocks);
        if (n && !vpic_raise(&vm->pic, 0) && vm->pit.div == 65536) {
            uint32_t *ticks = vm_ptr(vm, 0x46C);
            if (++*ticks >= 0x1800B0) {
                *ticks = 0;
                *(uint8_t *)vm_ptr(vm, 0x470) = 1;
            }
        }
        if (n || tick)
            thread_wake(vm->thread);
    }
    return true;
}

void vm_restore_dos(void)
{
    if (!ready)
        return;
    vm_set_focus(NULL);
    vga_force_text();                               /* DOS gets a text screen */
    map_video(system_vm, true);                     /* identity again */
    vm_switch_to(system_vm);                        /* its PT 0 and instance data */
    tlb_flush();
}
