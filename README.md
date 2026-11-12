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
`qemu-system-i386`, `mtools`, `curl`. Open Watcom (`/opt/watcom`) builds
the DOS/4GW test program for `make qemu-dpmi` and provides DOS4GW.EXE and
DOS32A.EXE; Bochs is for later.

| Command          | What it does                                                  |
|------------------|---------------------------------------------------------------|
| `make`           | builds `build/OS1993.COM` (loader), `build/VMM.BIN` (kernel), the ring-3 DLLs and programs (`*.DLL`, `*.BIN`) |
| `make run`       | boots it in DOSBox-X: 486, 16 MB, XMS, no EMS                 |
| `make test`      | headless round trip DOS → VMM → DOS, checked automatically    |
| `make check486`  | fails if any binary contains a post-486 or FPU instruction    |
| `make qemu`      | boots real FreeDOS 1.3 in QEMU (486, 16 MB, ISA PC with the ISA VGA and its VBE registers); `OS1993` is in `C:\OS1993`. `build/data.img` (made once by `tools/mkdata.sh`, then kept) is on the secondary IDE channel: D: (FAT32), E: (FAT16), F: (exFAT), G: (ext2), H: (ext2, 8 KB blocks), I: (ext3) |
| `make qemu-test` | headless FreeDOS run of the same checks as `make test`        |
| `make qemu-emm386` | FreeDOS with JEMM386 loaded: the loader starts OS1993 through VCPI, the self-tests run (two DOS sessions included) with the memory manager's HLT-stub vectors moved to the BIOS meanwhile, and OS1993 goes back to V86 mode under JEMM386 |
| `make qemu-sessions` | types into two concurrent DOS sessions via QEMU's monitor |
| `make qemu-absmouse` | absolute pointer through QEMU's VMware port: the cursor lands on the exact pixel, and clicks hit their control |
| `make qemu-crt` | the C runtime: CRTTEST.BIN (CRT.DLL) and CRTSTAT.BIN (libcrt.a) pass its checks; a stack overrun, a heap overrun, a double free, a fortified memcpy, a broken Annex K constraint, `%n` and a failed assert each end the program with the reason logged; every process has its own canary |
| `make qemu-kdbg` | the kernel debugger on COM1 (`OS1993 /D`), driven through a socket: Ctrl+C breaks in, R, .T, .VM, .M, DB, .PT, an execution breakpoint hit by pressing N, K, a single step, a write watch hit in V86 mode by a key in a DOS session; then a kernel thread overflows its stack, the double fault arrives in its own task and both the debugger and the blue screen name the thread |
| `make qemu-dosvm` | DOS sessions' virtual devices through DOSVM.COM (`test/dos/dosvm.asm`): a VM that sets the timer to 1 kHz gets 1000 IRQ 0s a second (the real timer follows, then slows down again), a VM polling the keyboard is idled, COM2 and LPT1 go to the first VM that touches them (COM2's interrupt reaches it) and a second VM is refused, COM1 stays the VMM's, the INT 33h mouse (an event handler sees a click at the right place), XMS (allocate, move, lock, an INT 15h AH=87h move into the locked block) and INT 15h; then OS1993.INI's `[DOS]` settings for a new session: Memory=400, no XMS, no mouse, LPT1 only |
| `make qemu-dpmi` | the DPMI host: DOSVM D's own checks (the mode switch, descriptors, 256 KB through a 32-bit offset, 0300h, vectors, the end through INT 21h), then DPMITEST.EXE (`test/dos/dpmitest.c`, built with Open Watcom) under DOS/4GW and again under DOS32A: malloc of 2 MB, files, the text screen, all in a DOS session that carries on |
| `make qemu-fat` | the VMM's own disks, run twice: with the data disk on the secondary channel and beside the boot disk (whose INT 13h the VMM then does); FAT32, FAT16, exFAT, two ext2 logical drives and an ext3 one left needing recovery, plus a 5 GB exFAT disk; FreeDOS had mounted the FAT ones as D: and E:, exFAT is F:, ext2 G: and H:, ext3 I:, the big disk J:; a DOS session works on them through the redirector (DIR, TYPE, COPY, MD, REN, DEL, a batch file read from E:), FSTEST.BIN's 184 checks through KERNEL.DLL (long names and aliases, numbered aliases, a 300 KB file in pieces, writing, gaps, truncating, folders, moves, read-only, locks, directories that grow, an exFAT file that can't stay contiguous, ext2's indirect blocks and 8 KB blocks, links, an ext3 journal replayed and ext3 written through its journal (internal, and an external one with data=journal and CRC32 checksums), ext4 read with its checksums and written (extent trees grown, split and truncated, unwritten extents written, a hashed directory changed, bitmaps written for the first time), a long name on C:, case and dot files, a 4.5 GB exFAT file on a 4 KB-sector disk, a 5 GB sparse ext2 file, the time zone), LFNTEST.COM's long-name calls, share modes and locks from DOS, DOS sees D: and E: as its own drives again after OS1993 ends, and mtools, `tools/exfat.py`, debugfs, fsck.fat, fsck.exfat and e2fsck check the image; then a crash: QEMU killed right after writes to ext3 and ext4, e2fsck and the next boot both replay the journals; then btrfs (zstd with a subvolume, LZO, zlib with a damaged sector) on a disk of its own |
| `make qemu-net` | the network: the NE2000 and a DHCP lease, NETTEST.BIN's 26 checks (loopback UDP and TCP with a server thread, refused connections, ICMP port unreachable, timeouts, non-blocking sockets and select, 64 KB echoed by a server on the host), PING to the host, the loopback and nobody, IPCONFIG, and the host connecting in to a server in the guest |
| `make qemu-threads` | threads: 67 checks (joins and exit codes, critical sections, events, semaphores, mutexes, timeouts, TLS and errno per thread, suspend/terminate, the 16-thread limit, malloc and printf from several threads, virtual memory, private heaps, a child process, the file calls); a stack overflow hits the guard page, a read-only page faults, the last thread's code ends the process, `exit` from a second thread ends them all; then 16 MB committed on the 16 MB machine, written and read back twice through a 12 MB swap file |
| `make qemu-vesa` | the VBE display driver: Display Properties switches to 1024×768 high colour, 800×600 in 256 colours and true colour, No and the 15-second timeout go back, a DOS session and back keeps the mode, OS1993.INI restores it next session, and 640×480×16 is VGA again |
| `make qemu-tasklist` | Ctrl+Alt+Del: Close Program ends a program, works from a DOS session, and the VMM's screen ends a program that froze the GUI |
| `make qemu-notepad` | Notepad: typing, Save As with CR LF, the clipboard between two processes, Open, select/delete/undo, unsaved-changes questions, Find/Find Next/whole words, Replace All and its undo, Word Wrap, the file's bytes on disk |
| `make qemu-files` | file access: Paint saves, opens and replaces a BMP through the file dialogs, a second process reads it, then the file is checked on the disk image |
| `make qemu-shell` | the shell and its programs as processes: Start menu by mouse and Ctrl+Esc, desktop icons, Calculator, Paint, Clock, taskbar buttons, Run, Shut Down |
| `make qemu-msgbox` | keyboard accelerators per window, message boxes: modality, default button, Tab, Yes/No letters, Esc, Alt+F4 |
| `make qemu-menu` | menu bar, system menu and right-click menus by mouse and keyboard (mnemonics, F10, Alt+Space, keyboard Move) |
| `make qemu-explorer` | Explorer, the tree view and the list view: the address field, the four views, a folder made and named in place, opened, filled, deleted with its contents; the tree by keyboard, letters jumping to names, Enter opening a file in Notepad, sorting by menu and by a header click, double-clicking a folder |
| `make qemu-gui` | the newer GUI parts: Calculator's scientific view, Notepad's Print through a dialog template to a .PRN file, Paint's Attributes dialog, picture clipboard, zoom and undo, Explorer's toolbar tooltip, files clipboard, Back and Forward, a taskbar button's system menu; the printed file and the folder checked on the disk |
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
`build/SERIAL.LOG`. `OS1993 /D` also puts the kernel debugger there (see
*The kernel debugger*).

## How it boots

1. **`OS1993.COM`** (real mode, `loader/os1993.asm`) checks for a 486 (one
   that can set EFLAGS.AC; a 386 is refused with a message). If the CPU is
   in V86 mode under a memory manager (EMM386, JEMM386, QEMM), the manager
   must offer VCPI (see *Running under EMM386*). It then allocates the largest
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
| `D0000000–D010FFFF`       | kernel window on the System VM's 0–1 MB + HMA (`KLOW`)  |
| `D0400000–D04FFFFF`       | kernel thread stacks, each with an unmapped guard page below |
| `E0000000–E3FFFFFF`       | the running VM's extended memory window: XMS blocks and DPMI memory (swapped with the VM) |
| `D0110000–FFBFFFFF`       | otherwise free for future kernel mappings               |
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
- **Kernel stacks and the double fault.** Every kernel thread's stack has
  an unmapped guard page below it (`sched.c`). Running off a stack faults
  there, and the CPU can't push that fault's frame on the same stack, so it
  double-faults; the double fault is a task gate to a TSS of its own with
  its own stack (`gdt.c`), whose handler finds the thread from the faulting
  ESP and stops with "Kernel stack overflow in thread ...".
- **Demand paging and the swap file** (`pager.c`). A program's private
  memory -- its heap, VirtualAlloc'd pages and thread stacks -- is
  committed without frames: the page table entry says "demand zero" and
  the first touch faults in a zeroed frame. When fewer than 1 MB of frames
  are free, a clock hand goes round the processes' page tables and writes
  pages nobody touched since it last came by to `WIN386.SWP` next to
  VMM.BIN (`SwapFile=` KB in OS1993.INI's `[386Enh]`, 8 MB by default),
  through the VMM's own file system; touching one reads it back. Pages of a
  process with a thread inside a system call stay put, and the VMM's checks
  on buffers (`user_range_ok`, `user_writable`) fault pages in first, so the
  VMM never meets a missing page in the middle of using one. Without a
  drive of the VMM's own (DOSBox-X), or with `SwapFile=0`, there is no swap
  file and memory just runs out sooner.

## The DOS virtual machine

The DOS that started OS1993 keeps running, as a virtual 8086 machine under
the VMM (`kernel/v86.c`, `kernel/vpic.c`):

- **Entering and leaving V86 mode.** `v86_enter` (`v86entry.asm`) IRETs into
  V86 mode with TSS.ESP0 pointing just below the kernel's saved state. Traps
  out of V86 mode land there, and `v86_leave` unwinds them (setjmp/longjmp).
- **Instruction emulation.** V86 code runs at IOPL 0, so `CLI`, `STI`,
  `PUSHF`, `POPF`, `INT n`, `IRET` and `HLT` raise #GP and are emulated
  against a virtual interrupt flag. IOPL reads as 3 and can't be changed,
  as under EMM386, so a program that tests for real mode that way learns
  it isn't in it. CPU exceptions 0–7 are reflected to the real-mode IVT,
  as on a real 8086.
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

Programs that switch to protected mode by themselves, without DPMI (a DOS
extender with DPMI turned off, a raw HIMEM block move), are stopped with a
message: a VM gets DPMI and XMS from the VMM instead (below), as under
Windows.

### Virtual devices for DOS sessions

- **Timer** (`vpit.c`). Ports 40h and 43h trap: each VM has its own
  channel 0 (reload value, access and counting modes, latches, read-back).
  The real timer runs at the fastest rate any VM asked for (up to about
  2 kHz), the kernel counts its own 18.2 Hz tick out of that, and every
  real interrupt is shared out: a VM whose period has ended gets its IRQ 0.
  A count read from port 40h comes from the PIT clocks gone by since the VM
  loaded the counter. Channel 2 (the speaker) goes to the hardware.
- **Serial and printer ports** (`vdev.c`). COM1-COM4 and LPT1-LPT3 each
  belong to at most one VM: the first one to touch a device's ports gets
  it, if its settings allow, and from then on the device's interrupt goes
  to that VM, its ports are open to it in the I/O bitmap (switched with the
  VM, so its accesses run at full speed), and the real PIC follows its
  virtual one for that IRQ. Any other VM gets FFh and its writes are
  dropped, logged once ("VM 2 wants COM2, which VM 1 owns"), until the
  owner ends. COM1 stays the VMM's while it carries the debug log.
- **Mouse** (`vmouse.c`). While a DOS session has the focus, the kernel's
  mouse goes to that VM's INT 33h driver, answered in the monitor
  (functions 00h-26h as text-mode programs use them): position, buttons,
  press and release counts, ranges, mickeys, the software text cursor
  (screen and cursor masks), and event handlers, called as a real driver
  calls them -- a far call with the events in AX -- through a monitor
  breakpoint that restores the VM afterwards. Under the VMware absolute
  pointer the VM's coordinates follow the host's pointer exactly.
- **XMS and INT 15h** (`xms.c`, `vmx.c`). INT 2Fh AX=4300h/4310h point a
  session's programs at an XMS 3.0 driver in the monitor. Each VM has its
  own blocks, in a 64 MB extended memory window at E0000000h that is
  swapped with the VM (out of V86 code's reach); moves copy between blocks
  and conventional memory, a lock gives the block's linear address (which
  INT 15h AH=87h accepts and a DPMI client can use), A20 is always on, the
  HMA is DOS's. INT 15h AH=88h says there's no extended memory outside XMS,
  as with HIMEM loaded. The System VM keeps the real HIMEM.
- **Idle detection.** A VM that polls the keyboard (INT 16h AH=01h/11h, or
  the controller's status port) more than `Idle=` times in one timer tick
  without a key sleeps until the next tick or a key; the other VMs get the
  time. (`make qemu-dosvm`: a polling loop went round 950 times in three
  seconds instead of millions.)
- **Settings per session** (`vmset.c`), like a PIF file's: OS1993.INI's
  `[DOS]` section is read for every new session -- `Memory=` KB of
  conventional memory (the free block spanning the limit is split and the
  rest given to DOS itself; INT 12h agrees), `XMS=` and `DPMI=` KB (0: no
  driver), `Devices=` which ports it may own (`COM2 LPT1`, `all`, `none`),
  `Idle=`, `Mouse=`. The GUI can pass its own for one session
  (`StartDosSessionEx`, `struct vm_settings` in `user.h`).

### The DPMI host

`dpmi.c` is a DPMI 0.9 host, so DOS extenders and their programs run in a
DOS session: DOS/4GW and DOS32A programs (`make qemu-dpmi`), and anything
else that asks INT 2Fh AX=1687h. A program that calls the mode-switch entry
becomes the VM's client: it gets an LDT of its own, its code, data, stack,
PSP and environment as 16-bit descriptors, and runs in ring 3 in the VM's
thread, as its V86 code did. Both are the same trap frame on the VM's
kernel stack -- TSS.ESP0 leaves 16 bytes spare below it, so a ring-3 frame
can be rewritten into a V86 one in place -- so every mode switch is the
monitor rewriting that frame:

- **Interrupts.** Software interrupts trap (their gates are ring 0). INT 31h
  is the API; a vector the client hooked (0205h) goes to its handler;
  anything else is reflected to real mode as an INT instruction in a stub
  in the host's data area, so DOS calls still pass the DOS critical section,
  the redirector, the mouse and XMS, and come back with the general
  registers. Hardware interrupts go to a hooked protected-mode handler
  through a host stub that restores the virtual interrupt flag after its
  IRET, or to real mode. Exceptions go to the client's handlers (0203h) on
  a host stack; one it doesn't handle ends it.
- **The API**: descriptors (0000h-000Dh), DOS memory (0100h-0102h, by a
  real-mode INT 21h underneath), vectors and exception handlers
  (0200h-0205h), real-mode calls (0300h-0302h), real-mode callbacks (0303h),
  state save and raw mode switches (0305h, 0306h), version, memory (0500h-
  0503h, from the VM's extended memory window, up to `DPMI=` KB), locking,
  page size, adapter-space mapping, the virtual interrupt flag (0900h-0902h).
- **Protection.** The client's descriptors are its own, ring 3 only; paging
  keeps it out of the VMM and out of the GUI's processes (their private
  page tables come out of the directory while a client runs). A selector
  freed while it was loaded becomes null on the way back, as the CPU's cache
  would have hidden it.
- **espfix.** Going back to ring 3 on a 16-bit stack segment, IRET loads only
  SP, leaving the VMM's top half of ESP -- and code that uses ESP, or an EBP
  copied from it, as a 32-bit address then faults (DOS32A does). So such
  returns go through a ring-3 trampoline on a 32-bit stack that loads the
  whole of SS:ESP with LSS and IRETs from the client's own stack.
- **The end.** INT 21h AH=4Ch (or INT 20h, AH=00h) from the client's process
  ends it: everything it had goes, its environment segment goes back in the
  PSP, and DOS ends the program.

One client per VM at a time.

### Running under EMM386

With a memory manager loaded, the CPU runs DOS in V86 mode and the manager
owns protected mode. If it offers VCPI, the loader asks it for its map of
the first megabytes (DE01h: UMBs and the HMA as it has them, its own code
above), builds the page tables in extended memory through XMS moves (V86
mode can't reach it otherwise), and has it switch to protected mode with
them (DE0Ch); at the end the kernel switches back through the manager's
protected-mode entry, so DOS carries on under it. Meanwhile:

- The KLOW window follows that map, so the kernel sees UMBs where DOS does.
- The manager's resident code is rows of HLT instructions -- breakpoints
  into its monitor, asleep while OS1993 runs. The vectors it hooked
  (INT 13h, 15h, 19h, 40h, 4Bh) go to the BIOS's fixed entry points for
  them, and its own come back at the end; a breakpoint reached anyway (its
  hook on the XMS entry, which FreeDOS calls for A20) is answered by the
  monitor; VDS is off.
- INT 67h in a VM says "not supported": no EMS and no VCPI while OS1993
  runs; programs get XMS and DPMI from the VMM instead.

`make qemu-emm386` runs the self-tests this way under JEMM386.

## Ring 3: processes

The GUI is 32-bit flat protected-mode code in ring 3, as Windows 3.x ran
its protected-mode code in the System VM (`kernel/user.c`, `user/`). A GUI
session is a set of **processes**, each with its own address space and up
to 16 threads, like Win95's 32-bit side.

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
  `GetFileSize`, `SetEndOfFile`, `FlushFileBuffers`, `CloseHandle`,
  `DeleteFile`, `MoveFile`, `CopyFile`, `Create`/`RemoveDirectory`,
  `Get`/`SetFileAttributes`, `Get`/`SetFileTime` (DOS keeps the last write,
  to 2 seconds; `FileTimeToSystemTime` and back), `GetFullPathName`,
  `GetDiskFreeSpace`, `Get`/`SetCurrentDirectory`,
  `FindFirstFile`/`FindNextFile`/`FindClose`, `GetLastError`.
- **Threads.** A process has up to 16 (`kernel/user.c`, KERNEL.DLL's
  `thread.c`). Each has its own kernel thread, a stack in a region of its
  own (64 KB by default, up to 1 MB) with an uncommitted guard page below
  it -- running into it ends the process with "overflowed its stack"; all
  ring-3 code is compiled with `-fstack-clash-protection`, so a big frame
  can't step over it -- and a **TIB** at `FS:0` (as Win32's TEB: a ring-3
  descriptor the scheduler points at the running thread's TIB) holding its
  id, its own `errno` and `GetLastError`, and 64 TLS slots. `CreateThread`
  (`CREATE_SUSPENDED`, `ResumeThread`), `ExitThread`, `TerminateThread`,
  `GetExitCodeThread`. The last thread to end ends the process with its
  code; `ExitProcess` (or `exit`, or a fault) from any thread ends them all.
  Only the first thread may use the window system.
- **Synchronization** is built on one kernel primitive, a futex
  (`SYS_FUTEX_WAIT`: sleep while a word holds a value, with a timeout;
  `SYS_FUTEX_WAKE`). On it KERNEL.DLL builds critical sections (recursive;
  taking a free one is one `CMPXCHG`, no system call), events (manual and
  auto-reset), mutexes (recursive, owner-checked), semaphores,
  `WaitForSingleObject` on any of them or on a thread or process, and
  `WaitForMultipleObjects` (one or all). `Interlocked*` use the 486's
  `XADD` and `CMPXCHG`. The C runtime takes critical sections around its
  `FILE`s, so threads can share `printf`.
- **Processes.** `CreateProcess` gives a pid; `OpenProcess` a handle to
  wait on (`WaitForSingleObject`) and `GetExitCodeProcess`;
  `GetCommandLine`, `GetModuleFileName`.
- **Memory.** `SYS_SBRK` grows the process heap at `2000_0000`; KERNEL.DLL
  manages it (`LocalAlloc`, `GetProcessHeap`), locked for threads, every
  block header checked as the heap is walked. `HeapCreate` makes private
  heaps in regions of their own (`HeapAlloc`, `HeapReAlloc`, `HeapFree`,
  `HeapSize`, `HeapValidate`, `HeapDestroy`). **Virtual memory**
  (`SYS_VMEM`): `VirtualAlloc` reserves regions of `6200_0000`-`7F00_0000`
  in 64 KB steps and commits pages; `VirtualFree` decommits or releases,
  `VirtualProtect` makes pages read-only or writable, `VirtualQuery`
  describes them; `GlobalMemoryStatus`. There is no paging to disk:
  committing allocates the memory at once.
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
| `WSOCK.DLL` | `1050_0000` | Winsock: sockets over the VMM's TCP/IP stack, `select`, a DNS resolver (`user/net/`) |
| `CRT.DLL` | `1040_0000` | the C runtime (`user/crt/`); imports KERNEL. Also built as `libcrt.a` |
| `SHELL.DLL` | `1030_0000` | the shell API: `ShellExecute`, `ShellAbout`, `Shell_NotifyIcon`, `GetOpenFileName`/`GetSaveFileName`, `FindText`/`ReplaceText`, stock icons (`user/shell/`) |
| `*.BIN`   | `0040_0000` | programs (`user/apps/`): SHELL, CALC, PAINT, CLOCK, NOTEPAD, DEMO, PING, IPCONFIG; tests CRTTEST (and CRTSTAT), THRTEST, NETTEST |

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
- **Stack-clash probes.** `-fstack-clash-protection`: a function whose
  frame is bigger than a page touches each page on the way down, so it
  can't jump over a thread stack's guard page.
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

## The network

`kernel/net/` is a TCP/IP stack in the VMM, for one Ethernet card and the
loopback, as Windows for Workgroups and Win95 had theirs in VxDs.

- **The card** (`ne2000.c`): an NE2000 (DP8390) at port 300h, IRQ 9 --
  the ISA card of 1993, and QEMU's (`-nic user,model=ne2k_isa`, which
  `make qemu` and the tests use: QEMU answers DHCP and is the gateway,
  10.0.2.2, which reaches the host). Its 16 KB of memory holds one transmit
  buffer and a receive ring, moved through its data port a word at a time.
  Without a card only the loopback works.
- **The stack** (`net.c`, `socket.c`, `tcp.c`): Ethernet, ARP (a cache of
  16, a frame waiting while it asks), IPv4 (no fragments: sent with Don't
  Fragment, dropped when they come), ICMP (echo replies, unreachables to
  the sockets they're about, port unreachable for UDP nobody listens to),
  UDP, and TCP: the full state machine, the MSS option, 8 KB windows each
  way, retransmission from Jacobson's estimate (Karn's rule, backing off up
  to a minute, 8 tries), slow start and congestion avoidance, zero-window
  probes, TIME_WAIT (2 s), listen queues, and closing gracefully after the
  owner has let go (unread data resets, as on BSD). A **DHCP client**
  configures the card at boot and renews at half the lease.
- **How it runs.** The card's interrupt only copies frames into pool
  buffers; a kernel thread, woken by it and by each timer tick, takes them
  up the stack and runs the timers. Everything runs with interrupts off, so
  system calls (which may block, with timeouts, until the stack wakes the
  socket) see consistent state. Packets to 127.0.0.0/8 or our own address
  go round through the same queue. Sockets belong to a process and close
  when it ends; 32 in all.
- **WSOCK.DLL** (`user/net/winsock.h`): Winsock -- `socket`, `bind`,
  `connect`, `listen`, `accept`, `send`/`recv`, `sendto`/`recvfrom`,
  `shutdown`, `closesocket`, `setsockopt`/`getsockopt` (`SO_RCVTIMEO`,
  `SO_SNDTIMEO`, `SO_REUSEADDR`, `SO_BROADCAST`, `SO_ERROR` ...),
  `ioctlsocket` (`FIONBIO`, `FIONREAD`), `select`, `WSAPoll`,
  `getsockname`/`getpeername`, `inet_*`, `gethostbyname` and
  `getaddrinfo` (numbers, `localhost`, or a DNS query to the server DHCP
  gave), `WSAGetLastError` (and `errno`). Raw ICMP sockets get every ICMP
  packet, IP header and all. KERNEL.DLL's `NetCall` makes the system call,
  letting go of the window system's lock while it may wait.
- **PING** and **IPCONFIG** run in a **console window**: SHELL.DLL's
  `RunConsoleProgram` runs `main` on a second thread and shows its
  `stdout` in a read-only edit control (the CRT hands each line to it), the
  first thread moving the text in on a timer; the window stays when the
  program ends. Start them from Run: `ping -n 3 10.0.2.2`, `ipconfig`.

## Disks and file systems

The VMM has its own disk driver and file systems, as Windows for
Workgroups 3.11 had 32-bit File Access and 32-bit Disk Access: files on its
disks never go through DOS or the BIOS, OS1993 programs get long names,
and DOS programs get them too.

- **IDE** (`kernel/disk/ata.c`): both legacy channels, PIO, interrupt-driven
  (a transfer sleeps until the drive's interrupt; a timer tick rescues one
  that never comes), LBA28 and LBA48 (to 2 TB). Each disk is matched to its
  BIOS drive number by reading its first sector both ways, and the BIOS's
  geometry for it is noted. The VMM takes every channel with a disk and no
  ATAPI device (a CD driver in DOS needs that one): its ports read as empty
  from V86 code, and **INT 13h** for its disks is done by the VMM
  (`int13.c`): CHS calls with the BIOS's geometry, the EDD calls (41h-48h)
  in LBA. So DOS's own boot drive, C:, works through the VMM's driver too,
  and a disk may share a channel with it.
- **Partitions** (`part.c`): the MBR's four entries and the logical drives
  in an extended partition; a disk with no table but a FAT or exFAT boot
  sector or an ext2 superblock is one volume. **The block cache**
  (`cache.c`): 1024 sectors in a hash table, least recently used out first,
  **written behind**: a changed sector waits until it is two seconds old
  (a flusher thread), its buffer is needed, a file is flushed
  (FlushFileBuffers, DOS's commit, INT 21h AH=0Dh), raw INT 13h reads its
  range, or OS1993 ends; neighbours go out as one transfer.
- **FAT12, FAT16, FAT32** (`kernel/fs/fat.c`): the cluster count decides
  the type; every FAT copy is kept (unless FAT32 says otherwise), the FAT32
  FSInfo free count and next free cluster too, and a bitmap of the free
  clusters, read from the FAT once. FAT[1]'s "cleanly unmounted" bit is off
  while the volume is changed and back on when OS1993 ends. **Long names**
  (VFAT): read back from their entries (checked by sequence and checksum,
  an orphan ignored), written for any name that isn't plain 8.3, with a
  unique `BASIS~N.EXT` alias; an 8.3 name in one case per part gets
  Windows NT's case bits instead (`lower.txt`). Names match without regard
  to case, long or short. Open files are shared nodes, one per directory
  entry, so every handle sees one size; whole clusters move straight
  between the disk and the caller. Files up to 4 GB - 1, as FAT allows.
- **exFAT** (`kernel/fs/exfat.c`): free space from the allocation bitmap;
  a file or directory is one contiguous run (NoFatChain) while the cluster
  after its end is free, and gets a FAT chain when it can't grow in place.
  Entry sets (File, Stream Extension, File Name entries) are checked by
  their checksum and written with it and the name hash; names keep their
  case and compare through the volume's up-case table. Files of any size
  (64-bit); bytes past a file's valid data length read as zeros, and a file
  made longer is allocated but not written. Sectors of 512 bytes to 4 KB.
  Timestamps carry the 10 ms field and, once the time zone is known, the
  UTC offset; a time with another offset is moved to ours. The boot
  sector's VolumeDirty flag is on while the volume is changed, and
  PercentInUse is set when OS1993 ends. exFAT has no 8.3 names: DOS sees
  the name itself if it fits 8.3, otherwise "XXhhhh~1.EXT" from the name
  hash ("~2" ... "~9" for names that would share one); "." and ".." are
  made up for searches, as FAT has them.
- **ext2** (`kernel/fs/ext2.c`): block groups with their bitmaps and inode
  tables, blocks of 1 to 64 KB, files mapped through direct and single,
  double and triple indirect blocks (holes read as zeros; a file made
  longer is a hole), files of any size (large_file), new blocks after the
  file's last one and new directories spread over the groups; the free
  counts in the group descriptors and the superblock stay exact, and the
  volume is marked not clean while mounted. Directory records are split
  and folded back as Linux does; a dir_index tree's flag goes when its
  directory changes. Names are UTF-8 on the disk, Latin-1 here; ext2 tells
  case apart, so a lookup tries the exact name first and a new name can't
  differ from another only in case; DOS gets numbered aliases as on exFAT.
  The owner's write bit is the read-only attribute, a leading dot hidden;
  symbolic links read as their target. **Hard and symbolic links** can be
  made (`CreateHardLink`, `CreateSymbolicLink`). Times are kept in UTC,
  through the time zone. A volume with read-only-compatible features it
  doesn't know mounts read-only.
  **ext4**: extent trees (index blocks, uninitialized extents as zeros),
  64-bit block group descriptors, flex_bg, meta_bg, hashed directories
  (read as the linear lists they also are), and metadata_csum: the
  superblock's, group descriptors', inodes', extent blocks' and directory
  blocks' CRC32C checksums are checked, and what fails isn't read. It is
  written too, through its journal (checksum v3): new files and folders
  get extent trees, which grow (an extent extended when the next block
  follows on the disk), split full nodes and gain levels, and shrink when
  a file is truncated; writing into a preallocated range splits its
  unwritten extent; every checksum is kept up to date (inodes, extent and
  directory blocks with their tails, bitmaps, descriptors, the superblock,
  the journal's); a group whose bitmaps were never written (uninit_bg)
  gets them when first used; a hashed directory that changes becomes a
  plain list (valid, if slower for Linux). uninit_bg's CRC16 descriptors
  are kept too. Inline data, encryption, quotas, bigalloc and journal
  devices of its own aren't mounted, or mount read-only.
  **ext3 journal replay**: a volume that wasn't unmounted cleanly
  (needs_recovery) has its journal replayed first, as Linux and e2fsck do
  it: the committed transactions found, revoke records gathered, every
  logged block written unless a later revoke covers it (escaped magic
  numbers restored), then the journal emptied and needs_recovery cleared.
  **ext3 journalled writes**, in ordered mode as ext3 defaults to: every
  metadata block a call changes (superblock, group descriptors, bitmaps,
  inode tables, directory and indirect blocks) joins a transaction and is
  pinned in the cache; freed blocks are revoked; at the end of the call the
  file data goes out first, then descriptors, block copies and revoke
  records, then the commit block, and only then may the blocks go home
  (written behind). A full journal is checkpointed: everything home, the
  log starts over. needs_recovery is set while mounted and cleared by a
  clean shutdown; after a crash, the next mount -- or Linux, or e2fsck --
  replays what was committed. Several calls share a transaction: it
  commits after two seconds (the cache's flusher), when it grows big, on a
  flush (FlushFileBuffers, DOS's commit) and at shutdown. Journal
  checksums -- v2/v3 (CRC32C per block) and the old CRC32 in the commit
  block -- are checked when replaying (a transaction whose checksums fail
  didn't happen; a damaged copy is left out) and written. An **external
  journal** (a journal device of its own, found by UUID) is used like an
  internal one. **data=journal** (the volume's default mount option) logs
  file data too. Journals with async commit, and JBD v1, stay read-only.
- **btrfs** (`kernel/fs/btrfs.c`), read-only, one device: the chunk tree
  (bootstrapped from the superblock's system chunks; single and DUP), the
  root tree, file system trees and subvolumes (shown as folders), inode
  and directory items, inline and regular extents, holes (no_holes) and
  preallocated ranges as zeros, symbolic links. Compressed extents are
  decompressed with zlib, LZO or zstd (`kernel/fs/decomp.c`, written for
  this and checked on the host against the real compressors). Every node's
  CRC32C is checked as it is read, and the checksum tree's CRC32C of every
  data sector.
- **Drivers** plug in through an operations table (`kernel/fs/fsimpl.h`);
  `fs.c` mounts each partition with the first driver that knows it, and
  holds the lock, the list of volumes, the share modes and byte-range
  locks, and the time zone.
- **OS1993 programs** use the same calls as before (`CreateFile`,
  `FindFirstFile` ...); `files.c` sends paths on the VMM's drives straight
  to the file system, keeping their case. `WIN32_FIND_DATA` has the long
  `name`, the 8.3 `short_name` and `size_high`; `SetFilePointerEx` and
  `GetFileSizeEx` reach past 4 GB; `LockFile`/`UnlockFile` lock byte
  ranges against other handles and DOS programs; `SetTimeZoneInformation`
  sets the time zone and its daylight saving rules (Win32's
  TIME_ZONE_INFORMATION), which the shell keeps in OS1993.INI (`[timezone]
  tzbias=`, `dstbias=`, `dststart=`, `dstend=`: minutes, UTC = local +
  bias; month,week,day_of_week,hour). Timestamps are converted with the
  offset of their own moment.
- **DOS** (`kernel/fs/redir.c`) sees the same volumes as network drives: the
  drive letter's current directory structure is marked "network", so DOS
  sends every call for it to the network redirector, INT 2Fh AH=11h, as it
  does for MSCDEX and network clients. DOS itself issues those INT 2Fh
  calls; they trap into the V86 monitor, which answers them from the file
  system in 32-bit code, in whichever VM made them: 8.3 names from DOS's
  swappable data area, open files as DOS's own file table entries with our
  file number in them, searches held by the VMM with a slot and a stamp in
  the caller's DTA. **Share modes and locks** are enforced (INT 21h
  AH=5Ch, the deny modes of an open), between DOS programs and OS1993's.
  A volume DOS had mounted itself (the same boot sector, or serial number)
  keeps its letter: DOS's buffers are flushed and dropped first, and when
  OS1993 ends the drive is given back and DOS rebuilds it from the disk.
  DOS's boot drive C: is taken over too, as Windows 95 did, with its
  current directory -- unless DOS has a file open on it for writing (as
  OS1993 /T's redirected log); another drive with files open stays DOS's
  (the VMM lets go of that volume). So OS1993 programs and DOS programs
  get long names on C: as well. Other volumes get free letters below
  LASTDRIVE.
- **Long names for DOS programs** (`kernel/fs/lfn.c`): Windows 95's
  INT 21h AX=71xxh calls (mkdir, rmdir, chdir, delete, attributes and
  times, current directory, find first/next/close, rename, full path,
  extended open/create, volume information, file information by handle,
  time conversion, short names) on the VMM's drives, answered in the
  monitor or turned into the plain DOS call with the 8.3 path. On DOS's own
  drives DOS answers 7100h ("not supported"), so programs use 8.3 names
  there, as on a DOS without long names.

What doesn't work: writing btrfs, btrfs on several devices or RAID (see
TODO.md); TexFAT (exFAT with two FATs, which only Windows CE
uses: desktop Windows doesn't mount it either) and ATAPI or 4 KB-sector
drives; Unix owners, permissions beyond the write bit, and extended
attributes on ext2 (kept as they are, not shown: nothing in a Win32-style
API would show them); lost+found is e2fsck's to fill.

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
  desktop with icons (double-click or Enter opens; drag them about, their
  places kept in OS1993.INI; right click for Line up Icons, Run, About and
  Properties) over an optional background pattern; a taskbar with the
  **Start** button and menu (large icons and a vertical banner; Programs,
  read from PROGRAMS.INI, Run..., Display Settings..., About, Shut
  Down...; also Ctrl+Esc), a button per program window (click to switch,
  again to minimize, right button for the window's system menu), and the
  notification area with tray icons (and their tooltips) and a clock. A
  Run dialog starts any program by name. Ctrl+Alt+Del's Close Program
  marks programs that stopped reading their messages "Not responding";
  End Task asks a program to close (WM_CLOSE) before ending it. Shut Down
  asks, then ends the session.
- **CALC.BIN**: the standard calculator: the four operations, square root,
  percent, reciprocal, sign, memory, by mouse or keyboard. Without an FPU
  or libgcc, it computes in 64-bit fixed point (six decimals, twelve
  digits) with its own division. View > Scientific adds sin, cos, tan
  (degrees or radians), ln, log, e^x, x^y, x², x³, n!, pi, Int, Mod, And,
  Or, Xor, Not and hex/octal/binary for whole numbers, computed with nine
  decimals inside by series after reducing the argument.
- **PAINT.BIN**: a canvas (400×280 to start, up to 1024×768 with Image >
  Attributes), pencil, brush, eraser, flood fill, line, rectangle, filled
  rectangle, ellipse (rubber-banded) and select, a 16-colour palette (left
  button foreground, right button background). A selection drags about and
  goes through the clipboard as a picture (Cut, Copy, Paste, Delete,
  Select All); View > Zoom magnifies 2-8 times with scroll bars; Edit >
  Undo takes back the last change. Open, Save and Save As (Ctrl+O, Ctrl+S)
  through the file dialogs: it writes 16-colour BMP files and opens 16- and
  256-colour ones (colours mapped to the nearest of the 16); it asks before
  discarding changes.
- **The file dialogs** (SHELL.DLL): "Look in" (the drives, with their
  labels), the folder, a file name field (its text selected, so typing
  replaces it), a list of subfolders and matching files, and "Files of
  type" (the program's filters); double-click or type a folder name to
  move, a pattern to filter. Names keep their case and length where the
  file system has them (8.3 names on FAT in capitals, as DOS keeps them).
  Save As asks before replacing a file; Open checks it exists. **The
  colour dialog** (`ChooseColor`): the 16 theme colours, 16 custom ones,
  and red/green/blue sliders with a preview.
- **NOTEPAD.BIN**: a text editor on the multi-line edit control: File
  (New, Open, Save, Save As, Print; plain text with CR LF line ends, up to
  512 KB), Edit (Undo, Cut, Copy, Paste, Delete, Select All, Time/Date,
  Word Wrap), Search (Find, Find Next F3, Replace; match case, whole words,
  up/down, Replace All as one undoable edit), View (a status bar with the
  caret's line and column), and the usual question about unsaved changes.
  Print lays the text out in pages (header, page numbers, form feeds) and,
  there being no printer driver, writes them to a .PRN file.
  `NOTEPAD.BIN name.txt` opens a file.
- **EXPLORER.BIN**: the file explorer: My Computer, its drives (with
  their labels: `GetLogicalDrives`, `GetVolumeInformation`) and their
  folders in a tree on the left, filled in as they are opened; the folder
  chosen on the right in a list view, in any of its four views (details:
  name, size, type, modified; a header click sorts, an arrow showing how);
  a toolbar (Back, Forward, Up, Cut, Copy, Paste, Delete, Properties, the
  views; tooltips) and an address field; a status bar (objects, their size,
  free space); the bar between the panes moves. Cut or Copy and Paste move
  or copy files and folders (cut ones fade until pasted); items dragged
  onto a folder or drive in either pane move there, or are copied (another
  drive, or Ctrl). File types (name, icon, the program that opens them)
  come from FILETYPES.INI. Enter or a double-click opens a folder or
  drive, runs a program, or opens a file in Notepad; Backspace goes up and
  selects the folder it came from; Tab switches panes; Delete deletes
  (folders with what they hold, after asking); F2 renames in place; File >
  New Folder makes one and lets you name it; Properties counts a folder's
  files; F5 reads it again. The desktop's My Computer opens it.
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
  default-button ring, check box, three-state box, radio button group,
  group box, owner-drawn, push-like), `STATIC` (text wrapped at spaces,
  aligned, '&' mnemonics; frames, etched lines, owner-drawn), `EDIT`
  (selection by mouse, Shift and double-click, sideways scrolling, a
  blinking caret, undo; password, number, case styles), `LISTBOX` with its
  own scroll bar (item data, sorting, multiple and extended selection,
  owner-drawn rows, type-ahead),
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
  The multi-line edit keeps an undo history (Ctrl+Z steps back a run of
  typing at a time, Ctrl+Y forward), blinks its caret, and scrolls while
  a selection is dragged past its edges. The clipboard also carries
  pictures (`SetClipboardBitmap`) and Explorer's files (`CF_FILES`).
  Every control draws inside its own rectangle, focus and default rings
  included. Parents can colour controls (`WM_CTLCOLOR*`) or draw them
  (`WM_DRAWITEM`, `WM_MEASUREITEM`); a window's `WM_NCHITTEST` answer can
  make part of its client area a caption or a border; anchored children
  follow the parent's edges (`SetWindowAnchor`).
- **Window text** is in a proportional font (`FONT_GUI`: the VGA glyphs,
  each trimmed to its inked columns) -- captions, menus, buttons, labels,
  lists, tabs -- with `DrawText` (alignment, word wrap, ellipses, '&'
  underlines), `SetTextColor`/`SetBkColor`; edit controls keep the 8×16
  fixed font. **Dialog templates**: `DialogBox` and `CreateDialog` build a
  dialog and its controls from a table, with a `DLGPROC` and
  `WM_INITDIALOG`/`EndDialog`; `IsDialogMessage` adds Alt+letter
  mnemonics, arrow keys within a group and Ctrl+Tab between tabs.
- **Status bar, toolbar, tooltips** (`statusbar.c`, `toolbar.c`,
  `tooltip.c`): status bars of parts with a size grip, toolbars of
  picture buttons (check groups, flat hot-tracking, tooltips), and
  tooltips that watch the pointer themselves -- no relaying of mouse
  messages -- and show a tool's text after a short rest.
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
  the closed box's. `CBS_SIMPLE` shows the list always, editable boxes
  complete what is typed from the items, `CBS_OWNERDRAWFIXED` items are the
  parent's to draw, and `CB_SHOWDROPDOWN` returns at once.
  `TABCONTROL` draws Platinum tabs, the selected one joined to its page
  (`TCM_INSERTITEM` with a `TCITEM`, `TCM_ADJUSTRECT`, Left/Right), icons
  from an image list, rows that wrap (`TCS_MULTILINE`) or arrows that
  scroll; `TCN_SELCHANGING` and `TCN_SELCHANGE` come as `WM_NOTIFY` and
  as `WM_COMMAND`. `UPDOWN` steps a position with auto-repeat that speeds
  up, asks the parent first (`UDN_DELTAPOS`) and reports `WM_VSCROLL
  SB_THUMBPOSITION`; a buddy edit can show it (`UDS_SETBUDDYINT`, with
  thousands separators, or in hex) and lend it its arrow keys
  (`UDS_ARROWKEYS`).
  The **tree view** (`WC_TREEVIEW`) shows items under items, a row each
  while their parents are open, with +/- boxes, dotted lines, an image and
  a selected image; items given `cChildren = 1` get a box before they have
  children, so a program fills them in when it hears `TVN_ITEMEXPANDING`.
  Keys as in Windows (Left closes or goes up, Right opens or goes down,
  `+`, `-`, `*`, a letter jumps to the next name with it); scroll bars
  both ways when needed (`TVM_INSERTITEM` with `TVI_SORT`, `TVM_EXPAND`,
  `TVM_GETNEXTITEM`, `TVM_SELECTITEM`, `TVM_HITTEST`, `TVM_ENSUREVISIBLE`,
  `TVM_SORTCHILDREN`...). The **list view** (`WC_LISTVIEW`) shows items
  with an image, a label and sub-items four ways: large icons in a grid
  (labels wrapped under them, all of the label for the focused item),
  small icons in rows, a list in columns scrolled sideways, and details
  under column headers that can be clicked (`LVN_COLUMNCLICK`) and resized
  by dragging their edges; `LVM_SETVIEW` (or the style) switches. Several
  items may be selected (Ctrl, Shift, Ctrl+Space); the arrow keys follow
  the view; labels are edited in place (F2, `LVM_EDITLABEL`:
  `LVN_BEGINLABELEDIT`, `LVN_ENDLABELEDIT`); `LVM_SORTITEMS` takes a
  comparison of `lParam`s (a stable merge sort). Both draw from **image
  lists** (`ImageList_Create`, `ImageList_AddPixels`, `ImageList_Draw`
  with `ILD_SELECTED`; SHELL.DLL's `ImageList_AddIcon` adds a stock icon)
  and tell their parent through **`WM_NOTIFY`** (`NMHDR`, `NMTREEVIEW`,
  `NMLISTVIEW`, `NMLVDISPINFO`, `NMLVKEYDOWN`). Both take check boxes
  (`TVS_CHECKBOXES`, `LVS_EX_CHECKBOXES`) and report drags
  (`TVN_BEGINDRAG`, `LVN_BEGINDRAG`; drop targets highlighted with
  `TVGN_DROPHILITE`, `LVIS_DROPHILITED`); the tree edits labels too. The
  list view adds grid lines and full-row selection, sort arrows in the
  header, a marquee over empty space, icons placed and dragged anywhere
  (`LVM_SETITEMPOSITION`, `LVM_ARRANGE`), faded cut items, and virtual
  lists (`LVS_OWNERDATA`: `LVM_SETITEMCOUNT`, `LVN_GETDISPINFO`).
- **Menus** take owner-drawn items, pictures and icons beside items, a
  banner up the side (`SetMenuBanner`), `InsertMenu`/`DeleteMenu`/
  `ModifyMenu`, tell the owner `WM_MENUSELECT`, and a menu bar too wide for
  its window wraps onto more rows. Message boxes can be task- or
  system-modal and beep through the PC speaker (`MessageBeep`, `Beep`).
  Update regions are lists of rectangles, so far-apart changes repaint
  apart.
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
  merged. While a DOS session has the screen they go to its INT 33h driver
  (`vmouse.c`, under *Virtual devices for DOS sessions*).
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

The GUI draws its cursor as an XOR image in 16-colour mode (drawing it
twice erases it, so it needs no save-under) and, in the VESA modes, as a
solid white-and-black sprite over a saved copy of what it covers. The
shape follows the pointer: resize arrows over borders, an I-beam over
edits, a class's or window's own cursor elsewhere, or what a program sets
(`LoadCursor`, `SetCursor`: wait, cross, move, no-entry...).

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

## The kernel debugger

`OS1993 /D` starts the kernel debugger on COM1 (`kernel/kdbg.c`), in the
spirit of WDEB386: a terminal on the serial line (115200 8N1) gets a `#`
prompt when Ctrl+C arrives on the line (polled from the timer interrupt),
when a breakpoint set there is hit -- the 486's debug registers, so
execution breakpoints and data watches (`BP`, `BW`, `BR`) work in the
kernel, in ring 3 and in V86 mode alike, and nothing is patched -- after a
single step (`T`), on an INT 3 in the kernel, and when the VMM panics or
double-faults, for a last look before the blue screen. The machine is
frozen while the prompt is up.

```
R [reg=val]        registers           D[B|W|D] addr [n]   dump       E addr b ...  enter
BP addr            execution break     BW|BR addr [len]    data watch BL / BC n|*  list, clear
G [addr]  T [n]    go, step            K                   stack (EBP chain)
.T .VM .P .M       threads, VMs, processes, memory          .PT addr   page table walk
.CRASH             overflow a thread's stack (the double-fault test)   .EXIT  Q
```

Addresses are linear, `seg:off` in the current VM, or `%phys`; memory that
isn't mapped shows as `??` instead of faulting.

## Source layout

```
loader/os1993.asm        real-mode loader (.COM)
kernel/entry.asm         VMM.BIN header, _start, exit_to_dos, gdt/idt load
kernel/isr.asm           exception + IRQ stubs → interrupt_dispatch()
kernel/vmm.c             kmain
kernel/pmm.c paging.c heap.c mmtest.c   memory manager
kernel/v86.c v86entry.asm vpic.c         V86 monitor and virtual PIC
kernel/vpit.c vdev.c vmouse.c            each VM's timer, COM/LPT ownership, INT 33h mouse
kernel/xms.c vmx.c                       XMS and INT 15h for VMs, their extended memory window
kernel/dpmi.c                            the DPMI host
kernel/vmset.c                           per-session settings (OS1993.INI [DOS])
kernel/pager.c                           demand paging and the swap file
kernel/kdbg.c                            the kernel debugger on COM1
kernel/v86test.c dosshell.c              DOS calls from the kernel, DOS sessions
kernel/vm.c sched.c switch.asm           VMs, address spaces, focus; threads
kernel/vmtest.c                          multi-VM self-test
kernel/dosmgr.c                          one DOS shared by all VMs
kernel/random.c                          random numbers (ChaCha20 from interrupt timing)
kernel/net/                              the NE2000, Ethernet/ARP/IP/ICMP/DHCP, sockets and UDP, TCP
kernel/disk/                             IDE (ATA PIO, interrupts, LBA48), INT 13h for DOS, partitions, the block cache
kernel/fs/                               FAT12/16/32 with long names, exFAT, ext2/3/4, btrfs (and zlib/LZO/zstd), the DOS redirector (INT 2Fh AH=11h), long names for DOS (INT 21h AX=71xxh)
kernel/files.c                           file calls for ring 3: DOS's drives through DOS, the VMM's directly
kernel/vga.c gfx.c                       VGA state (VDD), planar drawing
kernel/display.c vbe.c                   display modes: the VBE adapter, the VBE BIOS probe
kernel/user.c                            ring-3 loader, INT 30h system calls
kernel/mouse.c                           PS/2 mouse (IRQ 12)
user/lib/os1993.h                        the public API; crt0, the module header and linker script
user/crt/                                the C runtime: CRT.DLL and libcrt.a; include/ has its headers
user/net/                                WSOCK.DLL (winsock.h)
user/krnl/                               KERNEL.DLL (system call wrappers)
user/gfx/                                GFX.DLL (drawing: planar VGA, linear 8/16/32 bits)
user/ui/                                 UI.DLL (windows, messages, controls)
user/shell/                              SHELL.DLL (shell API, stock icons)
user/apps/                               SHELL.BIN, CALC.BIN, PAINT.BIN, CLOCK.BIN, DEMO.BIN
kernel/gdt.c idt.c pic.c timer.c keyboard.c console.c serial.c kprintf.c panic.c
kernel/include/          headers; bootinfo.h is shared with the loader
kernel/linker.ld         links at 0x80000000, header first
test/dosbox-x.conf       the emulated 1993 machine
tools/                   check486.sh, check-test.sh, mkfreedos.sh, mkdata.sh, exfat.py, decomp-test.c, the QEMU tests
test/dos/lfntest.asm     a DOS program using the long-name calls, share modes and locks
test/dos/dosvm.asm       a DOS program exercising a session's virtual devices and DPMI
test/dos/dpmitest.c      a DOS/4GW program (Open Watcom) for the DPMI host
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

1. ~~**Memory manager.**~~ Done: frame allocator, paging API, kernel heap,
   demand paging with a swap file, guard pages under kernel stacks and a
   double-fault task gate.
2. ~~**TSS + V86 monitor.**~~ Done: DOS runs as a V86 VM, the kernel calls
   DOS and BIOS through it, and the DOS prompt runs inside it.
3. ~~**Multiple VMs.**~~ Done: private address spaces, preemptive
   scheduling, virtual keyboard and display, Alt+Tab focus switching, and
   one DOS shared by all VMs with a critical section.
4. ~~**DOS services from protected mode.**~~ Done: programs' files go
   through `INT 21h` in the System VM on DOS's drives, and through the
   VMM's own IDE driver and FAT12/16/32 (long names), exFAT and ext2
   file systems on the others, which DOS reaches through a redirector in the V86 monitor (as
   32-bit File Access did in Windows for Workgroups 3.11), with INT 13h for
   DOS done by the VMM and long names for DOS programs. Still to do: the
   other file systems in TODO.md.
5. **More virtual devices (VxD-style).** The PIC, keyboard, text display,
   timer, mouse (INT 33h), serial and printer ports (one owner each), XMS
   and idle detection are done, with settings per session. Still to do:
   CRTC ports for background VMs and graphics modes.
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
   (CRT.DLL and libcrt.a), threads with Win32's synchronization, virtual
   memory and heaps, and networking (an NE2000 driver, TCP/IP in the VMM,
   Winsock, PING and IPCONFIG in console windows).
7. **Win16-style API layer.** 16-bit protected-mode apps in ring 3 with LDTs,
   built with Open Watcom.
8. ~~**Coexisting with EMM386** and acting as a DPMI host.~~ Done: OS1993
   starts through VCPI under EMM386-style managers and goes back to them,
   and DOS sessions have a DPMI 0.9 host (DOS/4GW, DOS32A).
