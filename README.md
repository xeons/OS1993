# OS1993

A hobby operating environment written as if it were 1993: a 32-bit,
paged, protected-mode **Virtual Machine Manager** that starts from the DOS
prompt, in the spirit of Windows 3.1's 386 Enhanced mode. Built with modern
tools; the output runs on a real (or emulated) 486 with DOS and HIMEM.SYS.

```
C:\> OS1993
```

## Building and running

Requirements: `nasm`, `i686-elf-gcc` / `i686-elf-binutils`, `dosbox-x`,
`qemu-system-i386`, `mtools`, `curl`. Bochs and Open Watcom (`/opt/watcom`)
are for later stages.

| Command          | What it does                                                  |
|------------------|---------------------------------------------------------------|
| `make`           | builds `build/OS1993.COM` (loader), `build/VMM.BIN` (kernel), the ring-3 DLLs and programs (`*.DLL`, `*.BIN`) |
| `make run`       | boots it in DOSBox-X: 486, 16 MB, XMS, no EMS                 |
| `make test`      | headless round trip DOS → VMM → DOS, checked automatically    |
| `make check486`  | fails if any binary contains a post-486 or FPU instruction    |
| `make qemu`      | boots real FreeDOS 1.3 in QEMU (486, 16 MB, ISA PC with the ISA VGA and its VBE registers); `OS1993` is in `C:\OS1993` |
| `make qemu-test` | headless FreeDOS run of the same checks as `make test`        |
| `make qemu-emm386` | FreeDOS with JEMM386: the loader must refuse to start       |
| `make qemu-sessions` | types into two concurrent DOS sessions via QEMU's monitor |
| `make qemu-absmouse` | absolute pointer through QEMU's VMware port: the cursor lands on the exact pixel, and clicks hit their control |
| `make qemu-crt` | the C runtime: CRTTEST.BIN (CRT.DLL) and CRTSTAT.BIN (libcrt.a) pass its checks; a stack overrun, a heap overrun, a double free, a fortified memcpy, a broken Annex K constraint, `%n` and a failed assert each end the program with the reason logged; every process has its own canary |
| `make qemu-vesa` | the VBE display driver: Display Properties switches to 1024×768 high colour, 800×600 in 256 colours and true colour, No and the 15-second timeout go back, a DOS session and back keeps the mode, OS1993.INI restores it next session, and 640×480×16 is VGA again |
| `make qemu-tasklist` | Ctrl+Alt+Del: Close Program ends a program, works from a DOS session, and the VMM's screen ends a program that froze the GUI |
| `make qemu-notepad` | Notepad: typing, Save As with CR LF, the clipboard between two processes, Open, select/delete/undo, unsaved-changes questions, Find/Find Next/whole words, Replace All and its undo, Word Wrap, the file's bytes on disk |
| `make qemu-files` | file access: Paint saves, opens and replaces a BMP through the file dialogs, a second process reads it, then the file is checked on the disk image |
| `make qemu-shell` | the shell and its programs as processes: Start menu by mouse and Ctrl+Esc, desktop icons, Calculator, Paint, Clock, taskbar buttons, Run, Shut Down |
| `make qemu-msgbox` | keyboard accelerators per window, message boxes: modality, default button, Tab, Yes/No letters, Esc, Alt+F4 |
| `make qemu-menu` | menu bar, system menu and right-click menus by mouse and keyboard (mnemonics, F10, Alt+Space, keyboard Move) |
| `make qemu-controls` | combo boxes (drop-down list with a scroll bar, editable, a list that opens upwards), the tab control and up-downs (buddy edit, auto-repeat, wrap) by mouse and keyboard |
| `make qemu-wm`   | drives the window manager through QEMU's mouse and keyboard: every control, activate/move/resize/minimize/maximize/close, palette after a DOS session |
| `make qemu-vga`  | the VDD: the GUI's desktop must come back pixel-identical after a text DOS session (except its animated progress bar) |

There are two test machines because they catch different bugs:

- **DOSBox-X** is fast and emulates a 486 (and a 386, for the test that the
  loader refuses one), but its DOS and COMMAND.COM are emulator built-ins
  written in C++.
- **QEMU + FreeDOS** runs a real DOS kernel, FreeCOM, HIMEMX and JEMM386.
  `tools/mkfreedos.sh` downloads the official FreeDOS 1.3 Lite image once
  into `.cache/freedos` and checks its SHA-256. It then writes its own
  `FDCONFIG.SYS`/`FDAUTO.BAT` and copies OS1993 onto the image with mtools.
  The `isa-debug-exit` device lets an automated run power off QEMU. It runs
  as a 486.

Kernel debug output goes to COM1; under DOSBox-X it lands in
`build/SERIAL.LOG`.

## How it boots

1. **`OS1993.COM`** (real mode, `loader/os1993.asm`) checks for a 486 (one
   that can set EFLAGS.AC; a 386 is refused with a message) that is in real mode, not V86 mode under EMM386/QEMM. It then allocates the largest
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

## Ring 3: processes

The GUI is 32-bit flat protected-mode code in ring 3, as Windows 3.x ran
its protected-mode code in the System VM (`kernel/user.c`, `user/`). A GUI
session is a set of **processes**, each with one thread and its own
address space, like Win95's 32-bit side.

- **Segments.** Ring-3 flat code and data descriptors (selectors 23h/2Bh).
  Isolation comes from paging: user pages have the U bit, kernel pages don't.
- **Address spaces.** There is still one page directory. A process owns
  the user page-directory entries (below 2 GB), which are swapped in when
  one of its threads is scheduled, the way VMs swap PDE 0, so the kernel
  half is identical everywhere by construction. A program is linked at
  `0040_0000`; DLLs at their own bases above `1000_0000`; the stack ends at
  `7FFF_0000`, where an info block (pid, arguments, the 8×16 font) sits.
- **The shared region** at `6000_0000` is one page table installed in every
  process: the VGA window, then a 512 KB **shared arena** where UI.DLL keeps
  the window manager's state, as Win95's USER kept its data in the shared
  arena.
- **Loading.** The VMM console thread loads programs through DOS (the
  System VM's own file system): `SYS_EXEC` from a process queues a request
  and waits.
- **Shared code.** A module's read-only part (header, code, constants,
  exports; the linker ends it on a page boundary, given in the header) is
  loaded once per session and mapped into every process that uses it,
  read-only -- a program writing into code is killed. Its writable part
  (data, import slots, bss) is copied for each process from a pristine
  copy taken after its imports were linked and before any of its code
  ran; with every DLL at a fixed address the linked slots are the same
  everywhere. Programs share too: a second Notepad maps the first one's
  code. The VMM logs how much each new process shares.
- **System calls** (`INT 30h`, DPL-3 gate): exit, debug print, ticks,
  yield, wait/wake (`SYS_WAIT` sleeps until another process wakes it,
  input arrives, or a timeout), exec, kill, process liveness, kernel locks
  (handed to the next waiter, released if their owner dies), the RTC time,
  input events, start a DOS session. User pointers are checked.
- **Files** (`kernel/files.c`). Programs use DOS's files: the console
  thread makes the INT 21h calls for them (the process waits), copying the
  data through the V86 transfer buffer. Each process has its own handle
  table (closed when it ends, however it ends), its own searches, and its
  own current directory, at first the OS1993 directory. KERNEL.DLL wraps
  it Win32-style: `CreateFile`, `ReadFile`, `WriteFile`, `SetFilePointer`,
  `GetFileSize`, `CloseHandle`, `DeleteFile`, `MoveFile`,
  `Create`/`RemoveDirectory`, `GetFileAttributes`, `Get`/`SetCurrentDirectory`,
  `FindFirstFile`/`FindNextFile`/`FindClose`, `GetLastError`.
- **Memory.** `SYS_SBRK` grows a per-process heap at `2000_0000`;
  KERNEL.DLL's `LocalAlloc`/`LocalReAlloc`/`LocalFree` manage it.
- **Ctrl+Alt+Del** never reaches a DOS session (its BIOS would reboot).
  With a GUI it comes to the front and the shell shows **Close Program**
  (the running programs by window title; End Task, Shut Down, Cancel).
  If the shell doesn't answer within two seconds -- a program holding the
  window system's lock is stuck -- the VMM shows its own text-mode screen
  over the desktop, as Windows 3.1 did: the programs, the one holding the
  lock marked and chosen; Enter ends it and the desktop comes back, Esc
  waits, Ctrl+Alt+Del again ends the whole session. `HANG.BIN` (start it
  from Run) hangs on purpose to try it.
- **Faults and kills.** An exception in ring 3 ends that process with a log
  line; the others carry on, and UI.DLL drops its windows. A killed process
  ends on its way back to ring 3.
- **Sessions.** **G** in the VMM console starts the shell (`SHELL.BIN`), the
  session's first process; when it ends, every other process goes too. In
  test mode `DEMO.BIN` is run twice as a self-test: executing `CLI` must be
  killed with #GP, and writing kernel memory must be killed with #PF.

## Shared libraries (DLLs)

Ring-3 code is built as **MOD3 modules** (`kernel/include/user.h`):
programs and DLLs share one format, a header with base address, entry
point, sizes, and export and import tables. Each DLL is linked at its own
fixed base, so nothing is ever relocated, as with OS/2's and Windows'
system DLLs.

- **`.DEF` files and import libraries**, as in Windows. `tools/mkdef.py`
  reads a `.DEF` (library name, base, exported functions) and generates the
  DLL's export table, plus an import library for callers: one
  `JMP [slot]` thunk per function, so calls are ordinary C calls.
- **The loader** (`kernel/user.c`) loads a program, then the DLLs it
  imports and theirs, from the program's directory. It fills every slot by
  name and refuses to start with a clear message if an export is missing.
  Everything is unmapped, page tables too, when the process ends; shared
  images stay until the session ends.
- **`HOOK fn`** in a `.DEF` makes every export call `fn()` first: UI.DLL
  takes its lock that way.

| Module    | Base        | Contents                                         |
|-----------|-------------|--------------------------------------------------|
| `KERNEL.DLL` | `1000_0000` | the userland kernel library: wraps the INT 30h system calls (`user/krnl/`) |
| `GFX.DLL` | `1010_0000` | the VGA display driver (`user/gfx/`)             |
| `UI.DLL`  | `1020_0000` | windows, messages, painting, controls (`user/ui/`); imports KERNEL, GFX |
| `CRT.DLL` | `1040_0000` | the C runtime (`user/crt/`); imports KERNEL. Also built as `libcrt.a` |
| `SHELL.DLL` | `1030_0000` | the shell API: `ShellExecute`, `ShellAbout`, `Shell_NotifyIcon`, `GetOpenFileName`/`GetSaveFileName`, `FindText`/`ReplaceText`, stock icons (`user/shell/`) |
| `*.BIN`   | `0040_0000` | programs (`user/apps/`): SHELL, CALC, PAINT, CLOCK, NOTEPAD, DEMO, CRTTEST (and CRTSTAT) |

Programs include one header, `user/lib/os1993.h` (the `windows.h` of
OS1993), and call KERNEL.DLL (`CreateProcess`, `GetTickCount`, `Sleep`,
`GetLocalTime`, `WaitForWake`/`WakeProcess`, `AcquireLock`, `DebugLog`,
...) instead of issuing system calls themselves.

## The C runtime

`user/crt/` is a C library for ring 3, built from one set of sources two
ways: **CRT.DLL**, which programs import and share (its code is mapped
once, like any DLL's), and **libcrt.a**, linked into a module of its own.
The system DLLs link the parts they use statically (KERNEL.DLL can't import
a DLL that imports it); programs use CRT.DLL; `CRTSTAT.BIN` is the test
program linked statically. Headers are in `user/crt/include` (`string.h`,
`stdlib.h`, `stdio.h`, `ctype.h`, `errno.h`, `assert.h`, `setjmp.h`,
`time.h`; the compiler supplies `stdint.h`, `stddef.h`, `stdarg.h`,
`limits.h`). `crt0` calls `main(argc, argv)` with the command line split
at spaces (quotes keep one together) and `exit`s with its result.

- **stdio.** Buffered `FILE`s over KERNEL.DLL's files; `stdout` and
  `stderr` are the debug log, a line at a time; `stdin` is empty. Files
  are binary (no CR LF translation). `printf` has every integer conversion,
  flag and length, but no floating point: a 486SX has no FPU.
- **stdlib.** `malloc` over the process heap, `strtol` and friends
  (64-bit without libgcc: overflow is checked in 32-bit halves), `qsort`,
  `bsearch`, `atexit`, `rand`, and `rand_s`/`getrandom_bytes` from the
  kernel's generator. `time.h` reads the RTC (local time is UTC).

It is hardened, by default and in every ring-3 module:

- **Stack canaries.** Everything is compiled with
  `-fstack-protector-strong`. Each module has its own `__stack_chk_guard`
  (data can't be imported), and the module header points at it: the loader
  stores a fresh random value there for each process, its low byte zero so
  a string copy can't reproduce it.
- **Random numbers.** `kernel/random.c`: a ChaCha20 generator whose key
  every request first mixes with an entropy pool (interrupt timing, the
  RTC and the PIT at boot) and then replaces ("fast key erasure").
  `SYS_RANDOM` / `GetRandomBytes`.
- **Fail fast.** A process that finds itself corrupted ends at once with
  `SYS_FAIL` (as Win32's `__fastfail`, a trap of its own rather than a call
  into a possibly corrupted DLL). The kernel logs the reason and the
  process ends with code -1033 (`STATUS_STACK_BUFFER_OVERRUN`'s 409h).
- **The heap.** Each block has a header with a cookie (its size and
  address mixed with a random secret) and a guard word after it. `free`
  and `realloc` check both: an overrun, a double free or a pointer malloc
  never returned is caught. Freed bytes are overwritten. `calloc` and
  `reallocarray` check their multiplication.
- **Bounds-checked functions** (C11 Annex K): `memcpy_s`, `strcpy_s`,
  `strcat_s`, `sprintf_s`, `strtok_s`, `fopen_s`, `qsort_s` and the rest.
  A broken constraint empties the destination and calls the handler;
  `abort_handler_s` is the default (`set_constraint_handler_s` changes
  it). Also `strlcpy`/`strlcat`, `explicit_bzero` and `timingsafe_memcmp`.
- **`_FORTIFY_SOURCE=2`.** Copies into objects of known size (`memcpy`,
  `strcpy`, `sprintf` ...) are checked at compile time where possible and
  otherwise by `__memcpy_chk` and friends.
- **No `%n`** in `printf`: it writes through a pointer, the classic
  format-string hole. `setjmp` stores its stack and code pointers XORed
  with the canary. `qsort` is a heapsort: no recursion, n log n whatever
  the input.

## The window system across processes

UI.DLL's state (windows, classes, z-order, queues, timers, menus, input
and cursor state, the off-screen buffer) lives in the shared arena, so an
`HWND` means the same window in every process. Each window belongs to the
process that created it, and only that process runs its window procedure.

- **One lock**, like Win95's Win16Mutex: every UI.DLL export takes it on
  entry; a process lets go only when it waits (in `GetMessage`, a modal
  loop, `Sleep`, `CreateProcess`, or a cross-process `SendMessage`).
- **A queue per process.** `PostMessage` goes to the window owner's queue
  and wakes it. `SendMessage` to another process's window waits while that
  process runs the window procedure, answering messages sent to it
  meanwhile so two processes can't deadlock. **`WM_COPYDATA`** copies its
  data across: the IPC primitive (the tray icons use it).
- **Input** is turned into messages by whichever process pumps first. A
  click on a frame becomes `WM_NCLBUTTONDOWN` for the window's owner, with
  raw input reserved for it while its `DefWindowProc` runs the move, size,
  box or menu loop.
- **Painting.** Each top-level window has its own update area; its owner
  paints it clipped to its visible region (minus the windows above), so
  processes paint independently, into the shared off-screen buffer.
- **Process death.** Windows, classes, queues and menus of a process that
  ended are removed by the next process that pumps.
- **Shell support:** `RegisterShellHookWindow` (`WM_SHELLHOOK` on window
  creation, activation, title and state changes), `SetShellWindow` (the
  desktop stays at the bottom), `WS_EX_TOPMOST`, `WS_EX_TOOLWINDOW`,
  `WS_EX_NOACTIVATE`, `EnumWindows`, `FindWindow`, `SetForegroundWindow`,
  the work area (`SystemParametersInfo`), class icons. Ctrl+Esc anywhere
  goes to the shell. With a shell running, minimized windows leave the
  screen for the taskbar instead of becoming tiles.

## The shell and its programs

- **SHELL.BIN** (`user/apps/shell.c`), in the style of Windows 95: a
  desktop with icons (double-click or Enter opens; right click for Run and
  About); a taskbar with the **Start** button and menu (Programs, Run...,
  About, Shut Down...; also Ctrl+Esc), a button per program window (click
  to switch, again to minimize), and the notification area with tray icons
  and a clock. A Run dialog starts any program by name. Shut Down asks,
  then ends the session.
- **CALC.BIN**: the standard calculator: the four operations, square root,
  percent, reciprocal, sign, memory, by mouse or keyboard. Without an FPU
  or libgcc, it computes in 64-bit fixed point (six decimals, twelve
  digits) with its own division.
- **PAINT.BIN**: a 400×280 canvas, pencil, brush, eraser, flood fill,
  line, rectangle, filled rectangle and ellipse (rubber-banded), a 16-colour
  palette (left button foreground, right button background). Open, Save and
  Save As (Ctrl+O, Ctrl+S) through the file dialogs: it writes 16-colour
  BMP files and opens 16- and 256-colour ones (colours mapped to the
  nearest of the 16); it asks before discarding changes.
- **The file dialogs** (SHELL.DLL): the folder, a file name field (its
  text selected, so typing replaces it), and a list of subfolders and
  matching files; double-click or type a folder name to move, a pattern
  to filter. Save As asks before replacing a file; Open checks it exists.
- **NOTEPAD.BIN**: a text editor on the multi-line edit control: File
  (New, Open, Save, Save As; plain text with CR LF line ends, up to 32 KB),
  Edit (Undo, Cut, Copy, Paste, Delete, Select All, Time/Date, Word Wrap),
  Search (Find, Find Next F3, Replace; match case, whole words, up/down,
  Replace All as one undoable edit), and the usual question about unsaved
  changes. `NOTEPAD.BIN name.txt` opens a file.
- **CLOCK.BIN**: analog (scales with the window; sine table) or digital
  (seven segments and the date), seconds on or off, from the RTC.
- **DEMO.BIN**: the Control Demo (Programs, Controls and About windows),
  with a tray icon through `Shell_NotifyIcon`.

Each module links its own copy of the tiny C runtime (`user/lib/`).

## The window manager and controls

The GUI (`user/`) is 32-bit ring-3 code in the System VM. Its look is
inspired by the Mac OS 8 "Platinum" appearance (drawn from scratch: light
grey bevels, pinstriped active title bars, rounded push buttons, inset
fields with an accent focus ring, lavender selection), and it has the
Windows 95 set of window controls and widgets.

- **Palette.** `gfx_set_palette` loads 16 custom colours (an identity
  attribute map plus DAC): eight greys for the Platinum ramp, accents, a
  desktop colour. The VDD captures the GUI's live palette when a DOS
  session takes the screen.
- **A Win32-style window system** (`user/ui/wnd.c`). Windows are `HWND`s
  of a registered class (`RegisterClass`) whose window procedure handles
  their messages; `DefWindowProc` supplies the defaults. One message queue:
  `GetMessage` / `TranslateMessage` / `DispatchMessage`, `SendMessage`,
  `PostMessage`, timers (`SetTimer` -> `WM_TIMER`), mouse capture, focus
  and activation messages, `WM_PAINT` with `BeginPaint`/`EndPaint` and a
  clipped `HDC`. Controls are child windows, so anything can be
  **subclassed** (`SetWindowLong(GWL_WNDPROC)` + `CallWindowProc`) or carry
  data (`SetProp`/`GetProp`, `GWL_USERDATA`, class extra bytes). Message
  numbers and control messages (`BM_`, `LB_`, `SBM_`, `TBM_`, `PBM_`, `EN_`,
  `BN_`, `WM_HSCROLL`...) follow Windows. `IsDialogMessage` gives any window
  Tab/Shift+Tab, Enter (default button) and Esc (`IDCANCEL`).
- **Frames and painting** (`user/ui/frame.c`, `paint.c`, `input.c`).
  Overlapping windows in a z-order, painted
  back to front into a dirty rectangle with clipping, **double-buffered**.
  Painting goes into an off-screen copy of the screen (four planes in
  memory), and only the finished rectangle is copied to video memory, so no
  half-drawn layer is ever visible. The cursor and drag outlines are XOR
  images on the real screen; the cursor is hidden only when a presented
  rectangle touches it. Each title bar has a
  system box on the left and minimize, maximize/restore and close on the
  right. Inactive windows are flat and show no boxes. Double-clicking the
  title maximizes; double-clicking the system box closes; Alt+F4 closes.
  The boxes send `WM_SYSCOMMAND` (`SC_CLOSE`, `SC_MINIMIZE`, ...) to the
  window procedure, so a program can intercept them.
  Windows resize from their borders, corners and size grip. Moves and
  resizes drag an XOR outline. Minimized windows become title tiles along
  the bottom until the shell gets a taskbar.
- **Menus** (`user/ui/menu.c`). Menu bars (`CreateMenu`, `SetMenu`),
  pop-up menus with submenus, separators, check marks and radio dots,
  greyed items, `&` mnemonics and right-aligned accelerator text
  (`"&Close\tAlt+F4"`), and context menus (`TrackPopupMenu`; a right click
  sends `WM_CONTEXTMENU` up the parent chain). As in Windows, a menu is a
  modal loop with each open pop-up a window of its own; the chosen item
  arrives as `WM_COMMAND`. Mouse: click and click, or press-drag-release;
  hovering opens submenus, which flip left at the screen edge. Keys: Alt
  or F10 selects the menu bar, Alt+letter opens a menu, arrows, Enter, Esc
  and mnemonics. Every window has a **system menu** (`GetSystemMenu`):
  Restore, Move, Size, Minimize, Maximize, Close, greyed as the window's
  state requires, from the system box or Alt+Space; it sends
  `WM_SYSCOMMAND`. Move and Size from the menu work from the keyboard
  (arrow keys, Enter, Esc), as in Windows.
- **Keyboard accelerators** (`user/ui/accel.c`):
  `CreateAcceleratorTable` and `TranslateAccelerator`, as in Windows: a key
  with Shift/Ctrl/Alt becomes `WM_COMMAND` (`HIWORD(wParam)` = 1), unless
  the command is a greyed item of the window's menu.
- **Message boxes** (`user/ui/msgbox.c`): `MessageBox` with the Windows
  button sets (OK, OK/Cancel, Yes/No, Yes/No/Cancel, Abort/Retry/Ignore,
  Retry/Cancel), `MB_DEFBUTTONn`, and stop / caution / note / question
  icons. Modal to its owner, which is disabled meanwhile (clicking it brings
  the box forward), through a message loop of its own, so other windows
  keep working. Mac-style layout: buttons bottom right, the default (ringed,
  Enter) rightmost. Esc and the close box mean Cancel when there is one;
  Yes/No/Abort/Retry/Ignore answer to their initial letters. Windows can
  own other windows (`CreateWindow` with a parent but no `WS_CHILD`);
  owned windows go when their owner does.
- **Controls**, one class per file: `BUTTON` (push button with
  default-button ring, check box, radio button group, group box), `STATIC`,
  `EDIT` (caret, Home/End/Delete), `LISTBOX` with its own scroll bar,
  `SCROLLBAR` (proportional thumb; the parent moves it, as in Windows),
  `TRACKBAR` and `PROGRESS`. `EDIT` with `ES_MULTILINE` is a text editor
  control: up to 32 KB in its process's heap, tabs, scrolling with
  `WS_VSCROLL`/`WS_HSCROLL` bars (without a horizontal one or
  `ES_AUTOHSCROLL`, words wrap at the width, re-wrapped on resize and on
  `WM_STYLECHANGED`, which `SetWindowLong(GWL_STYLE)` now sends), selection by mouse (double-click: a word)
  and Shift+keys, Ctrl+Home/End and word movement, cut/copy/paste through
  the **clipboard** (shared by every process: `SetClipboardText`,
  `GetClipboardText`), undo of the last run of typing, and the `EM_*`
  messages (`EM_GETSEL`, `EM_REPLACESEL`, `EM_GETMODIFY`, `EM_LINEINDEX`...).
  Single-line edits copy, cut and paste too. Every control draws inside its own rectangle,
  focus and default rings included.
- **Combo boxes, tabs, up-downs** (`user/ui/combo.c`, `tab.c`,
  `updown.c`), with Win32's message numbers. `COMBOBOX` comes as
  `CBS_DROPDOWNLIST` (a pop-up button showing the selection) and
  `CBS_DROPDOWN` (a child `EDIT` and the arrow); its list is a pop-up
  window that never activates, below the box or above it when the work
  area has no room, with a scroll bar past 8 rows, and like a menu it is a
  modal loop: hover, click or Enter chooses, Esc or a click outside
  cancels; Alt+Down/F4 open it, the arrow keys change the selection while
  it is closed. Strings live in the owning process's heap (`CB_ADDSTRING`,
  `CB_GETLBTEXT`, `CB_FINDSTRINGEXACT`..., `CBN_SELCHANGE`, `CBN_DROPDOWN`,
  `CBN_CLOSEUP`, `CBN_EDITCHANGE`). Unlike Windows, the window's height is
  the closed box's. `TABCONTROL` draws Platinum tabs, the selected one
  joined to its page (`TCM_INSERTITEM` with a `TCITEM`, `TCM_ADJUSTRECT`,
  Left/Right); as there is no `WM_NOTIFY`, `TCN_SELCHANGING` and
  `TCN_SELCHANGE` come as `WM_COMMAND`. `UPDOWN` steps a position with
  auto-repeat and reports `WM_VSCROLL SB_THUMBPOSITION`; a buddy edit can
  show it (`UDS_SETBUDDYINT`) and lend it its arrow keys
  (`UDS_ARROWKEYS`).
- **Background behaviour.** When a DOS session takes the screen, the GUI is
  told (`EV_FOCUS`) and stops drawing. Its VGA port accesses are trapped and
  emulated (writes dropped, reads FFh) instead of reaching the hardware
  under the session's text mode, as Windows' VDD did. Back in front, it
  repaints everything.

Demo windows (DEMO.BIN): **Programs** (MS-DOS Prompt, About, Exit, and a
File / Help menu bar; F1, Ctrl+D and Ctrl+Q; Exit asks first), **Controls** (one of each control, and a right-click
menu with a Speed submenu) and **About OS1993**; File > More Controls
opens a fourth, with a tab control over pages of controls, two combo boxes
and up-downs. Esc closes the demo.

## Mouse

`kernel/mouse.c` drives a PS/2 mouse on the 8042's auxiliary port (IRQ 12):

- **Setup.** At boot it enables the aux port and its interrupt, then sends
  "set defaults" and "enable streaming", waiting for each `FAh`
  acknowledgement. No acknowledgement means no mouse, and the VMM carries on.
- **Packets.** 3-byte packets resync on bit 3 of the first byte, use 9-bit
  signed deltas, and drop overflows. The kernel keeps an absolute pointer
  position clamped to the screen (640×480 or the display mode's size), plus the buttons. The keyboard handler
  checks the controller's "aux data" status bit, so a mouse byte arriving
  on IRQ 1 isn't taken for a key.
- **Delivery.** Events go to the ring-3 GUI while it has the screen.
  `SYS_GET_EVENT` returns key and mouse events, and consecutive moves are
  merged. DOS sessions get no mouse yet (that needs a virtual aux device
  for a DOS mouse driver).
- **Exit.** Before returning to DOS, streaming is stopped and the 8042
  command byte restored. Otherwise mouse bytes would jam DOS's keyboard.
- **Absolute positioning under emulators.** A PS/2 mouse only reports
  movement, so a guest pointer drifts away from the host's. If the VMware
  "backdoor" port answers (QEMU, VMware, VirtualBox), the driver switches
  that pointer to absolute mode. The PS/2 interrupt still signals each
  event, but the position is read exactly from the port. Real hardware
  doesn't answer the probe and stays on PS/2. `make qemu` enables QEMU's
  `vmport`; the scripted tests turn it off to drive the relative path, and
  `make qemu-absmouse` covers the absolute one.

The GUI draws an XOR arrow cursor (drawing it twice erases it, so it needs
no save-under), and the demo window can be dragged by its title bar.

## VGA

The VGA driver is split into a kernel VDD and a display driver that runs in
ring 3.

- **`kernel/vga.c`, the VDD.** Owns the VGA hardware. Modes are set by the
  VGA BIOS (INT 10h from the System VM), as Windows did, and the resulting
  register set is captured: misc output, sequencer, CRTC, graphics and
  attribute controllers, DAC palette. Switching the screen from the desktop
  to a text DOS session saves the four 38 KB planes, restores the boot text
  registers and the font in plane 2, and copies in the session's text
  buffer. Switching back restores the planes and registers exactly.
- **`kernel/gfx.c`, the display driver.** Planar mode-12h drawing: fills,
  lines and pixels through the Set/Reset and Bit Mask registers (one write
  updates all four planes), and 8×16 text using the VGA BIOS font from
  INT 10h AX=1130h. It needs only VGA port access and a pointer to
  A000:0000, and is linked into the ring-3 GUI as its `VGA.DRV`.

## Display modes (VBE)

`kernel/display.c` adds higher resolutions and colour depths to VGA's
640×480×16. It is a display driver for one card, as Windows' were: the VBE
adapter that Bochs and QEMU emulate (`-vga std`; on QEMU's ISA machine its
BIOS is `vgabios-isavga.bin`, linked as `build/qemu-rom/vgabios.bin`).

- **Hardware.** The adapter is programmed through its index/data ports
  (1CEh/1CFh) rather than INT 10h 4F02h: that gives a linear framebuffer at
  E0000000h (the ISA BIOS offers banked modes only) and mode switches made
  of port writes, which also work from the keyboard interrupt (switching to
  a DOS session) and the emergency screen. `kernel/vbe.c` still asks the
  VBE BIOS for its version, memory and modes at boot, for the log.
- **Modes.** 640×480, 800×600, 1024×768 and 1280×1024 in 8 bits (palette),
  16 (5:6:5) and 32 (8:8:8), the ones that fit the adapter's memory, plus
  640×480×16 planar (mode 12h), always there.
- **Memory.** The framebuffer (`USER_FB`, mapped uncached) and the back
  buffer in the screen's format (`USER_BACK`, up to 8 MB) are in the shared
  region, whose page tables every process shares, so a switch remaps them
  once for everyone. `SYS_DISPLAY_INFO/MODES/SET` describe and change the
  mode; a switch is allowed to the GUI while it has the screen.
- **Drawing.** GFX.DLL draws linear 8/16/32-bit pixels as well as planar
  ones. Programs still draw in the theme's 16 colours: 8-bit modes put the
  theme in the DAC, 16- and 32-bit ones map each to a pixel value;
  `FillRectRGB` draws any colour (the nearest theme colour where there are
  only 16 or 256).
- **Programs.** `EnumDisplaySettings`, `ChangeDisplaySettings` and
  `GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN)`, as in Win32. A switch
  re-creates every process's view of the screen the next time it takes
  the window system's lock (the mode's generation number has moved), fits
  maximized windows to the new work area, and posts `WM_DISPLAYCHANGE` to
  every top-level window; the shell lays out its desktop and taskbar again.
- **Display Properties** (Start menu, or the desktop's right-click menu)
  offers the sizes and depths, asks "Keep this setting?" for 15 seconds,
  and saves a kept mode in `OS1993.INI`, which the shell applies when it
  starts.
- **DOS sessions.** Switching to one turns the adapter off (in 256 colours
  the DAC is saved first); switching back turns it on and the GUI repaints.

Other cards would need the VBE BIOS's own linear modes (mode set with
INT 10h on the console thread): see TODO.md.

Limitations: DOS sessions must stay in text mode. A DOS program switching
its VM to graphics isn't virtualized yet. Mono adapters get text only.

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
kernel/random.c                          random numbers (ChaCha20 from interrupt timing)
kernel/vga.c gfx.c                       VGA state (VDD), planar drawing
kernel/display.c vbe.c                   display modes: the VBE adapter, the VBE BIOS probe
kernel/user.c                            ring-3 loader, INT 30h system calls
kernel/mouse.c                           PS/2 mouse (IRQ 12)
user/lib/os1993.h                        the public API; crt0, the module header and linker script
user/crt/                                the C runtime: CRT.DLL and libcrt.a; include/ has its headers
user/krnl/                               KERNEL.DLL (system call wrappers)
user/gfx/                                GFX.DLL (drawing: planar VGA, linear 8/16/32 bits)
user/ui/                                 UI.DLL (windows, messages, controls)
user/shell/                              SHELL.DLL (shell API, stock icons)
user/apps/                               SHELL.BIN, CALC.BIN, PAINT.BIN, CLOCK.BIN, DEMO.BIN
kernel/gdt.c idt.c pic.c timer.c keyboard.c console.c serial.c kprintf.c panic.c
kernel/include/          headers; bootinfo.h is shared with the loader
kernel/linker.ld         links at 0x80000000, header first
test/dosbox-x.conf       the emulated 1993 machine
tools/                   check486.sh, check-test.sh
```

## Rules of the house

- **486SX.** The 486's instructions are fine (`INVLPG` flushes single pages,
  CR0.WP makes read-only pages read-only for the kernel too, so it checks
  user buffers it writes into with `user_writable`), but no FPU
  (`-mgeneral-regs-only`), no CPUID (early 486s lack it), nothing Pentium.
  No libgcc, so no 64-bit division. `make check486` enforces this.
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
6. **Graphics and GUI.** Done: the VGA driver and ring-3 support (flat
   user segments, INT 30h system calls, program loader, fault
   isolation), the PS/2 mouse, and the window manager with the Windows 95
   control set in a Platinum-inspired style, double-buffered, plus DLLs
   (KERNEL.DLL, GFX.DLL, UI.DLL, SHELL.DLL) with a Win32-style
   message-based window API, menus, accelerators and message boxes, an
   absolute mouse under emulators, processes with their own address spaces
   sharing one window system, and a Windows 95-style shell with
   Calculator, Paint and Clock, file access for programs with the Open and
   Save As dialogs, a process heap, the clipboard, a multi-line edit control
   and Notepad (word wrap, Find/Replace), a Ctrl+Alt+Del task list, combo
   boxes, tabs and up-downs, and code shared between processes, and higher resolutions and colour depths
   (the VBE display driver, Display Properties), and a hardened C runtime
   (CRT.DLL and libcrt.a). Next: threads and the process, memory and file
   APIs; then a network driver, TCP/IP, sockets and PING.
7. **Win16-style API layer.** 16-bit protected-mode apps in ring 3 with LDTs,
   built with Open Watcom.
8. **Coexisting with EMM386** through VCPI/GEMMIS, and acting as a DPMI host.
