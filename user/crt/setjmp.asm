; =============================================================================
; user/crt/setjmp.asm -- setjmp and longjmp.
;
; jmp_buf: EBX ESI EDI EBP ESP EIP. EBP, ESP and EIP are stored XORed with
; this module's stack canary (a random value per process), so an overrun
; into a jmp_buf can't choose where longjmp goes (as glibc's pointer
; mangling).
; =============================================================================
        cpu     486
        bits    32

        extern  __stack_chk_guard

        section .text

        global  setjmp
setjmp:
        mov     edx, [esp + 4]          ; env
        mov     ecx, [__stack_chk_guard]
        mov     [edx + 0], ebx
        mov     [edx + 4], esi
        mov     [edx + 8], edi
        mov     eax, ebp
        xor     eax, ecx
        mov     [edx + 12], eax
        lea     eax, [esp + 4]          ; the caller's ESP after we return
        xor     eax, ecx
        mov     [edx + 16], eax
        mov     eax, [esp]              ; return address
        xor     eax, ecx
        mov     [edx + 20], eax
        xor     eax, eax
        ret

        global  longjmp
longjmp:
        mov     edx, [esp + 4]          ; env
        mov     eax, [esp + 8]          ; value (0 becomes 1)
        test    eax, eax
        jnz     .nonzero
        inc     eax
.nonzero:
        mov     ecx, [__stack_chk_guard]
        mov     ebx, [edx + 0]
        mov     esi, [edx + 4]
        mov     edi, [edx + 8]
        mov     ebp, [edx + 12]
        xor     ebp, ecx
        mov     esp, [edx + 16]
        xor     esp, ecx
        mov     edx, [edx + 20]
        xor     edx, ecx
        jmp     edx
