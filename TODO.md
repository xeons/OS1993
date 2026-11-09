# OS1993 TODO

Known gaps and next steps, roughly in the order they'd be tackled. See
README.md for what already works.

## GUI and UI library

- [ ] **Menus**: a per-window menu bar (Windows 95 style), drop-down
      menus, keyboard access (Alt, mnemonics), check marks and separators.
- [ ] **System menu**: the title bar's system box currently only closes on
      double-click; it needs Restore / Move / Size / Minimize / Maximize / Close.
- [ ] More controls: combo box (drop-down list), tab control, spin box,
      multi-line edit with scrolling, tree view, tooltips.
- [ ] Standard dialogs: message box (OK / Cancel / Yes / No), file open/save.
- [ ] Edit controls: text selection, clipboard (cut/copy/paste), horizontal
      scrolling done properly.
- [ ] Mouse cursor shapes: resize arrows over borders, I-beam over edits,
      wait cursor.
- [ ] Keyboard: accelerators, Alt+Space for the system menu, Alt+Esc,
      arrow keys between radio buttons in a group.
- [ ] Proportional font for window text (the VGA 8x16 font is all there is).
- [ ] Desktop: icons, wallpaper pattern, and a shell with a **taskbar**
      (minimized windows currently become tiles along the bottom).
- [ ] Program Manager-style launcher that starts real application programs.
- [ ] Layout: controls don't reflow when a window is resized.
- [ ] Repainting: the dirty region is a single bounding rectangle; a region
      list would repaint less when far-apart things change.

## Ring 3 and DLLs

- [ ] **Several ring-3 programs at once**, each with its own address space
      (per-process page directories), DLL code shared between them and
      DLL data per process (copy-on-write or per-process data pages).
- [ ] Kernel page directory entries must then stay identical in every
      address space (see the TODO in `kernel/paging.c`).
- [ ] DLL initialization / termination entry points (DllMain-style).
- [ ] Import by ordinal; versioning; a loader error shown in the GUI
      instead of only the debug log.
- [ ] File I/O system calls for ring-3 programs (through DOS in the
      System VM, behind an `fs_*` interface).
- [ ] Memory allocation system calls (a user heap).
- [ ] Message/event queue per window instead of one global event poll.

## Graphics

- [ ] **VESA truecolor backend** (VBE mode set via INT 10h 4F02h, linear
      or banked framebuffer). The UI uses theme colours only, so it can map
      them to RGB without changes.
- [ ] VDD for DOS sessions that switch to graphics modes (currently text
      mode only).
- [ ] Trap CRTC ports for background DOS sessions (a background session's
      BIOS can still move the real cursor).
- [ ] DOS sessions in a window on the desktop (render the VM's text screen).
- [ ] Monochrome adapters get no graphics desktop.

## DOS virtual machines

- [ ] Mouse for DOS sessions: virtual PS/2 aux device and/or an INT 33h
      driver in the VM.
- [ ] **VCPI / DPMI host**, so DOS extenders (DOS/4GW etc.) run in VMs;
      today a VM that tries to enter protected mode is stopped.
- [ ] XMS inside VMs; INT 15h AH=87h block moves.
- [ ] Coexisting with EMM386 (VCPI or the GEMMIS interface); today the
      loader refuses to start under a memory manager.
- [ ] Virtual PIT for programs that reprogram the timer.
- [ ] Virtual serial and printer ports; IRQ ownership per device instead of
      sending every device IRQ to the System VM.
- [ ] Per-VM settings (memory size, which devices it may use).
- [ ] Idle detection for VMs that busy-poll the keyboard.

## Kernel

- [ ] Demand paging and a swap file.
- [ ] Double-fault task gate, so a kernel stack guard page becomes safe.
- [ ] Native FAT driver to take disk I/O off DOS gradually (as 32-bit File
      Access did in Windows for Workgroups 3.11).
- [ ] A kernel debugger on COM1 (breakpoints, memory dumps), WDEB386-style.

## Testing and tooling

- [ ] `make qemu-vga` failed once (1 of 10 checks, which one wasn't
      recorded) in about 16 runs; it hasn't reproduced since. Make the test
      report details on failure and loop it to find out.
- [ ] Run on real 386/486 hardware (or 86Box/PCem) to catch emulator-only
      assumptions such as SeaBIOS's RAM use at E000.
- [ ] A Bochs configuration for debugging triple faults.
- [ ] One `make check` target that runs every suite and summarizes.
- [ ] DOSBox-X: its built-in DOS and COMMAND.COM can't run two interactive
      sessions at once; keep real-DOS tests on QEMU + FreeDOS.
