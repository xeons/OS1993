; =============================================================================
; kernel/entry.asm -- VMM.BIN header, entry point and low-level helpers
; =============================================================================
        cpu     486
        bits    32

%include "selectors.inc"

        extern  kmain
        extern  __image_start, __image_size, __kernel_size
        extern  __bss_start, __bss_end

; -----------------------------------------------------------------------------
; Kernel header. Must be the first bytes of VMM.BIN (see linker.ld) and must
; match what loader/os1993.asm checks.
; -----------------------------------------------------------------------------
        section .header progbits alloc exec nowrite align=4
        dd      0x334D4D56              ; magic "VMM3"
        dd      1                       ; header version
        dd      __image_start           ; virtual base (4 MB aligned)
        dd      _start                  ; entry point
        dd      __image_size            ; bytes in this file
        dd      __kernel_size           ; bytes needed including .bss

        section .text

; Entered from the loader: paging on, flat segments, interrupts off,
; EBX = linear address of struct bootinfo. ESP is not usable yet.
        global  _start
_start:
        cli
        push    dword 0x00000002        ; EFLAGS: clear IOPL and NT inherited
        popfd                           ; from DOS (NT would turn IRET into a
                                        ; task return)
        cld
        mov     edx, ebx                ; keep bootinfo pointer
        mov     edi, __bss_start        ; zero .bss (includes our stack)
        mov     ecx, __bss_end
        sub     ecx, edi
        shr     ecx, 2
        xor     eax, eax
        rep     stosd
        mov     esp, boot_stack_top
        xor     ebp, ebp                ; terminate stack traces
        push    edx
        call    kmain
.halt:  cli
        hlt
        jmp     .halt

; void exit_to_dos(const struct bootinfo *bi, uint8_t code)
; Restore the loader's GDT and jump to its return stub (identity mapped).
        global  exit_to_dos
exit_to_dos:
        cli
        mov     ebx, [esp + 4]
        movzx   eax, byte [esp + 8]
        lgdt    [ebx + 46]              ; bootinfo.gdtr_limit
        jmp     far [ebx + 36]          ; bootinfo.return_off:return_sel

; void gdt_load(const struct dtr *gdtr)
        global  gdt_load
gdt_load:
        mov     eax, [esp + 4]
        lgdt    [eax]
        mov     ax, KDATA_SEL
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        jmp     KCODE_SEL:.reload
.reload:
        ret

; void idt_load(const struct dtr *idtr)
        global  idt_load
idt_load:
        mov     eax, [esp + 4]
        lidt    [eax]
        ret

        section .bss
        alignb  16
boot_stack:
        resb    16384
boot_stack_top:

        section .text

; void user_enter(uint32_t eip, uint32_t esp, uint32_t ebx) -- never returns
; IRET to ring 3: flat user segments, interrupts on, IOPL 0 (port access
; is decided by the TSS I/O bitmap), EBX = argument for the program.
        global  user_enter
user_enter:
        mov     ecx, [esp + 4]          ; eip
        mov     edx, [esp + 8]          ; esp
        mov     ebx, [esp + 12]         ; argument
        mov     ax, UDATA_SEL
        mov     ds, ax
        mov     es, ax
        mov     gs, ax
        mov     ax, UTLS_SEL            ; FS:0 -> the thread's TIB (the scheduler set it)
        mov     fs, ax
        push    dword UDATA_SEL         ; ss
        push    edx                     ; esp
        push    dword 0x00000202        ; eflags: IF
        push    dword UCODE_SEL         ; cs
        push    ecx                     ; eip
        xor     eax, eax
        xor     ecx, ecx
        xor     edx, edx
        xor     esi, esi
        xor     edi, edi
        xor     ebp, ebp
        iretd
