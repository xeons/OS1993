; lfntest.asm -- a DOS program that uses Windows 95's long-name calls
; (INT 21h AX=71xxh) and DOS's share modes and record locks on the VMM's
; drives (D: is FAT32). Writes PASS/FAIL lines to E:\LFNOUT.TXT and ends.
; tools/qemu-fat-test.py runs it in a DOS session and reads the file.
;
;   nasm -f bin -o LFNTEST.COM lfntest.asm

        org     100h
        cpu     386

%macro  check 1                         ; CF clear: PASS, else FAIL (AX shown)
        mov     si, %1
        call    report
%endmacro

start:
        mov     ah, 3Ch                 ; the report file (8.3, on E:)
        xor     cx, cx
        mov     dx, outname
        int     21h
        jc      quit
        mov     [outh], ax

        ; 71A0: D: has the long-name calls; C: hasn't
        mov     ax, 71A0h
        mov     dx, root_d
        mov     di, fsname
        mov     cx, 32
        int     21h
        jc      .a0
        test    bx, 4000h
        jnz     .a0
        stc
.a0:    check   t_a0d
        mov     ax, 71A0h               ; C: too: the VMM took DOS's boot drive
        mov     dx, root_c
        mov     di, fsname
        mov     cx, 32
        int     21h
        jc      .a0c
        test    bx, 4000h
        jnz     .a0c
        stc
.a0c:   check   t_a0c

        ; 716C: create a file with a long name, write to it
        mov     ax, 716Ch
        mov     bx, 2                   ; read/write
        xor     cx, cx
        mov     dx, 12h                 ; replace, or create
        mov     si, longname
        int     21h
        mov     [fh], ax                ; (check uses AX and BX)
        check   t_create
        jc      .nofile
        mov     bx, [fh]
        mov     ah, 40h
        mov     cx, hello_len
        mov     dx, hello
        int     21h
        mov     ah, 3Eh
        int     21h
.nofile:
        ; 7139 mkdir, 7156 rename into it
        mov     ax, 7139h
        mov     dx, longdir
        int     21h
        check   t_mkdir
        mov     ax, 7156h
        mov     dx, longname
        mov     di, renamed
        int     21h
        check   t_rename

        ; 713B chdir into it, 7147 the long current directory
        mov     ax, 713Bh
        mov     dx, longdir
        int     21h
        check   t_chdir
        mov     ax, 7147h
        mov     dl, 4                   ; D:
        mov     si, buf
        int     21h
        jc      .cwd
        mov     si, buf
        mov     di, want_cwd
        call    strcmp
.cwd:   check   t_cwd

        ; 716C open the renamed file by a relative long name; read it
        mov     ah, 19h                 ; (relative to the current drive: D: for now)
        int     21h
        mov     [olddrv], al
        mov     ah, 0Eh
        mov     dl, 3
        int     21h
        mov     ax, 716Ch
        xor     bx, bx
        xor     cx, cx
        mov     dx, 1                   ; open
        mov     si, relname
        int     21h
        jc      .rd
        mov     bx, ax
        mov     ah, 3Fh
        mov     cx, 64
        mov     dx, buf
        int     21h
        pushf
        push    ax
        mov     ah, 3Eh
        int     21h
        pop     ax
        popf
        jc      .rd
        cmp     ax, hello_len
        jne     .rdbad
        mov     si, buf
        mov     di, hello
        mov     cx, hello_len
        repe    cmpsb
        je      .rd
.rdbad: stc
.rd:    check   t_read

        ; 714E/714F find *.txt here: the long name comes back
        mov     ax, 714Eh
        mov     cx, 0010h               ; directories allowed too
        mov     si, 1                   ; DOS times
        mov     dx, star_txt
        mov     di, finddata
        int     21h
        jc      .fd
        mov     [fh], ax
        mov     si, finddata + 2Ch
        mov     di, renamed_name
        call    strcmp
        pushf
        mov     ax, 71A1h
        mov     bx, [fh]
        int     21h
        popf
.fd:    check   t_find
        mov     ah, 0Eh
        mov     dl, [olddrv]
        int     21h

        ; 7160 the short path, and back to the long one
        mov     ax, 7160h
        mov     cx, 1
        mov     si, longdir
        mov     di, buf
        int     21h
        jc      .tn
        mov     ax, 7160h
        mov     cx, 2
        mov     si, buf
        mov     di, buf2
        int     21h
        jc      .tn
        mov     si, buf2
        mov     di, longdir
        call    strcmp
.tn:    check   t_truename

        ; 7143: attributes
        mov     ax, 7143h
        xor     bl, bl
        mov     dx, renamed
        int     21h
        jc      .at
        test    cl, 20h                 ; archive
        jnz     .at
        stc
.at:    check   t_attr

        ; share modes and locks on D:\HELLO.TXT
        mov     ax, 3D42h               ; read/write, deny none
        mov     dx, hellotxt
        int     21h
        jc      .sh
        mov     [h1], ax
        mov     ax, 3D42h
        mov     dx, hellotxt
        int     21h
        jc      .sh
        mov     [h2], ax
        mov     ax, 3D12h               ; deny all: refused, others have it open
        mov     dx, hellotxt
        int     21h
        jnc     .shbad
        cmp     ax, 32
        je      .sh
.shbad: stc
.sh:    check   t_share

        mov     ax, 5C00h               ; lock bytes 0-9 through the first handle
        mov     bx, [h1]
        xor     cx, cx
        xor     dx, dx
        xor     si, si
        mov     di, 10
        int     21h
        check   t_lock
        mov     ax, 4200h               ; the second can't read them
        mov     bx, [h2]
        xor     cx, cx
        xor     dx, dx
        int     21h
        mov     ah, 3Fh
        mov     cx, 4
        mov     dx, buf
        int     21h
        jnc     .lvbad
        cmp     ax, 33
        je      .lv
.lvbad: stc
.lv:    check   t_lockviol
        mov     ax, 5C01h               ; unlock: now it can
        mov     bx, [h1]
        xor     cx, cx
        xor     dx, dx
        xor     si, si
        mov     di, 10
        int     21h
        jc      .ul
        mov     ax, 4200h
        mov     bx, [h2]
        xor     cx, cx
        xor     dx, dx
        int     21h
        mov     ah, 3Fh
        mov     cx, 4
        mov     dx, buf
        int     21h
.ul:    check   t_unlock
        mov     ah, 3Eh
        mov     bx, [h1]
        int     21h
        mov     ah, 3Eh
        mov     bx, [h2]
        int     21h

        ; clean up: 713B to the root, 7141 delete, 713A rmdir
        mov     ax, 713Bh
        mov     dx, root_d
        int     21h
        mov     ax, 7141h
        xor     si, si
        mov     dx, renamed
        int     21h
        check   t_delete
        mov     ax, 713Ah
        mov     dx, longdir
        int     21h
        check   t_rmdir

        ; leave one behind for the GUI test to find
        mov     ax, 716Ch
        mov     bx, 1
        xor     cx, cx
        mov     dx, 12h
        mov     si, leftname
        int     21h
        jc      .done
        mov     bx, ax
        mov     ah, 40h
        mov     cx, hello_len
        mov     dx, hello
        int     21h
        mov     ah, 3Eh
        int     21h
.done:
        mov     ah, 40h
        mov     bx, [outh]
        mov     cx, done_len
        mov     dx, done_msg
        int     21h
        mov     ah, 3Eh
        mov     bx, [outh]
        int     21h
quit:   mov     ax, 4C00h
        int     21h

; report: CF -> "PASS name" or "FAIL name (AX)"; SI = name
report:
        pushf
        push    ax
        mov     dx, pass
        jnc     .w
        mov     dx, fail
.w:     push    si
        mov     ah, 40h
        mov     bx, [outh]
        mov     cx, 5
        int     21h
        pop     si
        mov     dx, si
        xor     cx, cx
.len:   cmp     byte [si], 0
        je      .gotlen
        inc     si
        inc     cx
        jmp     .len
.gotlen:
        mov     ah, 40h
        int     21h
        pop     ax
        popf
        jnc     .nl
        mov     di, hexbuf + 2          ; " (XXXX)"
        mov     cx, 4
.hex:   rol     ax, 4
        push    ax
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .dig
        add     al, 7
.dig:   mov     [di], al
        inc     di
        pop     ax
        loop    .hex
        mov     ah, 40h
        mov     cx, 7
        mov     dx, hexbuf
        int     21h
.nl:    mov     ah, 40h
        mov     bx, [outh]
        mov     cx, 2
        mov     dx, crlf
        int     21h
        ret

; strcmp SI, DI (any case): CF clear if equal
strcmp:
.l:     mov     al, [si]
        mov     ah, [di]
        cmp     al, 'a'
        jb      .u1
        cmp     al, 'z'
        ja      .u1
        sub     al, 32
.u1:    cmp     ah, 'a'
        jb      .u2
        cmp     ah, 'z'
        ja      .u2
        sub     ah, 32
.u2:    cmp     al, ah
        jne     .ne
        test    al, al
        jz      .eq
        inc     si
        inc     di
        jmp     .l
.eq:    clc
        ret
.ne:    stc
        ret

outname     db  'E:\LFNOUT.TXT', 0
root_d      db  'D:\', 0
root_c      db  'C:\', 0
longname    db  'D:\Long name from DOS.txt', 0
longdir     db  'D:\A DOS long dir', 0
renamed     db  'D:\A DOS long dir\Renamed by DOS.txt', 0
renamed_name db 'Renamed by DOS.txt', 0
relname     db  'Renamed by DOS.txt', 0
want_cwd    db  'A DOS long dir', 0
star_txt    db  '*.txt', 0
hellotxt    db  'D:\HELLO.TXT', 0
leftname    db  'D:\Written by a DOS program with LFN.txt', 0
hello       db  'hello lfn', 13, 10
hello_len   equ $ - hello
pass        db  'PASS '
fail        db  'FAIL '
crlf        db  13, 10
hexbuf      db  ' (0000)'
done_msg    db  'DONE', 13, 10
done_len    equ $ - done_msg
t_a0d       db  '71A0 D: has long names', 0
t_a0c       db  '71A0 C: has them too', 0
t_create    db  '716C create a long name', 0
t_mkdir     db  '7139 mkdir', 0
t_rename    db  '7156 rename into it', 0
t_chdir     db  '713B chdir', 0
t_cwd       db  '7147 long current directory', 0
t_read      db  '716C open relative, read', 0
t_find      db  '714E find returns the long name', 0
t_truename  db  '7160 short and long paths', 0
t_attr      db  '7143 attributes', 0
t_share     db  'deny all refused while open', 0
t_lock      db  '5C00 lock', 0
t_lockviol  db  'read of locked bytes refused', 0
t_unlock    db  '5C01 unlock, then read', 0
t_delete    db  '7141 delete', 0
t_rmdir     db  '713A rmdir', 0

outh        dw  0
h1          dw  0
h2          dw  0
fh          dw  0
olddrv      db  0
fsname      times 32 db 0
buf         times 270 db 0
buf2        times 270 db 0
finddata    times 320 db 0
