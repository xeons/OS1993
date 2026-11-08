; =============================================================================
; kernel/switch.asm -- kernel thread context switch
; =============================================================================
        cpu     386
        bits    32

        section .text

; void switch_context(uint32_t *save_esp, uint32_t next_esp)
; Saves the callee-saved registers and EFLAGS on the current stack, stores
; ESP in *save_esp, then resumes the thread whose stack is at next_esp.
; A new thread's stack is laid out by thread_create() to match.
        global  switch_context
switch_context:
        mov     eax, [esp + 4]
        mov     edx, [esp + 8]
        push    ebp
        push    ebx
        push    esi
        push    edi
        pushfd
        mov     [eax], esp
        mov     esp, edx
        popfd
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret
