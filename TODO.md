# OS1993 TODO

Known gaps and next steps, roughly in the order they'd be tackled. See
README.md for what already works.

## GUI and UI library

- [ ] Menus: owner-drawn
      items, bitmaps in menus, `WM_MENUSELECT` for status-bar help,
      `InsertMenu`/`DeleteMenu`/`ModifyMenu`, a menu bar that wraps to a
      second line when the window is narrow.
- [ ] More controls: tree view, list view, tooltips, status bar, toolbar.
      Combo boxes, the tab control and up-downs exist; missing there:
      `CBS_SIMPLE`, item data (`CB_SETITEMDATA`), `CBN_SELENDOK`/`CANCEL`,
      autocomplete in the editable combo; a non-blocking
      `CB_SHOWDROPDOWN` (it returns when the list closes); tab rows that
      wrap or scroll when the tabs don't fit, icons in tabs, Ctrl+Tab;
      `UDN_DELTAPOS`, acceleration while an up-down is held, thousands
      separators. A real `WM_NOTIFY` would let tabs notify as in Win32.
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
      menu), tooltips for tray icons. Close Program could mark programs
      "Not responding" (queue untouched for seconds) and try WM_CLOSE
      before ending one.
- [ ] Notepad: Print (no printer yet), files over 32 KB, a status bar. Calculator scientific view; Paint: cut/copy/paste, zoom, a
      canvas size setting.
- [ ] Multi-line edit: more than one undo step, a blinking caret, End on a
      wrapped row (it stops before the row's last character),
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

- [ ] Shared code: images are kept for the whole session (at most 16) and
      freed when it ends, not when their last user does; data pages are
      copied whole at start, not copy-on-write; a DLL changed on disk during
      a session isn't noticed.
- [ ] Threads: done (16 per process). Only the first may use the window
      system (UI.DLL's lock and queues are per process); per-thread message
      queues would lift that. No SuspendThread of a running thread, no
      named objects shared between processes, WaitForMultipleObjects with
      wait_all takes the objects one at a time rather than atomically.
- [ ] Virtual memory: no demand paging (committing allocates at once), no
      PAGE_NOACCESS or PAGE_GUARD on committed pages, no paging to disk.
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
- [ ] C runtime: no floating point (no FPU on a 486SX; a soft-float
      library would be needed), no locales or wide characters, no
      environment variables; text files aren't translated (CR LF).
- [ ] Heap checks catch overruns and double frees when a block is freed,
      not when the bytes are written; a freed block reused by a new
      allocation can't be told from it.
- [ ] IPC: done -- per-process queues, cross-process SendMessage and
      WM_COPYDATA, wait/wake and kernel locks. Still to do: named shared
      memory sections, named events, pipes.

## Graphics

- [x] Display modes up to 1280x1024 in 8/16/32 bits on the Bochs/QEMU VBE
      adapter (kernel/display.c), Display Properties, OS1993.INI.
- [ ] A generic VBE BIOS backend for other cards: mode set with INT 10h
      4F02h on the console thread (V86), using the BIOS's linear modes
      (vbe.c already lists them); banked-only cards would need a bank
      switch per 64 KB in gfx_present.
- [ ] 15- and 24-bit modes (VBE's 5:5:5 and packed 3-byte pixels).
- [ ] Hardware cursor / blits where the adapter has them.
- [ ] Programs that draw only in the theme's 16 colours: a true-colour
      bitmap API (DIBs) for Paint and images.
- [ ] VDD for DOS sessions that switch to graphics modes (currently text
      mode only).
- [ ] Trap CRTC ports for background DOS sessions (a background session's
      BIOS can still move the real cursor).
- [ ] DOS sessions in a window on the desktop (render the VM's text screen).
- [ ] Monochrome adapters get no graphics desktop.

## Network

- [ ] Only the NE2000 at 300h/IRQ 9; no other cards (3C509, WD8003 ...),
      no settings in OS1993.INI (port, IRQ, a static address instead of
      DHCP).
- [ ] IP: no fragments, no options, one interface; no IPv6; no routing
      table beyond the gateway; no multicast.
- [ ] TCP: no out-of-order queue (the peer resends), no SACK, no window
      scaling or timestamps, no fast retransmit, no keepalives, no Nagle or
      delayed ACKs; 8 KB buffers; 32 sockets system-wide.
- [ ] DNS: A records only, no cache, no reverse lookups; no hosts file.
- [ ] PING's times are to the 55 ms timer (a high-resolution clock from the
      PIT, read without disturbing DOS VMs that use it, would fix that).
- [ ] DOS sessions have no network: a virtual packet driver (INT 60h,
      sharing the card with the stack) or a Winsock VxD for DOS programs.
- [ ] Netstat, tracert (TTL), a telnet client, an FTP client.

## File systems

The VMM has its own IDE driver, FAT12/16/32, exFAT and ext2 (README: Disks
and file systems); DOS reaches their volumes through the redirector.

- [x] Native disk access in the VMM: an IDE/ATA driver, MBR and extended
      partitions, superfloppies, a write-behind block cache.
- [x] INT 13h done by the VMM for DOS (32-bit disk access), so every IDE
      channel without an ATAPI device is the VMM's, the boot disk's too.
      Interrupt-driven transfers. LBA48.
- [x] DOS sees the volumes as drive letters through a network redirector
      (INT 2Fh AH=11h); volumes DOS had mounted keep their letters and go
      back to DOS when OS1993 ends.
- [x] Long names for DOS programs (INT 21h AX=71xxh); share modes and
      byte-range locks enforced, between DOS and OS1993 programs; searches
      kept by the VMM.
- [x] FAT12, FAT16 and FAT32 natively (FSInfo, VFAT long names, NT case
      bits, the clean-unmount bit, a free-cluster bitmap).
- [x] A volume ops table (`kernel/fs/fsimpl.h`), so file systems plug in beside FAT.
- [x] exFAT (allocation bitmap, up-case table, checksummed entry sets,
      contiguous files with FAT chains when they must, files over 4 GB,
      sectors to 4 KB, VolumeDirty and PercentInUse, 10 ms and UTC offset
      fields, numbered DOS aliases).
- [x] ext2 (block groups, indirect blocks, blocks to 64 KB, files over
      4 GB, UTF-8 names, permissions as the read-only attribute, hard and
      symbolic links, UTC times through the time zone, numbered DOS
      aliases).
- [x] 64-bit file sizes and positions through the API (SetFilePointerEx,
      GetFileSizeEx, size_high); the time zone (SetTimeZoneInformation,
      OS1993.INI).
- [x] ext3: journal replay (JBD and JBD2's layout: descriptor, commit and
      revoke blocks, escaped blocks, a wrapping log), so a volume that
      wasn't unmounted cleanly is recovered and mounted.
- [x] ext3: journalled writes, ordered mode (transactions shared by
      calls, revoke records, checkpoints, needs_recovery while mounted);
      journal checksums (v2/v3 and the old CRC32) checked and written;
      external journal devices; data=journal.
- [x] ext4, read-only: extents, 64-bit descriptors, flex_bg, meta_bg,
      hashed directories, metadata_csum checked.
- [ ] ext4 writing (extent trees, checksums everywhere); inline data;
      async commit journals.
- [x] btrfs, read-only: chunk, root and file system trees, subvolumes,
      zlib/LZO/zstd, node and data checksums (CRC32C).
- [ ] btrfs: several devices and RAID profiles; xxhash/SHA-256/BLAKE2
      checksums (read past now); writing.
- [x] 64-bit file sizes and positions through the API (SetFilePointerEx,
      GetFileSizeEx, size_high); the time zone and daylight saving
      (SetTimeZoneInformation, OS1993.INI).
- [x] Long names on DOS's own boot drive for OS1993 and DOS programs (the
      VMM takes C: over unless DOS has a file open on it for writing).
- Not planned: TexFAT (exFAT with two FATs; only Windows CE uses it, and
  desktop Windows doesn't mount it); ATAPI and 4 KB-sector drives (none
  speak ATA PIO with 512-byte sectors); Unix owners, permissions and
  extended attributes on ext2 (no Win32-style call would show them);
  filling lost+found (e2fsck's job); names outside Latin-1 (the system's
  character set: such characters show as '_', and the alias still opens
  the file).

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
