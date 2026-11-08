# OS1993

A hobby operating environment written as if it were 1993: a 32-bit,
paged, protected-mode **Virtual Machine Manager** that starts from the DOS
prompt, in the spirit of Windows 3.1's 386 Enhanced mode. Built with modern
tools; the output runs on a real (or emulated) 386 with DOS and HIMEM.SYS.

```
C:\> OS1993
```

## Building and running

Requirements: `nasm`, `i686-elf-gcc` / `i686-elf-binutils`, `dosbox-x`,
`qemu-system-i386`, `mtools`, `curl`. Bochs and Open Watcom (`/opt/watcom`)
are for later stages.

| Command          | What it does                                                  |
|------------------|---------------------------------------------------------------|
| `make`           | builds `build/OS1993.COM` (loader) and `build/VMM.BIN` (kernel) |
| `make run`       | boots it in DOSBox-X: 386, 16 MB, XMS, no EMS                 |
| `make test`      | headless round trip DOS → VMM → DOS, checked automatically    |
| `make check386`  | fails if the kernel contains any post-386 or FPU instruction  |
| `make qemu`      | boots real FreeDOS 1.3 in QEMU (486, 16 MB, ISA PC); `OS1993` is in `C:\OS1993` |
| `make qemu-test` | headless FreeDOS run of the same checks as `make test`        |
| `make qemu-emm386` | FreeDOS with JEMM386: the loader must refuse to start       |
| `make qemu-sessions` | types into two concurrent DOS sessions via QEMU's monitor |

There are two test machines because they catch different bugs:

- **DOSBox-X** is fast and can emulate a strict 386, but its DOS and
  COMMAND.COM are emulator built-ins written in C++.
- **QEMU + FreeDOS** runs a real DOS kernel, FreeCOM, HIMEMX and JEMM386.
  `tools/mkfreedos.sh` downloads the official FreeDOS 1.3 Lite image once
  into `.cache/freedos` and checks its SHA-256. It then writes its own
  `FDCONFIG.SYS`/`FDAUTO.BAT` and copies OS1993 onto the image with mtools.
  The `isa-debug-exit` device lets an automated run power off QEMU. QEMU has
  no 386 model, so it runs as a 486.

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

- **Address spaces** (`kernel/vm.c`), split the way Win386 split them:
  - *Shared*: everything below OS1993's loader (the DOS kernel and its
    buffers, file tables and FAT; drivers; the parent shell), the HMA, ROM
    and adapter space.
  - *Instance data*: shared addresses whose contents are swapped on every
    VM switch. That's the IVT and BIOS data (0000–05FF), DOS's swappable
    data area (INT 21h AX=5D06h) and the current-directory table.
  - *Private*: the loader's segment and everything above it up to 640 KB
    (each VM's own memory arena, MCB chain and programs), plus the BIOS's
    working RAM in the E000 segment. All of it is copied from a boot-time
    snapshot.

  Each VM has its own page table for 0–4 MB, and switching VMs swaps
  page-directory entry 0. A session costs about 660 KB.
- **One shared DOS** (`kernel/dosmgr.c`, a small DOSMGR). Only one VM may
  be inside DOS at a time. INT 20h/21h/25h/26h/27h are intercepted, and a
  caller blocks while another VM holds DOS. DOS's return address is pointed
  at a second ARPL breakpoint, so the monitor knows exactly when it returns.
  An EXEC stays open while the child runs, but the child has a different
  PSP, so the parent doesn't hold DOS meanwhile. A VM waiting for the
  keyboard inside DOS (INT 16h) or idling there (INT 28h) releases DOS and
  takes it back before continuing. INT 2Fh AX=1680h yields the time slice.
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

What "per VM" has to include was learned the hard way:

- **DOS's console buffers.** FreeDOS keeps its line-input and type-ahead
  buffers at DOS data offset 01FBh, outside the range INT 21h AX=5D06h
  reports. Two sessions at a prompt shared one line buffer. MS-DOS reports
  these to Windows through the INT 2Fh AX=1605h startup broadcast. The VMM
  does that broadcast too, but the distributed FreeDOS kernel is built
  without `WIN31SUPPORT` and answers with nothing, so the VMM instances
  01FBh–031Fh itself when the SDA is at the MS-DOS offset 0320h.
- **BIOS working RAM.** SeaBIOS (QEMU's BIOS) switches some interrupt
  handlers onto an internal stack at E000:FFF0. With that page shared, a VM
  paused inside the BIOS had its stack frames overwritten by another VM and
  resumed into garbage. It took an invalid-opcode fault, and FreeDOS killed
  the shell with exit code 7Fh. The E000 segment is now private per VM.

Debugging aids that stay in: invariant checks on every V86 entry/exit (the
running VM's page table, instance data and kernel stack must be live), a log
line for every CPU exception reflected into a VM, and a per-VM trace of the
last 48 DOS calls, dumped when a session ends abnormally.

Tests: `make qemu-test` runs two VMs whose batch files append 60 lines each
through the shared DOS at the same time. With the critical section disabled,
this test failed in 2 of 6 runs. `make qemu-sessions` types into two
interactive FreeDOS prompts through QEMU's monitor. It failed about one run
in four until both fixes above, then passed 30 runs in a row.

Under DOSBox-X, whose DOS and COMMAND.COM are emulator built-ins, each
batch file runs inside one emulator callback, so the two VMs' DOS work runs
one after the other rather than interleaved. DOSBox's CDS layout is also
different, so it isn't instanced there.

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
kernel/dosmgr.c                          one DOS shared by all VMs
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
   scheduling, virtual keyboard and display, Alt+Tab focus switching, and
   one DOS shared by all VMs with a critical section.
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
