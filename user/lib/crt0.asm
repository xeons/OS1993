; =============================================================================
; user/lib/crt0.asm -- program entry point (the header is header.asm)
; =============================================================================
        cpu     386
        bits    32

        extern  main

        section .text

; Entered in ring 3 with EBX -> struct user_info, every import already
; linked. The loader zero-fills every page, so .bss is already clear.
        global  _start
_start:
        push    ebx
        call    main
        mov     ebx, eax                ; SYS_EXIT with main's result
        xor     eax, eax
        int     0x30
.hang:  jmp     .hang
