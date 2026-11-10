; =============================================================================
; user/lib/crt0.asm -- program entry point (the header is header.asm)
; =============================================================================
        cpu     386
        bits    32

        extern  main, __crt_start

        section .text

; Entered in ring 3 with EBX -> struct user_info, every import already
; linked. The loader zero-fills every page, so .bss is already clear.
; The C runtime splits the arguments, calls main(argc, argv) and exit()s
; with its result.
        global  _start
_start:
        push    ebx
        push    main
        call    __crt_start
.hang:  jmp     .hang
