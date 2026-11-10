; =============================================================================
; kernel/v86entry.asm -- switching between the kernel and virtual 8086 mode
;
; v86_enter() saves the kernel's callee-saved state, points TSS.ESP0 just
; below it, and IRETs into V86 mode. Traps out of V86 mode then build their
; frames below the saved state. When the monitor decides the V86 call is
; over (or the VM crashed), v86_leave() discards those frames and returns
; from v86_enter() as if from a normal call -- a setjmp/longjmp pair.
; =============================================================================
        cpu     486
        bits    32

%include "selectors.inc"

        extern  kernel_tss

; struct v86_frame offsets (kernel/include/v86.h)
F_EAX   equ 0
F_EBX   equ 4
F_ECX   equ 8
F_EDX   equ 12
F_ESI   equ 16
F_EDI   equ 20
F_EBP   equ 24
F_EIP   equ 28
F_CS    equ 32
F_EFL   equ 36
F_ESP   equ 40
F_SS    equ 44
F_ES    equ 48
F_DS    equ 52
F_FS    equ 56
F_GS    equ 60

        section .text

; int v86_enter(const struct v86_frame *f, uint32_t *save_esp)
        global  v86_enter
v86_enter:
        push    ebp
        push    ebx
        push    esi
        push    edi
        pushfd
        cli
        mov     edx, [esp + 28]
        mov     [edx], esp              ; this VM's saved kernel state
        mov     [kernel_tss + 4], esp   ; TSS.ESP0: traps from V86 land here
        mov     eax, [esp + 24]
        push    dword [eax + F_GS]      ; V86 IRET frame
        push    dword [eax + F_FS]
        push    dword [eax + F_DS]
        push    dword [eax + F_ES]
        push    dword [eax + F_SS]
        push    dword [eax + F_ESP]
        push    dword [eax + F_EFL]
        push    dword [eax + F_CS]
        push    dword [eax + F_EIP]
        mov     ebx, [eax + F_EBX]
        mov     ecx, [eax + F_ECX]
        mov     edx, [eax + F_EDX]
        mov     esi, [eax + F_ESI]
        mov     edi, [eax + F_EDI]
        mov     ebp, [eax + F_EBP]
        mov     eax, [eax + F_EAX]
        iretd

; void v86_leave(int rc, uint32_t saved_esp) -- never returns; the matching
; v86_enter() returns rc instead
        global  v86_leave
v86_leave:
        mov     eax, [esp + 4]
        mov     esp, [esp + 8]
        mov     cx, KDATA_SEL
        mov     ds, cx
        mov     es, cx
        mov     fs, cx
        mov     gs, cx
        popfd
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret
