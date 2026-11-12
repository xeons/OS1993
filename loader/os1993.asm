; =============================================================================
; OS1993.COM -- real-mode loader for the OS1993 Virtual Machine Manager
;
; Plays the role WIN.COM + WIN386.EXE's startup code played in 1993:
;
;   1. verify a 486+ CPU, in real mode -- or in V86 mode under a memory
;      manager (EMM386, JEMM386, QEMM) that offers VCPI, which then hands
;      the machine over for as long as OS1993 runs
;   2. allocate extended memory from HIMEM.SYS through the XMS interface
;   3. load VMM.BIN (from the same directory as this program) into it
;   4. switch to protected mode, build page tables, enable paging
;   5. jump to the kernel at 0x80000000 with EBX -> bootinfo
;
; The kernel can later jump back to pm32_return, which unwinds everything
; and returns to the DOS prompt with the kernel's exit code as ERRORLEVEL.
;
; Command line:  OS1993 [/T] [/D]
;   /T   test mode: kernel exits on its own after ~2 seconds
;   /D   the kernel debugger on COM1 (Ctrl+C on the line breaks in)
;
; Assemble: nasm -f bin -o OS1993.COM os1993.asm
; =============================================================================

        cpu     386
        bits    16
        org     0x100

%define KHDR_MAGIC      0x334D4D56      ; "VMM3"  (see kernel/entry.asm)
%define BI_MAGIC        0x3339534F      ; "OS93"  (see kernel/include/bootinfo.h)
%define BI_FLAG_TEST    1
%define BI_FLAG_DEBUG   2
%define BI_FLAG_VCPI    4
%define CHUNK           0x4000          ; file read / XMS move chunk size
%define MIN_ARENA_KB    1024
%define MIN_FREE        0x10000         ; free arena space required after tables

; Loader GDT selectors
SEL_CODE32      equ     0x08            ; 32-bit code, base = loader segment
SEL_DATA32      equ     0x10            ; 32-bit data, base = loader segment
SEL_FCODE       equ     0x18            ; flat 32-bit code (kernel entry)
SEL_FDATA       equ     0x20            ; flat 32-bit data
SEL_CODE16      equ     0x28            ; 16-bit code, base = loader (return path)
SEL_DATA16      equ     0x30            ; 16-bit data, base = loader (return path)
SEL_TSS         equ     0x38            ; a TSS, for VCPI's switch to protected mode
SEL_VCPI        equ     0x40            ; the VCPI server's three descriptors (DE01h)

; Kernel image header (first bytes of VMM.BIN)
KH_MAGIC        equ     0
KH_VERSION      equ     4
KH_VBASE        equ     8
KH_ENTRY        equ     12
KH_FILESIZE     equ     16
KH_MEMSIZE      equ     20

; I/O delay for old 8259s
%macro IODELAY 0
        jmp     short $+2
        jmp     short $+2
%endmacro

; -----------------------------------------------------------------------------
; Entry. CS=DS=ES=SS=PSP, as for any .COM program.
; -----------------------------------------------------------------------------
start:
        cld
        mov     di, bss_start  ; DOS does not zero our .bss
        mov     cx, bss_end - bss_start
        xor     al, al
        rep     stosb
        mov     sp, loader_stack_top    ; own stack, so we can shrink to fit
        mov     dx, msg_banner
        call    print

        call    check_386               ; 8086-safe code only up to here
        jnc     .is386
        mov     dx, err_cpu
        jmp     die
.is386:
        call    check_486               ; 386 instructions are safe now
        jnc     .is486
        mov     dx, err_cpu486
        jmp     die
.is486:
        smsw    ax                      ; MSW.PE set while in real mode => V86
        test    al, 1
        jz      .real_mode
        call    check_vcpi              ; a memory manager: does it speak VCPI?
        jnc     .real_mode
        mov     dx, err_v86
        jmp     die
.real_mode:
        ; Give conventional memory back to DOS: keep only our image + .bss.
        ; Programs the kernel later runs in the DOS VM need the space.
        mov     bx, bss_end
        add     bx, 15
        shr     bx, 4
        mov     ah, 0x4A                ; resize memory block, ES = PSP
        int     0x21

        call    parse_cmdline
        call    find_kernel_path

        ; ---- XMS driver ----------------------------------------------------
        mov     ax, 0x4300
        int     0x2F
        cmp     al, 0x80
        je      .have_xms
        mov     dx, err_noxms
        jmp     die
.have_xms:
        push    es
        mov     ax, 0x4310
        int     0x2F
        mov     [xms_entry], bx
        mov     [xms_entry+2], es
        pop     es

        mov     ah, 0x08                ; query free extended memory
        xor     bl, bl
        call    far [xms_entry]         ; AX = largest free block (KB)
        cmp     ax, MIN_ARENA_KB
        jae     .enough
        mov     dx, err_nomem
        jmp     die
.enough:
        mov     [xms_kb], ax
        mov     dx, ax
        mov     ah, 0x09                ; allocate EMB, DX = KB
        call    far [xms_entry]
        cmp     ax, 1
        je      .allocated
        mov     dx, err_xms
        jmp     die
.allocated:
        mov     [xms_handle], dx
        mov     ah, 0x0C                ; lock EMB -> DX:BX = physical address
        call    far [xms_entry]
        cmp     ax, 1
        je      .locked
        mov     dx, err_xms
        jmp     die
.locked:
        mov     [xms_phys], bx
        mov     [xms_phys+2], dx
        mov     ah, 0x03                ; global enable A20
        call    far [xms_entry]
        cmp     ax, 1
        je      .a20_on
        mov     dx, err_xms
        jmp     die
.a20_on:
        ; arena = XMS block rounded inward to 4K boundaries
        mov     eax, [xms_phys]
        add     eax, 0xFFF
        and     eax, 0xFFFFF000
        mov     [arena_phys], eax
        mov     ebx, eax
        sub     ebx, [xms_phys]
        mov     [arena_skew], ebx       ; offset of arena within the EMB
        movzx   ecx, word [xms_kb]
        mov     [bi_xms_kb], ecx
        shl     ecx, 10
        sub     ecx, ebx
        and     ecx, 0xFFFFF000
        mov     [arena_size], ecx

        mov     dx, msg_xms1
        call    print
        movzx   eax, word [xms_kb]
        call    print_dec
        mov     dx, msg_xms2
        call    print
        mov     eax, [arena_phys]
        mov     cx, 8
        call    print_hex
        mov     dx, msg_crlf
        call    print

        call    load_kernel
        call    layout_memory
        cmp     byte [vcpi_mode], 0
        je      .no_vcpi
        call    vcpi_tables             ; page tables through XMS moves
.no_vcpi:

        ; ---- prepare protected mode ----------------------------------------
        xor     eax, eax
        mov     ax, cs
        shl     eax, 4
        mov     [loader_base], eax
        mov     bx, SEL_CODE32
        call    set_gdt_base
        mov     bx, SEL_DATA32
        call    set_gdt_base
        mov     bx, SEL_CODE16
        call    set_gdt_base
        mov     bx, SEL_DATA16
        call    set_gdt_base
        add     eax, gdt
        mov     [gdtr_base], eax
        mov     [rm_jmp_seg], cs        ; patch the far jump back to real mode

        in      al, 0x21                ; remember DOS's IRQ masks
        mov     [pic1_mask], al
        in      al, 0xA1
        mov     [pic2_mask], al

        mov     dx, msg_go
        call    print

        cli
        mov     [rm_ss], ss
        mov     [rm_sp], sp
        cmp     byte [vcpi_mode], 0
        je      .raw
        mov     eax, [loader_base]      ; VCPI: the server switches, paging on
        mov     ebx, eax
        add     ebx, loader_tss
        mov     [gdt + SEL_TSS + 2], bx
        shr     ebx, 16
        mov     [gdt + SEL_TSS + 4], bl
        mov     [gdt + SEL_TSS + 7], bh
        mov     ebx, eax
        add     ebx, gdtr
        mov     [vs_gdtr], ebx
        mov     ebx, eax
        add     ebx, vcpi_idtr
        mov     [vs_idtr], ebx
        mov     ebx, [pd_phys]
        mov     [vs_cr3], ebx
        lea     esi, [eax + vcpi_switch]
        mov     ax, 0xDE0C
        int     0x67                    ; -> pm32_vcpi_entry
        mov     dx, err_vcpi
        jmp     die
.raw:
        lgdt    [gdtr]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     SEL_CODE32:pm32_entry

; -----------------------------------------------------------------------------
; 32-bit protected mode, paging off. CS/DS = loader-based segments, so
; labels work unchanged; ES = flat for writing page tables in extended memory.
; -----------------------------------------------------------------------------
        bits    32
pm32_entry:
        mov     ax, SEL_DATA32
        mov     ds, ax
        mov     ss, ax
        movzx   esp, sp                 ; same base as real-mode SS (a .COM)
        mov     ax, SEL_FDATA
        mov     es, ax

        ; zero page directory + all page tables
        mov     edi, [pd_phys]
        mov     ecx, [tables_bytes]
        shr     ecx, 2
        xor     eax, eax
        rep     stosd

        ; PT0: identity map 0-4 MB
        mov     edi, [pt0_phys]
        mov     eax, 0x003              ; present | writable
        mov     ecx, 1024
.ident: stosd
        add     eax, 0x1000
        loop    .ident

        ; arena page tables (contiguous): map every arena page
        mov     edi, [pta_phys]
        mov     eax, [arena_phys]
        or      eax, 0x003
        mov     ecx, [arena_pages]
.arena: stosd
        add     eax, 0x1000
        loop    .arena

        ; page directory
        mov     ebx, [pd_phys]
        mov     eax, [pt0_phys]
        or      eax, 0x003
        mov     [es:ebx], eax           ; PDE 0 -> identity table
        mov     edx, [arena_virt]
        shr     edx, 22
        lea     edi, [ebx + edx*4]
        mov     eax, [pta_phys]
        or      eax, 0x003
        mov     ecx, [n_pt]
.pde:   stosd
        add     eax, 0x1000
        loop    .pde
        mov     eax, ebx
        or      eax, 0x003
        mov     [es:ebx + 1023*4], eax  ; recursive mapping

        mov     cr3, ebx
        mov     eax, cr0
        or      eax, 0x80000000
        mov     cr0, eax
        jmp     short kernel_jump       ; flush the 386 prefetch queue

; VCPI's switch lands here: protected mode, paging on with our tables.
pm32_vcpi_entry:
        mov     ax, SEL_DATA32
        mov     ds, ax
        mov     ss, ax
        movzx   esp, word [rm_sp]

kernel_jump:
        mov     eax, [k_entry]
        mov     edx, [loader_base]
        lea     ebx, [edx + bootinfo]
        add     esp, edx                ; make ESP a flat address
        mov     cx, SEL_FDATA
        mov     ds, cx
        mov     es, cx
        mov     fs, cx
        mov     gs, cx
        mov     ss, cx
        push    dword SEL_FCODE
        push    eax
        retf                            ; -> kernel _start, EBX = &bootinfo

; -----------------------------------------------------------------------------
; Return path. The kernel restores our GDTR and far-jumps here with
; interrupts off, paging on, AL = exit code. We run identity mapped.
; -----------------------------------------------------------------------------
pm32_return:
        mov     cx, SEL_DATA32
        mov     ds, cx
        mov     [exit_code], al
        cmp     byte [vcpi_mode], 0
        jne     vcpi_return
        mov     ecx, cr0
        and     ecx, 0x7FFFFFFF         ; paging off
        mov     cr0, ecx
        jmp     short .paging_off
.paging_off:
        xor     ecx, ecx
        mov     cr3, ecx                ; flush TLB
        mov     cx, SEL_DATA16          ; real-mode-compatible segment caches
        mov     ds, cx
        mov     es, cx
        mov     fs, cx
        mov     gs, cx
        mov     ss, cx
        jmp     SEL_CODE16:pm16_return

        bits    16
pm16_return:
        mov     eax, cr0
        and     al, 0xFE                ; leave protected mode
        mov     cr0, eax
        db      0xEA                    ; jmp far seg:rm_return
        dw      rm_return
rm_jmp_seg:
        dw      0                       ; patched with CS at startup
rm_return:
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, [rm_ss]
        xor     esp, esp
        mov     sp, [rm_sp]
        lidt    [rm_idtr]
rm_return_common:
        call    restore_pic
        sti

        mov     ax, 0x0003              ; reset text mode, like Windows on exit
        int     0x10
        call    xms_release

        mov     dx, msg_back
        call    print
        movzx   eax, byte [exit_code]
        mov     cx, 2
        call    print_hex
        mov     dx, msg_crlf
        call    print
        mov     al, [exit_code]
        mov     ah, 0x4C
        int     0x21

; -----------------------------------------------------------------------------
; Back to V86 mode under the memory manager: VCPI DE0Ch from protected mode.
; Paging stays on with the kernel's tables, whose 0-4 MB are the System VM's
; (the server's own map, which DE01h gave us). The stack holds the V86 state
; the server IRETs to, under the far call's return address.
; -----------------------------------------------------------------------------
        bits    32
vcpi_return:
        mov     edx, [loader_base]
        mov     cx, SEL_FDATA           ; zero-based: the server finds the frame by linear address
        mov     ss, cx
        lea     esp, [edx + vcpi_stack_top]
        xor     eax, eax
        mov     ax, [rm_jmp_seg]        ; the loader's segment
        push    eax                     ; GS
        push    eax                     ; FS
        push    eax                     ; DS
        push    eax                     ; ES
        movzx   ebx, word [rm_ss]
        push    ebx                     ; SS
        movzx   ebx, word [rm_sp]
        push    ebx                     ; ESP
        push    dword 0                 ; EFLAGS (reserved)
        push    eax                     ; CS
        push    dword rm_vcpi_return    ; EIP
        mov     ds, cx                  ; zero-based: the server reads its own data through DS
        mov     es, cx
        mov     ax, 0xDE0C
        call    far [edx + vcpi_pm_entry]   ; never returns here

        bits    16
rm_vcpi_return:                         ; V86 mode again; the server set SS:SP
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        jmp     rm_return_common        ; (no LIDT: the server's IDT stays)

; -----------------------------------------------------------------------------
; Subroutines (real mode)
; -----------------------------------------------------------------------------

; In V86 mode: CF clear if a VCPI server is there (INT 67h AX=DE00h).
check_vcpi:
        push    es
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:0x67 * 4]
        or      ax, [es:0x67 * 4 + 2]
        pop     es
        jz      .no                     ; no INT 67h handler at all
        mov     ax, 0xDE00
        int     0x67
        test    ah, ah
        jnz     .no
        mov     byte [vcpi_mode], 1
        or      dword [bi_flags], BI_FLAG_VCPI
        mov     dx, msg_vcpi
        call    print
        clc
        ret
.no:    stc
        ret

; VCPI: the server's map of the first megabytes (DE01h), then the page
; directory and tables built in `buffer` and moved into the arena with XMS:
; in V86 mode there's no other way into extended memory.
vcpi_tables:
        push    es
        push    ds
        pop     es
        mov     di, vcpi_pt             ; ES:DI: page table 0, as far as the server fills it
        mov     si, gdt + SEL_VCPI      ; DS:SI: its three descriptors
        mov     ax, 0xDE01
        int     0x67
        pop     es
        test    ah, ah
        jz      .got
        mov     dx, err_vcpi
        jmp     die
.got:   mov     [vcpi_pm_entry], ebx
        sub     di, vcpi_pt
        shr     di, 2
        mov     [vcpi_n], di            ; entries it filled

        ; PT0: identity, the server's entries over it (ring 0 only above 1 MB + 64 KB)
        mov     di, buffer
        mov     eax, 0x003
        mov     cx, 1024
.id:    mov     [di], eax
        add     di, 4
        add     eax, 0x1000
        loop    .id
        xor     bx, bx
.cp:    cmp     bx, [vcpi_n]
        jae     .cpd
        cmp     bx, 1024
        jae     .cpd
        mov     si, bx
        shl     si, 2
        mov     eax, [vcpi_pt + si]
        cmp     bx, 0x110
        jb      .low
        and     al, 0xFB                ; no PTE_U: V86 code can't, DPMI clients mustn't
.low:   mov     [buffer + si], eax
        inc     bx
        jmp     .cp
.cpd:   mov     eax, [pt0_phys]
        call    vcpi_put_page

        ; the page directory
        call    clear_page
        mov     eax, [pt0_phys]
        or      eax, 0x003
        mov     [buffer], eax
        mov     edx, [arena_virt]
        shr     edx, 22
        shl     dx, 2
        mov     di, buffer
        add     di, dx
        mov     eax, [pta_phys]
        or      eax, 0x003
        mov     ecx, [n_pt]
.pde:   mov     [di], eax
        add     di, 4
        add     eax, 0x1000
        loop    .pde
        mov     eax, [pd_phys]
        or      eax, 0x003
        mov     [buffer + 1023 * 4], eax
        mov     eax, [pd_phys]
        call    vcpi_put_page

        ; the arena's page tables
        xor     edx, edx                ; table number
        mov     ebx, [arena_phys]
        or      ebx, 0x003
        mov     esi, [arena_pages]
.pt:    call    clear_page
        mov     di, buffer
        mov     cx, 1024
.pte:   test    esi, esi
        jz      .ptd
        mov     [di], ebx
        add     ebx, 0x1000
        dec     esi
        add     di, 4
        loop    .pte
.ptd:   mov     eax, edx
        shl     eax, 12
        add     eax, [pta_phys]
        push    ebx
        push    esi
        push    edx
        call    vcpi_put_page
        pop     edx
        pop     esi
        pop     ebx
        inc     edx
        cmp     edx, [n_pt]
        jb      .pt
        ret

clear_page:
        push    di
        push    cx
        push    eax
        mov     di, buffer
        xor     eax, eax
        mov     cx, 1024
        rep     stosd
        pop     eax
        pop     cx
        pop     di
        ret

; XMS-move the 4 KB in `buffer` to physical address EAX in the arena.
vcpi_put_page:
        sub     eax, [arena_phys]
        add     eax, [arena_skew]
        mov     [mv_dst_off], eax
        mov     dword [mv_len], 0x1000
        mov     word [mv_src_handle], 0
        mov     word [mv_src_off], buffer
        mov     [mv_src_off + 2], ds
        mov     dx, [xms_handle]
        mov     [mv_dst_handle], dx
        mov     si, xms_move
        mov     ah, 0x0B
        call    far [xms_entry]
        cmp     ax, 1
        je      .ok
        mov     dx, err_xms
        jmp     die
.ok:    ret

; CF clear if 486 or better: only a 486 can set EFLAGS.AC (bit 18).
check_486:
        pushfd
        pop     eax
        mov     ecx, eax
        xor     eax, 0x00040000         ; flip AC
        push    eax
        popfd
        pushfd
        pop     eax
        push    ecx                     ; put EFLAGS back
        popfd
        xor     eax, ecx
        test    eax, 0x00040000         ; did it stick?
        jz      .no
        clc
        ret
.no:    stc
        ret

; CF clear if 386 or better. Must use 8086 instructions only.
check_386:
        pushf
        pop     ax
        mov     cx, ax
        and     ax, 0x0FFF              ; try to clear FLAGS bits 12-15
        push    ax
        popf
        pushf
        pop     ax
        and     ax, 0xF000
        cmp     ax, 0xF000              ; 8086/88/186: bits stuck at 1
        je      .no
        mov     ax, cx
        or      ax, 0x7000              ; try to set IOPL/NT
        push    ax
        popf
        pushf
        pop     ax
        test    ax, 0x7000              ; 286 in real mode: stuck at 0
        jz      .no
        push    cx
        popf
        clc
        ret
.no:    push    cx
        popf
        stc
        ret

; Scan the PSP command tail for switches.
parse_cmdline:
        mov     si, 0x81
        mov     cl, [0x80]
        xor     ch, ch
.next:  jcxz    .done
        lodsb
        dec     cx
        cmp     al, '/'
        jne     .next
        jcxz    .done
        lodsb
        dec     cx
        and     al, 0xDF                ; upper case
        cmp     al, 'D'
        jne     .not_d
        or      dword [bi_flags], BI_FLAG_DEBUG
        jmp     .next
.not_d: cmp     al, 'T'
        jne     .next
        or      dword [bi_flags], BI_FLAG_TEST
        jmp     .next
.done:  ret

; kpath = directory of this program + "VMM.BIN". The program's own path
; follows the environment block (DOS 3.0+).
find_kernel_path:
        push    es
        mov     es, [0x2C]
        xor     di, di
.scan:  cmp     word [es:di], 0
        je      .found
        inc     di
        cmp     di, 0x7FF0
        jb      .scan
        jmp     .fallback
.found: cmp     word [es:di+2], 1       ; count of strings that follow
        jne     .fallback
        lea     si, [di+4]
        mov     di, kpath
        mov     bx, kpath               ; where the file name goes
.copy:  mov     al, [es:si]
        inc     si
        mov     [di], al
        inc     di
        test    al, al
        jz      .append
        cmp     di, kpath + KPATH_MAX - 9
        jae     .fallback
        cmp     al, '\'
        je      .sep
        cmp     al, ':'
        jne     .copy
.sep:   mov     bx, di
        jmp     .copy
.fallback:
        mov     bx, kpath
.append:
        pop     es
        mov     si, kname
        mov     di, bx
        mov     cx, kname_len
        rep     movsb
        ret

; Read VMM.BIN in chunks and copy each one into the arena with XMS move.
load_kernel:
        mov     dx, msg_load
        call    print
        mov     si, kpath
        call    print_z
        mov     dx, msg_crlf
        call    print

        mov     dx, kpath
        mov     ax, 0x3D00
        int     0x21
        jnc     .opened
        mov     dx, err_open
        jmp     die
.opened:
        mov     [fhandle], ax
.read:  mov     bx, [fhandle]
        mov     cx, CHUNK
        mov     dx, buffer
        mov     ah, 0x3F
        int     0x21
        jnc     .ok
        mov     dx, err_read
        jmp     die
.ok:    test    ax, ax
        jz      .eof
        movzx   eax, ax
        cmp     dword [loaded], 0
        jne     .body
        call    check_header
.body:  mov     ecx, [loaded]
        add     ecx, eax
        cmp     ecx, [arena_size]
        jbe     .fits
        mov     dx, err_big
        jmp     die
.fits:  mov     ecx, eax
        inc     ecx
        and     ecx, ~1                 ; XMS moves must be even
        mov     [mv_len], ecx
        mov     word [mv_src_handle], 0 ; conventional memory, seg:off
        mov     word [mv_src_off], buffer
        mov     [mv_src_off+2], ds
        mov     dx, [xms_handle]
        mov     [mv_dst_handle], dx
        mov     edx, [arena_skew]
        add     edx, [loaded]
        mov     [mv_dst_off], edx
        add     [loaded], eax
        mov     si, xms_move
        mov     ah, 0x0B
        call    far [xms_entry]
        cmp     ax, 1
        je      .read
        mov     dx, err_xms
        jmp     die
.eof:   mov     bx, [fhandle]
        mov     ah, 0x3E
        int     0x21
        mov     eax, [loaded]
        cmp     eax, [k_filesize]
        je      .done
        mov     dx, err_badk
        jmp     die
.done:  ret

; Validate the kernel header at the start of `buffer` (EAX preserved).
check_header:
        cmp     ax, KH_MEMSIZE + 4
        jb      .bad
        cmp     dword [buffer + KH_MAGIC], KHDR_MAGIC
        jne     .bad
        cmp     dword [buffer + KH_VERSION], 1
        jne     .bad
        mov     edx, [buffer + KH_VBASE]
        test    edx, 0x003FFFFF         ; must be 4 MB aligned ...
        jnz     .bad
        cmp     edx, 0x00400000         ; ... and above the identity map
        jb      .bad
        mov     [arena_virt], edx
        mov     edx, [buffer + KH_ENTRY]
        mov     [k_entry], edx
        mov     edx, [buffer + KH_FILESIZE]
        mov     [k_filesize], edx
        mov     edx, [buffer + KH_MEMSIZE]
        cmp     edx, [k_filesize]
        jb      .bad
        mov     [k_memsize], edx
        ret
.bad:   mov     dx, err_badk
        jmp     die

; Arena layout: [kernel image+bss][page dir][PT0][arena PTs][free...]
layout_memory:
        mov     eax, [k_memsize]
        add     eax, 0xFFF
        and     eax, 0xFFFFF000
        mov     [kernel_size], eax
        mov     ebx, [arena_phys]
        add     ebx, eax
        mov     [pd_phys], ebx
        add     ebx, 0x1000
        mov     [pt0_phys], ebx
        add     ebx, 0x1000
        mov     [pta_phys], ebx

        mov     ecx, [arena_size]
        shr     ecx, 12
        mov     [arena_pages], ecx
        add     ecx, 1023
        shr     ecx, 10
        mov     [n_pt], ecx

        mov     edx, [arena_virt]       ; arena must fit below PDE 1023
        shr     edx, 22
        add     edx, ecx
        cmp     edx, 1023
        ja      .toobig

        lea     edx, [ecx + 2]
        shl     edx, 12
        mov     [tables_bytes], edx
        add     eax, edx                ; kernel + tables
        mov     [free_offset], eax
        add     eax, MIN_FREE
        cmp     eax, [arena_size]
        ja      .toobig
        ret
.toobig:
        mov     dx, err_big
        jmp     die

; BX = selector: set that descriptor's base to EAX (EAX preserved).
set_gdt_base:
        push    eax
        mov     [gdt + bx + 2], ax
        shr     eax, 16
        mov     [gdt + bx + 4], al
        mov     [gdt + bx + 7], ah
        pop     eax
        ret

; Put both 8259s back the way the BIOS had them, then restore DOS's masks.
restore_pic:
        mov     al, 0x11
        out     0x20, al
        IODELAY
        out     0xA0, al
        IODELAY
        mov     al, 0x08                ; master vectors 08h-0Fh
        out     0x21, al
        IODELAY
        mov     al, 0x70                ; slave vectors 70h-77h
        out     0xA1, al
        IODELAY
        mov     al, 0x04
        out     0x21, al
        IODELAY
        mov     al, 0x02
        out     0xA1, al
        IODELAY
        mov     al, 0x01
        out     0x21, al
        IODELAY
        out     0xA1, al
        IODELAY
        mov     al, [pic1_mask]
        out     0x21, al
        mov     al, [pic2_mask]
        out     0xA1, al
        ret

; Unlock and free the XMS block, release A20. Safe to call more than once.
xms_release:
        mov     dx, [xms_handle]
        test    dx, dx
        jz      .done
        mov     ah, 0x0D                ; unlock
        call    far [xms_entry]
        mov     dx, [xms_handle]
        mov     ah, 0x0A                ; free
        call    far [xms_entry]
        mov     ah, 0x04                ; global disable A20
        call    far [xms_entry]
        mov     word [xms_handle], 0
.done:  ret

; Print DX ('$'-terminated), clean up, exit with ERRORLEVEL 1.
die:
        call    print
        call    xms_release
        mov     ax, 0x4C01
        int     0x21

print:                                  ; DX -> '$'-terminated string
        mov     ah, 0x09
        int     0x21
        ret

print_z:                                ; SI -> ASCIIZ string
        lodsb
        test    al, al
        jz      .done
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        jmp     print_z
.done:  ret

print_hex:                              ; EAX = value, CX = digits (1-8)
        pushad
        mov     ebx, eax
        mov     ax, 8
        sub     ax, cx
        shl     ax, 2
        push    cx
        mov     cl, al
        rol     ebx, cl
        pop     cx
.digit: rol     ebx, 4
        mov     dl, bl
        and     dl, 0x0F
        add     dl, '0'
        cmp     dl, '9'
        jbe     .out
        add     dl, 'A' - '9' - 1
.out:   mov     ah, 0x02
        int     0x21
        loop    .digit
        popad
        ret

print_dec:                              ; EAX = unsigned value
        pushad
        mov     ebx, 10
        xor     cx, cx
.div:   xor     edx, edx
        div     ebx
        push    dx
        inc     cx
        test    eax, eax
        jnz     .div
.emit:  pop     dx
        add     dl, '0'
        mov     ah, 0x02
        int     0x21
        loop    .emit
        popad
        ret

; -----------------------------------------------------------------------------
; Data
; -----------------------------------------------------------------------------
msg_banner      db  'OS1993 Loader  Version 0.1', 13, 10, '$'
msg_xms1        db  'XMS: allocated $'
msg_xms2        db  ' KB, arena at physical $'
msg_load        db  'Loading $'
msg_go          db  'Starting the Virtual Machine Manager...', 13, 10, '$'
msg_vcpi        db  'A memory manager is running: it hands over through VCPI.', 13, 10, '$'
msg_back        db  'OS1993 has ended. Exit code $'
msg_crlf        db  13, 10, '$'

err_cpu         db  'OS1993 requires an 80486 or better processor.', 13, 10, '$'
err_cpu486      db  'OS1993 requires an 80486 or better processor (this is a 386).', 13, 10, '$'
err_v86         db  'The processor is in virtual 8086 mode, and the memory manager that', 13, 10
                db  'put it there offers no VCPI. Remove it from CONFIG.SYS (or let it', 13, 10
                db  'offer VCPI) and try again.', 13, 10, '$'
err_vcpi        db  'The VCPI server refused the switch to protected mode.', 13, 10, '$'
err_noxms       db  'No XMS driver found. Add DEVICE=HIMEM.SYS to CONFIG.SYS.', 13, 10, '$'
err_nomem       db  'Not enough extended memory (at least 1024 KB free is required).', 13, 10, '$'
err_xms         db  'XMS driver error.', 13, 10, '$'
err_open        db  'Cannot open VMM.BIN.', 13, 10, '$'
err_read        db  'Error reading VMM.BIN.', 13, 10, '$'
err_badk        db  'VMM.BIN is not a valid OS1993 kernel image.', 13, 10, '$'
err_big         db  'VMM.BIN does not fit in the available extended memory.', 13, 10, '$'

kname           db  'VMM.BIN', 0
kname_len       equ $ - kname

; ---- boot information block: layout shared with kernel/include/bootinfo.h ----
        align   4
bootinfo:
bi_magic        dd  BI_MAGIC            ;  0
bi_flags        dd  0                   ;  4
arena_phys      dd  0                   ;  8
arena_size      dd  0                   ; 12
arena_virt      dd  0                   ; 16
kernel_size     dd  0                   ; 20
pd_phys         dd  0                   ; 24
free_offset     dd  0                   ; 28
loader_base     dd  0                   ; 32
bi_return_off   dd  pm32_return         ; 36
bi_return_sel   dw  SEL_CODE32          ; 40
                dw  0, 0                ; 42
gdtr            dw  gdt_end - gdt - 1   ; 46
gdtr_base       dd  0                   ; 48
bi_xms_kb       dd  0                   ; 52
bi_v86_stub     dw  v86_stub            ; 56 V86 breakpoint (ARPL) offset
bi_v86_stack    dw  v86_stack_top       ; 58 V86 stack top offset
bi_kpath        dw  kpath               ; 60 ASCIIZ path of VMM.BIN
bi_buf_off      dw  buffer              ; 62 transfer buffer offset ...
bi_buf_size     dw  CHUNK               ; 64 ... and size
pic1_mask       db  0                   ; 66 DOS's IRQ masks
pic2_mask       db  0                   ; 67
bootinfo_end:
%if bootinfo_end - bootinfo != 68
%error "bootinfo layout drifted from bootinfo.h"
%endif

; ---- loader GDT ----
        align   8
gdt:    dq      0
        dw      0xFFFF, 0               ; 0x08 code32, loader based
        db      0, 0x9A, 0xCF, 0
        dw      0xFFFF, 0               ; 0x10 data32, loader based
        db      0, 0x92, 0xCF, 0
        dw      0xFFFF, 0               ; 0x18 flat code32
        db      0, 0x9A, 0xCF, 0
        dw      0xFFFF, 0               ; 0x20 flat data32
        db      0, 0x92, 0xCF, 0
        dw      0xFFFF, 0               ; 0x28 code16, 64K, loader based
        db      0, 0x9A, 0x00, 0
        dw      0xFFFF, 0               ; 0x30 data16, 64K, loader based
        db      0, 0x92, 0x00, 0
        dw      103, 0                  ; 0x38 TSS (VCPI wants one in TR)
        db      0, 0x89, 0x00, 0
        dq      0, 0, 0                 ; 0x40-0x50 the VCPI server's (DE01h)
gdt_end:

; ---- VCPI: the switch to protected mode (DE0Ch), its tables ----
        align   4
vcpi_switch:
vs_cr3          dd  0
vs_gdtr         dd  0                   ; linear address of the GDTR image
vs_idtr         dd  0                   ; ... and of an IDTR (empty: interrupts stay off)
                dw  0                   ; LDTR
                dw  SEL_TSS             ; TR
                dd  pm32_vcpi_entry     ; EIP
                dw  SEL_CODE32          ; CS
vcpi_idtr       dw  0
                dd  0
vcpi_pm_entry   dd  0                   ; the server's protected-mode entry (DE01h) ...
                dw  SEL_VCPI            ; ... in its code segment
vcpi_mode       db  0
vcpi_n          dw  0

; Returning from a V86 call: ARPL is invalid in real/V86 mode, so it raises
; #UD and the kernel's V86 monitor regains control (the Win386 technique).
; The first ends kernel-initiated calls, the second catches returns from DOS
; (for the DOS critical section).
v86_stub        db  0x63, 0xC0          ; arpl ax, ax
                db  0x63, 0xC0          ; arpl ax, ax

rm_idtr         dw  0x03FF
                dd  0

; ---- XMS move structure (function 0Bh) ----
        align   4
xms_move:
mv_len          dd  0
mv_src_handle   dw  0
mv_src_off      dd  0
mv_dst_handle   dw  0
mv_dst_off      dd  0

; ---- uninitialised data ----
        section .bss
        alignb  4
bss_start:
xms_entry       resd 1
xms_phys        resd 1
arena_skew      resd 1
loaded          resd 1
k_entry         resd 1
k_filesize      resd 1
k_memsize       resd 1
pt0_phys        resd 1
pta_phys        resd 1
arena_pages     resd 1
n_pt            resd 1
tables_bytes    resd 1
xms_kb          resw 1
xms_handle      resw 1
fhandle         resw 1
rm_ss           resw 1
rm_sp           resw 1
exit_code       resb 1
KPATH_MAX       equ 128
kpath           resb KPATH_MAX
        alignb  16
buffer          resb CHUNK + 2
        alignb  16
v86_stack       resb 2048               ; stack for kernel-initiated V86 calls
v86_stack_top:
loader_stack    resb 1024
loader_stack_top:
        alignb  16
vcpi_pt         resb 4096               ; VCPI DE01h: the server's page table 0
loader_tss      resb 104
vcpi_stack      resb 256
vcpi_stack_top:
bss_end:
