; =============================================================================
; user/crt0.asm -- GUI.BIN header and ring-3 entry point
; =============================================================================
        cpu     386
        bits    32

        extern  main
        extern  __image_size, __mem_size

; Image header, checked by the VMM's loader (kernel/user.c)
        section .header progbits alloc exec nowrite align=4
        dd      0x33495547              ; "GUI3"
        dd      _start
        dd      __image_size
        dd      __mem_size

        section .text

; Entered in ring 3 with EBX -> struct user_info. The loader zero-fills
; every page, so .bss is already clear.
        global  _start
_start:
        push    ebx
        call    main
        mov     ebx, eax                ; SYS_EXIT with main's result
        xor     eax, eax
        int     0x30
.hang:  jmp     .hang
