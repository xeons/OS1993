# OS1993

A hobby operating environment written as if it were 1993: a 32-bit,
paged, protected-mode **Virtual Machine Manager** that starts from the DOS
prompt, in the spirit of Windows 3.1's 386 Enhanced mode. Built with modern
tools; the output runs on a real (or emulated) 386 with DOS and HIMEM.SYS.

```
C:\> OS1993
```

## Building and running

Requirements: `nasm`, `i686-elf-gcc` / `i686-elf-binutils`, `dosbox-x`
(`qemu`, `bochs` and Open Watcom in `/opt/watcom` are for later stages).

| Command          | What it does                                                  |
|------------------|---------------------------------------------------------------|
| `make`           | builds `build/OS1993.COM` (loader) and `build/VMM.BIN` (kernel) |
| `make run`       | boots it in DOSBox-X: 386, 16 MB, XMS, no EMS                 |
| `make test`      | headless round trip DOS → VMM → DOS, checked automatically    |
| `make check386`  | fails if the kernel contains any post-386 or FPU instruction  |

Kernel debug output goes to COM1; under DOSBox-X it lands in
`build/SERIAL.LOG`.

## How it boots

1. **`OS1993.COM`** (real mode, `loader/os1993.asm`) checks for a 386 that is
   in real mode, not V86 mode under EMM386/QEMM. It then allocates the largest
   free extended memory block from HIMEM.SYS (XMS), locks it, and enables A20.
2. It reads **`VMM.BIN`** from its own directory and copies it into the XMS
   block with XMS moves.
3. It switches to protected mode and builds page tables:
   - `0–4 MB` identity mapped (DOS, BIOS, video RAM: the future V86 machine)
   - the whole XMS arena mapped at **`0x80000000`**, kernel first
   - PDE 1023 → page directory (recursive mapping)
4. It turns paging on and jumps to the kernel with `EBX` pointing at a
   `struct bootinfo` (`kernel/include/bootinfo.h`, the loader/kernel contract).
5. The **kernel** installs its own GDT and IDT, remaps the PICs to 20h–2Fh,
   and runs timer and keyboard interrupts. It keeps the BIOS tick count
   moving so DOS's clock stays right.
6. On exit, `exit_to_dos()` hands control back to the loader. The loader
   turns paging and protected mode off, restores the BIOS PIC layout and IVT,
   frees the XMS block, and returns to the prompt. The kernel's exit code
   becomes `ERRORLEVEL`.

Arena layout: `[kernel image + bss][page directory][PT 0–4MB][arena PTs][free …]`

## Kernel address space

| Linear range              | Use                                                     |
|---------------------------|---------------------------------------------------------|
| `00000000–003FFFFF`       | identity: DOS, BIOS, video RAM (the future V86 machine) |
| `80000000–83FFFFFF`       | XMS arena, direct mapped: kernel, then PMM frames       |
| `C0000000–CFFFFFFF`       | kernel heap (`kmalloc`), grown on demand                |
| `D0000000–D010FFFF`       | kernel window on physical 0–1 MB + HMA (`KLOW`)         |
| `D0000000–FFBFFFFF`       | free for future kernel mappings                         |
| `FFC00000–FFFFFFFF`       | recursive window: page tables; directory at `FFFFF000`  |

Memory manager (`kernel/include/mm.h`):
- `pmm.c` is a bitmap frame allocator over the arena. It panics on double
  frees and on frees of kernel frames.
- `paging.c` provides `vmm_map`/`vmm_unmap`/`vmm_translate` through the
  recursive mapping. TLB flushes reload CR3, because `invlpg` is a 486
  instruction.
- `heap.c` is `kmalloc`/`kfree`: first fit with boundary tags, coalescing,
  freed memory poisoned to 0xDD, and a full consistency checker
  (`heap_check`).
- `mmtest.c` is a self-test of all three that runs on every boot.

## The DOS virtual machine

The DOS that started OS1993 keeps running, as a virtual 8086 machine under
the VMM (`kernel/v86.c`, `kernel/vpic.c`):

- **Entering and leaving V86 mode.** `v86_enter` (`v86entry.asm`) IRETs into
  V86 mode with TSS.ESP0 pointing just below the kernel's saved state. Traps
  out of V86 mode land there, and `v86_leave` unwinds them (setjmp/longjmp).
- **Instruction emulation.** V86 code runs at IOPL 0, so `CLI`, `STI`,
  `PUSHF`, `POPF`, `INT n`, `IRET` and `HLT` raise #GP and are emulated
  against a virtual interrupt flag. CPU exceptions 0–7 are reflected to the
  real-mode IVT, as on a real 8086.
- **Ports.** The TSS I/O bitmap lets the VM drive hardware directly, except
  the PIC ports 20h/21h/A0h/A1h, which trap to the virtual PIC.
- **Virtual PIC.** IRQs without a kernel handler are reflected into DOS's
  handlers at the BIOS vectors (08h/70h). Their EOIs are virtualized, and the
  real IRQ stays masked until the VM acknowledges it. The timer is shared:
  undelivered ticks are credited straight to the BIOS tick count.
- **Kernel calls into DOS.** `v86_int(vec, &regs)` and `v86_call_far()` run
  real-mode code on the kernel's behalf. The return address is an `ARPL`
  instruction, which is invalid in V86 mode, so its #UD tells the monitor
  the call is done. This is the technique Win386 used.

## Multiple virtual machines

As in Windows 3.1's 386 Enhanced mode, the **System VM** is the DOS that
started OS1993 (the VMM console runs in it). Press **N** for a new **DOS
session**, a separate VM running `%COMSPEC%`. **Alt+Tab** cycles between
the console and the sessions, and `EXIT` ends a session.

- **Address spaces** (`kernel/vm.c`). At boot, the VMM snapshots
  conventional memory and the HMA. Each new VM gets private copies (its own
  IVT, BIOS data, DOS data, MCB chain and programs) and its own page table
  for 0–4 MB. Switching VMs swaps page-directory entry 0. ROM, UMB and
  adapter space are shared. A session costs about 740 KB.
- **Scheduler** (`kernel/sched.c`). One kernel thread per VM, preemptive
  round robin with ~110 ms slices, switched only at the end of an interrupt.
  A VM that executes `HLT` blocks until an interrupt is raised for it.
- **Virtual PIC per VM.** Timer ticks go to every VM, and a VM that misses
  a tick gets its BIOS clock advanced directly. Other device IRQs go to the
  System VM.
- **Virtual keyboard (VKD)** (`kernel/keyboard.c`). The kernel owns IRQ 1.
  Scancodes go to the focused VM's virtual 8042 (ports 60h/64h trap), so a
  VM only ever sees its own keys.
- **Virtual display (VDD).** Only the focused VM's B800 pages map to real
  video RAM. Background VMs draw into private buffers, and focus switches
  copy the buffers and restore the cursor. The console draws into a shadow
  buffer while a VM has the screen.

**Known limitation: DOS is not yet serialized between VMs.** Each VM has
its own copy of DOS's memory, but the disk is shared, and there is no DOS
critical section like Win386's DOSMGR. Under DOSBox-X this matters even
more: its DOS and COMMAND.COM are emulator built-ins (C++, not real DOS
code), so two sessions sitting at a prompt at the same time can break
DOSBox's internal state. One interactive session at a time is safe. Real
MS-DOS or FreeDOS under QEMU/Bochs is the next step.

Not handled yet: programs that switch to protected mode themselves (DOS
extenders, a real HIMEM's block moves). The VM is stopped with a message.
That needs VCPI/DPMI.

## Source layout

```
loader/os1993.asm        real-mode loader (.COM)
kernel/entry.asm         VMM.BIN header, _start, exit_to_dos, gdt/idt load
kernel/isr.asm           exception + IRQ stubs → interrupt_dispatch()
kernel/vmm.c             kmain
kernel/pmm.c paging.c heap.c mmtest.c   memory manager
kernel/v86.c v86entry.asm vpic.c         V86 monitor and virtual PIC
kernel/v86test.c dosshell.c              DOS calls from the kernel, DOS sessions
kernel/vm.c sched.c switch.asm           VMs, address spaces, focus; threads
kernel/vmtest.c                          multi-VM self-test
kernel/gdt.c idt.c pic.c timer.c keyboard.c console.c serial.c kprintf.c panic.c
kernel/include/          headers; bootinfo.h is shared with the loader
kernel/linker.ld         links at 0x80000000, header first
test/dosbox-x.conf       the emulated 1993 machine
tools/                   check386.sh, check-test.sh
```

## Rules of the house

- **386 only.** No `invlpg` (486): flush the TLB by reloading CR3. No FPU
  (`-mgeneral-regs-only`). No libgcc, so no 64-bit division. `make check386`
  enforces this.
- **The loader and kernel share `bootinfo`.** Change both sides together; the
  `_Static_assert`s and the `%if` in the loader catch layout drift.

## Roadmap

1. ~~**Memory manager.**~~ Done: frame allocator, paging API, kernel heap.
   Still to do: demand paging and a double-fault task gate, which would make
   a kernel stack guard page safe.
2. ~~**TSS + V86 monitor.**~~ Done: DOS runs as a V86 VM, the kernel calls
   DOS and BIOS through it, and the DOS prompt runs inside it.
3. ~~**Multiple VMs.**~~ Done: private address spaces, preemptive
   scheduling, virtual keyboard and display, Alt+Tab focus switching.
   Next: a DOS critical section (DOSMGR-style instancing of DOS's
   swappable data area) and a QEMU + FreeDOS test setup.
4. **DOS services from protected mode.** Call `INT 21h` inside the System VM
   for file I/O, behind a `fs_*` interface so a native FAT driver can replace
   it later (as 32-bit File Access did in Windows for Workgroups 3.11).
5. **More virtual devices (VxD-style).** The PIC, keyboard and text display
   are done. Still to do: the PIT (programs that reprogram the timer), CRTC
   ports for background VMs, graphics modes, mouse, serial and printer ports.
6. **Graphics.** VGA 640×480×16 driver, mouse, window manager, GUI shell.
7. **Win16-style API layer.** 16-bit protected-mode apps in ring 3 with LDTs,
   built with Open Watcom.
8. **Coexisting with EMM386** through VCPI/GEMMIS, and acting as a DPMI host.
