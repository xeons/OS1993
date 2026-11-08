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

## Source layout

```
loader/os1993.asm        real-mode loader (.COM)
kernel/entry.asm         VMM.BIN header, _start, exit_to_dos, gdt/idt load
kernel/isr.asm           exception + IRQ stubs → interrupt_dispatch()
kernel/vmm.c             kmain
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

1. **Physical and virtual memory manager.** Page frame allocator over the
   arena, kernel heap, page fault handler.
2. **TSS + V86 monitor.** Run the DOS that started us as the first virtual
   machine, the "System VM": reflect interrupts into it, emulate `CLI`/`STI`,
   `IN`/`OUT` and `INT` through #GP.
3. **DOS services from protected mode.** Call `INT 21h` inside the System VM
   for file I/O, behind a `fs_*` interface so a native FAT driver can replace
   it later (as 32-bit File Access did in Windows for Workgroups 3.11).
4. **Virtual devices (VxD-style).** Virtual PIC, timer, keyboard, and display,
   so several DOS VMs can share the hardware. Preemptive scheduling between VMs.
5. **Graphics.** VGA 640×480×16 driver, mouse, window manager, GUI shell.
6. **Win16-style API layer.** 16-bit protected-mode apps in ring 3 with LDTs,
   built with Open Watcom.
7. **Coexisting with EMM386** through VCPI/GEMMIS, and acting as a DPMI host.
