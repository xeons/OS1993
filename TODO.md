# OS1993 TODO

Known gaps and next steps, roughly in the order they'd be tackled. See
README.md for what already works.

## GUI and UI library

- [ ] Menus: owner-drawn
      items, bitmaps in menus, `WM_MENUSELECT` for status-bar help,
      `InsertMenu`/`DeleteMenu`/`ModifyMenu`, a menu bar that wraps to a
      second line when the window is narrow.
- [ ] More controls: combo box (drop-down list), tab control, spin box,
      multi-line edit with scrolling, tree view, tooltips.
- [ ] Standard dialogs: file open/save, colour picker. Message boxes could
      use a system-modal flag and a beep (no sound driver yet).
- [ ] Edit controls: text selection, clipboard (cut/copy/paste), horizontal
      scrolling done properly.
- [ ] Mouse cursor shapes: resize arrows over borders, I-beam over edits,
      wait cursor.
- [ ] Keyboard: Alt+Esc,
      arrow keys between radio buttons in a group.
- [ ] Proportional font for window text (the VGA 8x16 font is all there is).
- [ ] Shell: wallpaper patterns, icon dragging, a Programs list read from a
      file (needs file I/O), large icons in the Start menu with the
      vertical banner, taskbar button right-click (the window's system
      menu), tooltips for tray icons, a task list (Ctrl+Alt+Del) that can
      end a hung program (SYS_KILL exists).
- [ ] Notepad: Find/Replace, Word Wrap, Print (no printer yet), files over
      32 KB. Calculator scientific view; Paint: cut/copy/paste, zoom, a
      canvas size setting.
- [ ] Multi-line edit: word wrap, more than one undo step, a blinking caret,
      auto-scroll while drag-selecting outside the control.
- [ ] Clipboard: text only (16 KB); bitmaps for Paint.
- [ ] File dialogs: a drive list, long file names (FreeDOS has no LFN
      here), a file type list.
- [ ] Layout: controls don't reflow when a window is resized (programs can
      do it themselves now on `WM_SIZE` with `MoveWindow`).
- [ ] Dialog templates (`CreateDialog` from a table) and modal loops.
- [ ] Owner-drawn controls (`WM_DRAWITEM`), `WM_CTLCOLOR*`, `WM_NCHITTEST`.
- [ ] Repainting: the dirty region is a single bounding rectangle; a region
      list would repaint less when far-apart things change.

## Ring 3 and DLLs

- [ ] DLL code shared between processes (every process loads its own copy
      today); DLL data per process (copy-on-write or per-process data pages).
- [ ] Several threads per process; per-thread message queues.
- [ ] The window manager lock is held from a process's first UI call until
      it waits, like Win16Mutex: a program that computes for a long time
      inside a message handler freezes the GUI. Finer-grained locking.
- [ ] A process killed while holding the window manager lock can leave the
      shared arena inconsistent (the lock is released, the data isn't
      repaired).
- [ ] DLL initialization / termination entry points (DllMain-style).
- [ ] Import by ordinal; versioning; a loader error shown in the GUI
      instead of only the debug log.
- [ ] File I/O goes through one request queue to the console thread, one
      operation at a time: a native FAT driver (or several DOS calls in
      flight) would let processes do I/O in parallel.
- [ ] All processes' files share the System VM's PSP handle table (raised
      to 40 with INT 21h 67h); FILES= in CONFIG.SYS bounds them.
- [ ] The heap only grows (SYS_SBRK); freed memory is reused but never
      returned to the system until the process ends.
- [ ] IPC: done -- per-process queues, cross-process SendMessage and
      WM_COPYDATA, wait/wake and kernel locks. Still to do: named shared
      memory sections, named events, pipes.

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
