/*
 * vdev.c -- serial and printer ports for the VMs (a small VCD and VPD).
 *
 * COM1-COM4 and LPT1-LPT3 each belong to at most one VM at a time. A VM
 * that touches a free device's ports gets it, if its settings allow
 * (vm->set.devices), and from then on the device's interrupt goes to that
 * VM instead of the System VM, and its ports are open to it -- the I/O
 * bitmap is switched with the VM, so its own port accesses run at full
 * speed while every other VM's still trap. A VM that touches a device
 * someone else has gets nothing (reads FFh, writes dropped), as Windows'
 * device contention did when the user said no; the device is free again
 * when its owner ends. COM1 stays the VMM's when it carries the debug log.
 */
#include "kernel.h"
#include "gdt.h"
#include "interrupts.h"
#include "io.h"
#include "serial.h"
#include "vm.h"

struct device {
    const char *name;
    uint16_t base, nports;
    uint8_t  irq, bit;
    struct vm *owner;
    bool     kernel;                /* the VMM's own (COM1 with the debug log) */
    int      refused_vm;            /* last VM told no (to log once) */
};

static struct device devs[] = {
    { "COM1", 0x3F8, 8, 4, VMDEV_COM1, NULL, false, -1 },
    { "COM2", 0x2F8, 8, 3, VMDEV_COM2, NULL, false, -1 },
    { "COM3", 0x3E8, 8, 4, VMDEV_COM3, NULL, false, -1 },
    { "COM4", 0x2E8, 8, 3, VMDEV_COM4, NULL, false, -1 },
    { "LPT1", 0x378, 3, 7, VMDEV_LPT1, NULL, false, -1 },
    { "LPT2", 0x278, 3, 5, VMDEV_LPT2, NULL, false, -1 },
    { "LPT3", 0x3BC, 3, 7, VMDEV_LPT3, NULL, false, -1 },
};

static struct vm *irq_owner[16];    /* NULL: the System VM */
static bool trapped_for;            /* the bitmap is set up */
static struct vm *bitmap_vm;        /* whose ports are open in the bitmap */

static struct device *dev_of(uint16_t port)
{
    for (unsigned i = 0; i < ARRAY_SIZE(devs); i++)
        if (port >= devs[i].base && port < devs[i].base + devs[i].nports)
            return &devs[i];
    return NULL;
}

static void set_trap(struct device *d, bool trap)
{
    for (uint16_t p = 0; p < d->nports; p++)
        tss_trap_port((uint16_t)(d->base + p), trap);
}

void vdev_init(void)
{
    devs[0].kernel = serial_present();
    for (unsigned i = 0; i < ARRAY_SIZE(devs); i++)
        set_trap(&devs[i], true);
    trapped_for = true;
}

bool vdev_port(uint16_t port) { return trapped_for && dev_of(port) != NULL; }

/* The scheduler switched to vm: open its devices' ports, close the rest. */
void vdev_switch(struct vm *vm)
{
    if (!trapped_for || vm == bitmap_vm)
        return;
    for (unsigned i = 0; i < ARRAY_SIZE(devs); i++)
        if (devs[i].owner && (devs[i].owner == vm || devs[i].owner == bitmap_vm))
            set_trap(&devs[i], devs[i].owner != vm);
    bitmap_vm = vm;
}

/* The real PIC follows the owner's virtual one for its devices' IRQs, so a
 * device whose IRQ DOS never enabled still reaches the VM that enables it. */
void vdev_imr_changed(struct vm *vm)
{
    for (unsigned i = 0; i < ARRAY_SIZE(devs); i++) {
        struct device *d = &devs[i];
        if (d->owner != vm)
            continue;
        uint16_t bit = (uint16_t)(1u << d->irq);
        if (!(vm->pic.imr & bit) && !(vm->pic.inflight & bit))
            pic_unmask(d->irq);
    }
}

static bool claim(struct vm *vm, struct device *d)
{
    if (d->owner == vm)
        return true;
    if (d->owner || d->kernel || !(vm->set.devices & d->bit)) {
        if (d->refused_vm != vm->id) {
            d->refused_vm = vm->id;
            if (d->owner)
                klog("VDEV: VM %d wants %s, which VM %d owns\n", vm->id, d->name, d->owner->id);
            else
                klog("VDEV: VM %d may not use %s%s\n", vm->id, d->name,
                     d->kernel ? " (the VMM's debug line)" : " (its settings)");
        }
        return false;
    }
    d->owner = vm;
    d->refused_vm = -1;
    irq_owner[d->irq] = vm;
    klog("VDEV: %s (IRQ %u) now belongs to VM %d\n", d->name, d->irq, vm->id);
    if (vm == vm_current()) {
        set_trap(d, false);         /* its own accesses no longer trap */
        bitmap_vm = vm;
    }
    vdev_imr_changed(vm);
    return true;
}

uint8_t vdev_in(struct vm *vm, uint16_t port)
{
    struct device *d = dev_of(port);
    return d && claim(vm, d) ? inb(port) : 0xFF;
}

void vdev_out(struct vm *vm, uint16_t port, uint8_t v)
{
    struct device *d = dev_of(port);
    if (d && claim(vm, d))
        outb(port, v);
}

/* Who gets a device IRQ the kernel doesn't handle itself. */
struct vm *vdev_irq_owner(unsigned irq)
{
    struct vm *vm = irq < 16 ? irq_owner[irq] : NULL;
    return vm && vm->state == VM_RUNNING ? vm : system_vm;
}

/* A VM ended: its devices are free again. */
void vdev_vm_gone(struct vm *vm)
{
    for (unsigned i = 0; i < ARRAY_SIZE(devs); i++) {
        struct device *d = &devs[i];
        if (d->owner != vm)
            continue;
        d->owner = NULL;
        if (irq_owner[d->irq] == vm)
            irq_owner[d->irq] = NULL;
        set_trap(d, true);
        klog("VDEV: %s is free again\n", d->name);
    }
    if (bitmap_vm == vm)
        bitmap_vm = NULL;
}
