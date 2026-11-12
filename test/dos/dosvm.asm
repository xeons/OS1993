; dosvm.asm -- a DOS program that exercises the virtual devices of the DOS
; session it runs in. Its argument picks the test; PASS/FAIL lines go to
; standard output (redirect them to a file). tools/qemu-dosvm-test.py runs
; it in DOS sessions and reads the files.
;
;   DOSVM I    polls INT 16h for a key in a tight loop (idle detection)
;   DOSVM P    reprograms the timer to 1 kHz, counts IRQ 0s for an RTC second
;   DOSVM C n  opens COMn: its UART answers, or it belongs to someone else
;   DOSVM L    writes to LPT1's data port
;   DOSVM M    the INT 33h mouse: reset, position, buttons, a handler
;   DOSVM X    XMS: version, allocate, move to and from, lock, free
;   DOSVM E    INT 15h AH=87h block move, AH=88h extended memory size
;   DOSVM K    conventional memory: INT 12h and the largest free block
;   DOSVM D    DPMI: enter protected mode, descriptors, memory, back to DOS
;
;   nasm -f bin -o DOSVM.COM dosvm.asm

        org     100h
        cpu     386

start:
        mov     sp, 0FFFEh
        mov     ah, 4Ah                 ; keep 64 KB: the rest is for DOS to hand out
        mov     bx, 1000h
        int     21h
        mov     si, 81h
.skip:  lodsb
        cmp     al, ' '
        je      .skip
        and     al, 0DFh
        cmp     al, 'I'
        je      t_idle
        cmp     al, 'P'
        je      t_pit
        cmp     al, 'C'
        je      t_com
        cmp     al, 'L'
        je      t_lpt
        cmp     al, 'M'
        je      t_mouse
        cmp     al, 'X'
        je      t_xms
        cmp     al, 'E'
        je      t_e15
        cmp     al, 'D'
        je      t_dpmi
        cmp     al, 'K'
        je      t_mem
        mov     dx, usage
        call    puts
        jmp     quit

quit:   mov     ax, 4C00h
        int     21h

; ---- output ---------------------------------------------------------------

puts:                                   ; DS:DX -> ASCIIZ
        push    ax
        push    bx
        push    cx
        push    si
        mov     si, dx
        xor     cx, cx
.len:   cmp     byte [si], 0
        je      .go
        inc     si
        inc     cx
        jmp     .len
.go:    mov     ah, 40h
        mov     bx, 1
        int     21h
        pop     si
        pop     cx
        pop     bx
        pop     ax
        ret

putdec:                                 ; EAX, unsigned
        push    eax
        push    ebx
        push    edx
        push    di
        mov     di, numbuf + 11
        mov     byte [di], 0
        mov     ebx, 10
.d:     xor     edx, edx
        div     ebx
        add     dl, '0'
        dec     di
        mov     [di], dl
        test    eax, eax
        jnz     .d
        mov     dx, di
        call    puts
        pop     di
        pop     edx
        pop     ebx
        pop     eax
        ret

puthex:                                 ; AX, four digits
        push    ax
        push    bx
        push    cx
        push    dx
        mov     cx, 4
        mov     bx, numbuf
.h:     rol     ax, 4
        mov     dl, al
        and     dl, 0Fh
        add     dl, '0'
        cmp     dl, '9'
        jbe     .s
        add     dl, 7
.s:     mov     [bx], dl
        inc     bx
        loop    .h
        mov     byte [bx], 0
        mov     dx, numbuf
        call    puts
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

pass:                                   ; "PASS " + DX + CRLF
        push    dx
        mov     dx, s_pass
        jmp     result
fail:
        push    dx
        mov     dx, s_fail
result: call    puts
        pop     dx
        call    puts
        push    dx
        mov     dx, crlf
        call    puts
        pop     dx
        ret

check:                                  ; CF clear: PASS DX, else FAIL DX
        jc      fail
        jmp     pass

; Wait for the RTC's seconds to change (CMOS register 0).
rtc_sec:
        mov     al, 0
        out     70h, al
        in      al, 71h
        ret
rtc_next:
        call    rtc_sec
        mov     ah, al
.w:     call    rtc_sec
        cmp     al, ah
        je      .w
        ret

; ---- I: idle detection -----------------------------------------------------

t_idle:
        mov     dx, s_idle_go
        call    puts
        xor     ebp, ebp
.poll:  inc     ebp
        mov     ah, 01h
        int     16h
        jz      .poll
        xor     ah, ah
        int     16h
        mov     dx, s_idle_n
        call    puts
        mov     eax, ebp
        call    putdec
        mov     dx, crlf
        call    puts
        mov     dx, s_idle
        clc
        call    check
        jmp     quit

; ---- P: the virtual timer --------------------------------------------------

t_pit:
        mov     ax, 3508h               ; keep INT 8
        int     21h
        mov     [old8], bx
        mov     [old8 + 2], es
        mov     ax, 2508h
        mov     dx, irq0
        int     21h
        cli
        mov     al, 34h                 ; channel 0, lo/hi, mode 2
        out     43h, al
        mov     ax, 1193                ; 1000 Hz
        out     40h, al
        mov     al, ah
        out     40h, al
        sti
        call    rtc_next
        mov     dword [ticks], 0
        call    rtc_next
        mov     ebx, [ticks]            ; IRQ 0s in one RTC second

        mov     al, 0                   ; latch the count twice: it moves, and stays below 1193
        out     43h, al
        in      al, 40h
        mov     cl, al
        in      al, 40h
        mov     ch, al
        mov     dx, 200
.dl:    dec     dx
        jnz     .dl
        mov     al, 0
        out     43h, al
        in      al, 40h
        mov     ah, al
        in      al, 40h
        xchg    al, ah
        mov     [cnt2], ax
        mov     [cnt1], cx

        cli
        mov     al, 36h                 ; back to the BIOS's 18.2 Hz
        out     43h, al
        xor     al, al
        out     40h, al
        out     40h, al
        sti
        push    ds
        lds     dx, [old8]
        mov     ax, 2508h
        int     21h
        pop     ds

        mov     dx, s_pit_n
        call    puts
        mov     eax, ebx
        call    putdec
        mov     dx, crlf
        call    puts
        cmp     ebx, 800
        jb      .bad
        cmp     ebx, 1150
        ja      .bad
        clc
        jmp     .r1
.bad:   stc
.r1:    mov     dx, s_pit_rate
        call    check
        mov     ax, [cnt1]
        call    puthex
        mov     dx, s_space
        call    puts
        mov     ax, [cnt2]
        call    puthex
        mov     dx, crlf
        call    puts
        mov     ax, [cnt1]
        cmp     ax, 1193
        ja      .bad2
        cmp     word [cnt2], 1193
        ja      .bad2
        cmp     ax, [cnt2]
        je      .bad2
        clc
        jmp     .r2
.bad2:  stc
.r2:    mov     dx, s_pit_count
        call    check
        jmp     quit

irq0:   inc     dword [cs:ticks]
        push    ax
        mov     al, 20h
        out     20h, al
        pop     ax
        iret

; ---- C: a serial port ------------------------------------------------------

t_com:
        lodsb                           ; the digit after "C "
        cmp     al, ' '
        je      t_com
        sub     al, '1'
        cmp     al, 3
        ja      .usage
        xor     ah, ah
        mov     bx, ax
        shl     bx, 1
        mov     dx, [comports + bx]
        mov     [combase], dx
        add     al, '1'
        mov     [s_com_n + 3], al
        add     dx, 7                   ; scratch register: write, read back
        mov     al, 5Ah
        out     dx, al
        in      al, dx
        cmp     al, 5Ah
        jne     .none
        mov     al, 0A5h
        out     dx, al
        in      al, dx
        cmp     al, 0A5h
        jne     .none
        mov     dx, s_com_n
        clc
        call    check
                                        ; now its interrupt: THR empty, IER bit 1
        mov     al, [s_com_n + 3]       ; COM1/3: IRQ 4 (INT 0Ch), COM2/4: IRQ 3 (INT 0Bh)
        and     al, 1
        mov     byte [comvec], 0Bh
        mov     byte [commask], 08h
        jz      .v
        mov     byte [comvec], 0Ch
        mov     byte [commask], 10h
.v:     mov     ah, 35h
        mov     al, [comvec]
        int     21h
        mov     [oldcom], bx
        mov     [oldcom + 2], es
        mov     ah, 25h
        mov     al, [comvec]
        mov     dx, comirq
        int     21h
        cli
        in      al, 21h
        mov     ah, [commask]
        not     ah
        and     al, ah
        out     21h, al
        mov     dx, [combase]
        add     dx, 4                   ; MCR: OUT2 (the IRQ line), DTR, RTS
        mov     al, 0Bh
        out     dx, al
        sub     dx, 3                   ; IER: transmitter empty
        mov     al, 02h
        out     dx, al
        sti
        push    es
        xor     ax, ax
        mov     es, ax
        mov     bx, [es:46Ch]
.wt:    cmp     word [comirqs], 0
        jne     .seen
        mov     ax, [es:46Ch]
        sub     ax, bx
        cmp     ax, 10
        jb      .wt
.seen:  pop     es
        cli
        mov     dx, [combase]
        inc     dx
        xor     al, al                  ; IER off
        out     dx, al
        in      al, 21h
        or      al, [commask]
        out     21h, al
        sti
        push    ds
        mov     ah, 25h
        mov     al, [comvec]
        lds     dx, [oldcom]
        int     21h
        pop     ds
        mov     dx, s_com_irq
        cmp     word [comirqs], 0
        je      .noirq
        clc
        call    check
        jmp     quit
.noirq: stc
        call    check
        jmp     quit
.none:  mov     dx, s_com_n
        stc
        call    check
        jmp     quit
.usage: mov     dx, usage
        call    puts
        jmp     quit

comirq: push    ax
        push    dx
        inc     word [cs:comirqs]
        mov     dx, [cs:combase]
        add     dx, 2                   ; IIR: acknowledges THR empty
        in      al, dx
        dec     dx
        xor     al, al                  ; one is enough
        out     dx, al
        mov     al, 20h
        out     20h, al
        pop     dx
        pop     ax
        iret

; ---- L: the printer port ---------------------------------------------------

t_lpt:
        mov     dx, 378h
        mov     al, 55h
        out     dx, al
        in      al, dx
        cmp     al, 55h
        mov     dx, s_lpt
        je      .ok
        stc
        call    check
        jmp     quit
.ok:    clc
        call    check
        jmp     quit

; ---- M: the mouse ----------------------------------------------------------

t_mouse:
        xor     ax, ax                  ; reset: AX=FFFFh, BX=buttons
        int     33h
        cmp     ax, 0FFFFh
        mov     dx, s_m_reset
        je      .r1
        stc                             ; no mouse: nothing more to try
        call    check
        jmp     quit
.r1:    clc
        call    check
        mov     ax, 4                   ; put the pointer at 100,48
        mov     cx, 100
        mov     dx, 48
        int     33h
        mov     ax, 3
        int     33h
        cmp     cx, 100
        jne     .f2
        cmp     dx, 48
        jne     .f2
        clc
        jmp     .r2
.f2:    stc
.r2:    mov     dx, s_m_pos
        call    check
        mov     ax, 7                   ; range: 0..639 x 0..199 (text mode)
        mov     cx, 0
        mov     dx, 639
        int     33h
        mov     ax, 0Ch                 ; a handler for moves and the left button
        mov     cx, 07h
        push    cs
        pop     es
        mov     dx, mhandler
        int     33h
        mov     ax, 1                   ; show it
        int     33h
        mov     dx, s_m_wait
        call    puts
        xor     ebp, ebp
.wait:  cmp     byte [mclick], 0        ; the test clicks: the handler sees it
        jne     .got
        mov     ah, 1
        int     16h
        jz      .wait
        xor     ah, ah
        int     16h
        mov     dx, s_m_click
        stc
        call    check
        jmp     .done
.got:   mov     dx, s_m_xy
        call    puts
        movzx   eax, word [mx]
        call    putdec
        mov     dx, s_comma
        call    puts
        movzx   eax, word [my]
        call    putdec
        mov     dx, crlf
        call    puts
        mov     dx, s_m_click
        clc
        call    check
        mov     ax, 5                   ; press count for the left button
        xor     bx, bx
        int     33h
        cmp     bx, 1
        jb      .f3
        clc
        jmp     .r3
.f3:    stc
.r3:    mov     dx, s_m_press
        call    check
.done:  mov     ax, 2
        int     33h
        xor     ax, ax                  ; reset (drops the handler)
        int     33h
        jmp     quit

mhandler:                               ; AX events, BX buttons, CX x, DX y
        test    ax, 2                   ; left button down
        jz      .r
        mov     [cs:mx], cx
        mov     [cs:my], dx
        mov     byte [cs:mclick], 1
.r:     retf

; ---- X: XMS ----------------------------------------------------------------

t_xms:
        mov     ax, 4300h
        int     2Fh
        cmp     al, 80h
        mov     dx, s_x_there
        je      .there
        stc
        call    check
        jmp     quit
.there: clc
        call    check
        mov     ax, 4310h
        int     2Fh
        mov     [xms], bx
        mov     [xms + 2], es
        xor     ah, ah                  ; version
        call    far [xms]
        cmp     ax, 0200h
        mov     dx, s_x_ver
        jb      .f1
        clc
        jmp     .r1
.f1:    stc
.r1:    call    check
        mov     ah, 08h                 ; free memory
        call    far [xms]
        mov     [xfree], ax
        mov     dx, s_x_free
        call    puts
        movzx   eax, ax
        call    putdec
        mov     dx, s_kb
        call    puts
        mov     ah, 09h                 ; allocate 64 KB
        mov     dx, 64
        call    far [xms]
        mov     [xh], dx
        cmp     ax, 1
        mov     dx, s_x_alloc
        je      .ok2
        stc
        call    check
        jmp     quit
.ok2:   clc
        call    check
        mov     di, pattern             ; 1 KB of a pattern, out and back
        xor     cx, cx
.fill:  mov     al, cl
        xor     al, 5Ah
        mov     [di], al
        inc     di
        inc     cx
        cmp     cx, 1024
        jb      .fill
        mov     dword [emm + 0], 1024   ; length
        mov     word [emm + 4], 0       ; from conventional memory
        mov     word [emm + 6], pattern
        mov     [emm + 8], ds
        mov     ax, [xh]
        mov     [emm + 10], ax          ; to the block, at 30000
        mov     dword [emm + 12], 30000
        mov     si, emm
        mov     ah, 0Bh
        call    far [xms]
        cmp     ax, 1
        jne     .mf
        mov     ax, [xh]                ; and back, to another buffer
        mov     [emm + 4], ax
        mov     dword [emm + 6], 30000
        mov     word [emm + 10], 0
        mov     word [emm + 12], back
        mov     [emm + 14], ds
        mov     si, emm
        mov     ah, 0Bh
        call    far [xms]
        cmp     ax, 1
        jne     .mf
        mov     si, pattern
        mov     di, back
        mov     cx, 1024
        push    ds
        pop     es
        repe    cmpsb
        jne     .mf
        clc
        jmp     .mr
.mf:    stc
.mr:    mov     dx, s_x_move
        call    check
        mov     ah, 0Ch                 ; lock: a 32-bit address
        mov     dx, [xh]
        call    far [xms]
        mov     [xaddr], bx
        mov     [xaddr + 2], dx
        cmp     ax, 1
        mov     dx, s_x_lock
        jne     .lf
        clc
        jmp     .lr
.lf:    stc
.lr:    call    check
        jc      .nolock
                                        ; INT 15h AH=87h into the locked block at +4096
        xor     eax, eax
        mov     ax, ds
        shl     eax, 4
        add     eax, pattern
        mov     [gdt + 10h + 2], ax
        shr     eax, 16
        mov     [gdt + 10h + 4], al
        mov     [gdt + 10h + 7], ah
        mov     word [gdt + 10h], 0FFFFh
        mov     byte [gdt + 10h + 5], 93h
        mov     eax, [xaddr]
        add     eax, 4096
        mov     [gdt + 18h + 2], ax
        shr     eax, 16
        mov     [gdt + 18h + 4], al
        mov     [gdt + 18h + 7], ah
        mov     word [gdt + 18h], 0FFFFh
        mov     byte [gdt + 18h + 5], 93h
        push    ds
        pop     es
        mov     si, gdt
        mov     cx, 512                 ; 1 KB
        mov     ah, 87h
        int     15h
        jc      .e87f
        mov     di, back                ; clear, then XMS-move it back
        mov     cx, 1024
        xor     al, al
        rep     stosb
        mov     dword [emm + 0], 1024
        mov     ax, [xh]
        mov     [emm + 4], ax
        mov     dword [emm + 6], 4096
        mov     word [emm + 10], 0
        mov     word [emm + 12], back
        mov     [emm + 14], ds
        mov     si, emm
        mov     ah, 0Bh
        call    far [xms]
        cmp     ax, 1
        jne     .e87f
        mov     si, pattern
        mov     di, back
        mov     cx, 1024
        repe    cmpsb
        jne     .e87f
        clc
        jmp     .e87r
.e87f:  stc
.e87r:  mov     dx, s_x_87
        call    check
.nolock:
        mov     ah, 0Dh
        mov     dx, [xh]
        call    far [xms]
        mov     ah, 0Ah                 ; free
        mov     dx, [xh]
        call    far [xms]
        cmp     ax, 1
        mov     dx, s_x_freeb
        jne     .ff
        clc
        jmp     .fr
.ff:    stc
.fr:    call    check
        mov     ah, 07h                 ; A20: on
        call    far [xms]
        cmp     ax, 1
        mov     dx, s_x_a20
        jne     .af
        clc
        jmp     .ar
.af:    stc
.ar:    call    check
        jmp     quit

; ---- E: INT 15h ------------------------------------------------------------

t_e15:
        mov     ah, 88h
        int     15h
        mov     dx, s_e_size
        call    puts
        movzx   eax, ax
        call    putdec
        mov     dx, s_kb
        call    puts
        mov     di, pattern
        xor     cx, cx
.fill:  mov     al, cl
        xor     al, 0A7h
        mov     [di], al
        inc     di
        inc     cx
        cmp     cx, 512
        jb      .fill
        xor     eax, eax                ; source: our pattern, as a linear address
        mov     ax, ds
        shl     eax, 4
        push    eax
        add     eax, pattern
        mov     [gdt + 10h + 2], ax
        shr     eax, 16
        mov     [gdt + 10h + 4], al
        mov     word [gdt + 10h], 0FFFFh
        mov     byte [gdt + 10h + 5], 93h
        pop     eax                     ; target: the other buffer
        add     eax, back
        mov     [gdt + 18h + 2], ax
        shr     eax, 16
        mov     [gdt + 18h + 4], al
        mov     word [gdt + 18h], 0FFFFh
        mov     byte [gdt + 18h + 5], 93h
        push    ds
        pop     es
        mov     si, gdt
        mov     cx, 256                 ; words
        mov     ah, 87h
        int     15h
        jc      .bad
        mov     si, pattern
        mov     di, back
        mov     cx, 512
        repe    cmpsb
        jne     .bad
        clc
        jmp     .r
.bad:   stc
.r:     mov     dx, s_e_move
        call    check
        jmp     quit

; ---- K: conventional memory -------------------------------------------------

t_mem:
        int     12h
        mov     dx, s_k_bios
        call    puts
        movzx   eax, ax
        call    putdec
        mov     dx, s_kb
        call    puts
        mov     ah, 48h                 ; the largest free block
        mov     bx, 0FFFFh
        int     21h
        mov     dx, s_k_free
        call    puts
        movzx   eax, bx
        shr     eax, 6
        call    putdec
        mov     dx, s_kb
        call    puts
        jmp     quit

; ---- D: DPMI ---------------------------------------------------------------

t_dpmi:
        mov     ax, 1687h               ; is there a DPMI host?
        int     2Fh
        test    ax, ax
        mov     dx, s_d_host
        jz      .there
        stc
        call    check
        jmp     quit
.there: test    bx, 1                   ; 32-bit programs supported
        jnz     .b32
        stc
        call    check
        jmp     quit
.b32:   mov     [dpmi_cpu], cl
        clc
        call    check
        mov     [dpmi_entry], di
        mov     [dpmi_entry + 2], es
        mov     [dpmi_paras], si
        mov     dx, s_d_ver
        call    puts
        movzx   eax, byte [dpmi_cpu]
        call    putdec
        mov     dx, crlf
        call    puts
        mov     bx, [dpmi_paras]        ; the host's private data
        test    bx, bx
        jz      .nomem
        mov     ah, 48h
        int     21h
        jc      quit
        mov     es, ax
.nomem: mov     ax, 1                   ; a 32-bit client
        call    far [dpmi_entry]
        jnc     .inpm
        mov     dx, s_d_enter
        stc
        call    check
        jmp     quit

        ; ---- protected mode, 16-bit code segment (our CS, limit 64 KB) ----
.inpm:  mov     [pm_ds], ds
        mov     dx, s_d_enter
        clc
        call    check               ; INT 21h AH=40h, reflected to DOS
        mov     ax, cs
        lar     cx, ax
        mov     dx, s_d_cs
        jnz     .f_cs
        test    ch, 08h             ; a code segment
        jz      .f_cs
        clc
        jmp     .r_cs
.f_cs:  stc
.r_cs:  call    check

        mov     ax, 0400h           ; version
        int     31h
        cmp     ax, 005Ah           ; 0.90
        mov     dx, s_d_0400
        jb      .f_v
        clc
        jmp     .r_v
.f_v:   stc
.r_v:   call    check

        xor     ax, ax              ; allocate a descriptor
        mov     cx, 1
        int     31h
        jc      .f_desc
        mov     [sel], ax
        mov     bx, ax
        mov     ax, 0007h           ; base: B8000h (the text screen)
        mov     cx, 000Bh
        mov     dx, 8000h
        int     31h
        jc      .f_desc
        mov     ax, 0008h           ; limit: 4000 bytes
        xor     cx, cx
        mov     dx, 3999
        int     31h
        jc      .f_desc
        mov     es, [sel]
        mov     word [es:3990], 2F44h   ; a 'D' in the last row
        mov     ax, [es:3990]
        cmp     ax, 2F44h
        jne     .f_desc
        clc
        jmp     .r_desc
.f_desc:
        stc
.r_desc:
        mov     dx, s_d_desc
        push    ds
        pop     es
        call    check

        mov     ax, 0501h           ; 256 KB of memory, through a 4 GB selector
        mov     bx, 4
        xor     cx, cx
        int     31h
        jc      .f_mem
        mov     [mem_lin], cx
        mov     [mem_lin + 2], bx
        mov     [mem_h], di
        mov     [mem_h + 2], si
        mov     bx, [sel]
        mov     ax, 0007h
        mov     cx, [mem_lin + 2]
        mov     dx, [mem_lin]
        int     31h
        mov     ax, 0008h
        mov     cx, 0003h           ; 256 KB - 1
        mov     dx, 0FFFFh
        int     31h
        jc      .f_mem
        mov     es, [sel]
        mov     edi, 0              ; fill all of it, through a 32-bit address
        mov     ecx, 65536
        mov     eax, 12345678h
.fl:    mov     [es:edi], eax
        add     edi, 4
        add     eax, 3
        dec     ecx
        jnz     .fl
        mov     edi, 0
        mov     ecx, 65536
        mov     eax, 12345678h
.ck:    cmp     [es:edi], eax
        jne     .f_mem
        add     edi, 4
        add     eax, 3
        dec     ecx
        jnz     .ck
        push    ds
        pop     es
        mov     ax, 0502h           ; free it
        mov     di, [mem_h]
        mov     si, [mem_h + 2]
        int     31h
        jc      .f_mem
        clc
        jmp     .r_mem
.f_mem: push    ds
        pop     es
        stc
.r_mem: mov     dx, s_d_mem
        call    check

        mov     ax, 0300h           ; a real-mode INT 21h AH=2Ah (date) via 0300h
        mov     bl, 21h
        xor     bh, bh
        xor     cx, cx
        mov     edi, rmregs
        push    ds
        pop     es
        mov     byte [rmregs + 1Dh], 2Ah    ; AH
        int     31h
        jc      .f_rm
        cmp     word [rmregs + 18h], 1980   ; CX = year
        jb      .f_rm
        clc
        jmp     .r_rm
.f_rm:  stc
.r_rm:  mov     dx, s_d_0300
        call    check

        mov     ax, 0204h           ; protected-mode INT 21h vector
        mov     bl, 21h
        int     31h
        mov     dx, s_d_0204
        jc      .f_pv
        test    cx, cx
        jz      .f_pv
        clc
        jmp     .r_pv
.f_pv:  stc
.r_pv:  call    check

        mov     ax, 0003h           ; the selector increment
        int     31h
        mov     dx, s_d_inc
        cmp     ax, 8
        jne     .f_inc
        clc
        jmp     .r_inc
.f_inc: stc
.r_inc: call    check

        mov     ax, 0001h           ; free the descriptor
        mov     bx, [sel]
        int     31h
        mov     dx, s_d_free
        call    check
        mov     dx, s_d_done
        call    puts
        mov     ax, 4C00h           ; ends the client (and the program)
        int     21h

; ---- data -------------------------------------------------------------------

usage   db      "DOSVM I|P|C n|L|M|X|E|K|D", 13, 10, 0
s_pass  db      "PASS ", 0
s_fail  db      "FAIL ", 0
crlf    db      13, 10, 0
s_space db      " ", 0
s_comma db      ",", 0
s_kb    db      " KB", 13, 10, 0
s_idle_go db    "polling INT 16h for a key", 13, 10, 0
s_idle_n db     "polls: ", 0
s_idle  db      "the key arrived while polling", 0
s_pit_n db      "IRQ 0s in a second at 1 kHz: ", 0
s_pit_rate db   "the timer runs at the rate the VM set", 0
s_pit_count db  "the count latched from port 40h moves within the VM's period", 0
s_com_n db      "COM? answers", 0
s_com_irq db    "its interrupt reaches this VM", 0
s_lpt   db      "LPT1's data port holds a byte", 0
s_m_reset db    "INT 33h reset: a mouse", 0
s_m_pos db      "INT 33h 4/3: set and read the position", 0
s_m_wait db     "click in the session", 13, 10, 0
s_m_xy  db      "clicked at ", 0
s_m_click db    "the event handler saw the left button", 0
s_m_press db    "INT 33h 5: press count", 0
s_x_there db    "XMS driver present", 0
s_x_ver db      "XMS version 2 or later", 0
s_x_free db     "XMS free: ", 0
s_x_alloc db    "XMS allocate 64 KB", 0
s_x_move db     "XMS move to the block and back", 0
s_x_lock db     "XMS lock", 0
s_x_87  db      "INT 15h AH=87h into the locked block, XMS move back out", 0
s_x_freeb db    "XMS free the block", 0
s_x_a20 db      "XMS A20 on", 0
s_k_bios db     "INT 12h: ", 0
s_k_free db     "largest free block: ", 0
s_e_size db     "INT 15h AH=88h: ", 0
s_e_move db     "INT 15h AH=87h: 512 bytes moved, no mode switch", 0
s_d_host db     "DPMI host for 32-bit programs", 0
s_d_ver db      "DPMI processor: ", 0
s_d_enter db    "entered protected mode", 0
s_d_cs  db      "CS is a code descriptor", 0
s_d_0400 db     "DPMI 0.9 or later", 0
s_d_desc db     "a descriptor of our own onto the screen", 0
s_d_mem db      "256 KB allocated, written and read through a 32-bit offset, freed", 0
s_d_0300 db     "0300h: a real-mode INT 21h", 0
s_d_0204 db     "0204h: the protected-mode INT 21h vector", 0
s_d_inc db      "0003h: selector increment 8", 0
s_d_free db     "0001h: descriptor freed", 0
s_d_done db     "DPMI done", 13, 10, 0
comports dw     3F8h, 2F8h, 3E8h, 2E8h

        align   4
ticks   dd      0
old8    dd      0
cnt1    dw      0
cnt2    dw      0
xms     dd      0
xh      dw      0
xaddr   dd      0
xfree   dw      0
mx      dw      0
my      dw      0
mclick  db      0
combase dw      0
comirqs dw      0
oldcom  dd      0
comvec  db      0
commask db      0
        align   4
dpmi_entry dd   0
dpmi_paras dw   0
dpmi_cpu db     0
pm_ds   dw      0
sel     dw      0
mem_lin dd      0
mem_h   dd      0
emm     times 16 db 0
gdt     times 48 db 0
rmregs  times 50 db 0
numbuf  times 12 db 0
pattern times 1024 db 0
back    times 1024 db 0
