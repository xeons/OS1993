; =============================================================================
; kernel/isr.asm -- interrupt and exception entry stubs
;
; Vectors 00h-1Fh: CPU exceptions.  20h-2Fh: hardware IRQ 0-15 (PIC remapped).
; 30h: system calls from ring 3.
; Every stub builds a struct regs (kernel/include/interrupts.h) and calls
; interrupt_dispatch(struct regs *).
; =============================================================================
        cpu     486
        bits    32

%include "selectors.inc"

        extern  interrupt_dispatch

        section .text

%assign i 0
%rep 49
isr_stub_%[i]:
%if (i == 8) || (i == 10) || (i == 11) || (i == 12) || (i == 13) || (i == 14) || (i == 17)
        ; CPU already pushed an error code
%else
        push    dword 0
%endif
        push    dword i
        jmp     isr_common
%assign i i+1
%endrep

isr_common:
        pushad
        push    ds
        push    es
        push    fs
        push    gs
        mov     ax, KDATA_SEL
        mov     ds, ax
        mov     es, ax
        cld
        push    esp                     ; struct regs *
        call    interrupt_dispatch
        add     esp, 4
        pop     gs
        pop     fs
        pop     es
        pop     ds
        popad
        add     esp, 8                  ; vector number + error code
        iretd

        section .rodata
        global  isr_stub_table
isr_stub_table:
%assign i 0
%rep 49
        dd      isr_stub_%[i]
%assign i i+1
%endrep
