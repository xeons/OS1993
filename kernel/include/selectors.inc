; Kernel GDT selectors -- keep in sync with kernel/include/gdt.h
KCODE_SEL       equ     0x08
KDATA_SEL       equ     0x10
TSS_SEL         equ     0x18
UCODE_SEL       equ     0x23
UDATA_SEL       equ     0x2B
UTLS_SEL        equ     0x33            ; ring 3 data at the thread's TIB (FS)
