; =============================================================================
; user/lib/header.asm -- the MOD3 header at the start of every module.
; Assembled once per module with -DKIND=1 (program) or 2 (library) and
; -DMODNAME='"NAME.EXT"'. Layout: struct mod_header in kernel/include/user.h.
; =============================================================================
        cpu     386
        bits    32

        extern  __image_start, __image_size, __mem_size, __exports, __imports, __data_start, __guard
%if KIND == 1
        extern  _start
%endif

        section .header progbits alloc exec nowrite align=4
        dd      0x33444F4D              ; "MOD3"
        dd      KIND
        dd      __image_start           ; linked and loaded here
%if KIND == 1
        dd      _start
%else
        dd      0
%endif
        dd      __image_size
        dd      __mem_size
        dd      __exports
        dd      __imports
name:   db      MODNAME
        times   16 - ($ - name) db 0
        dd      __data_start            ; writable (per process) from here
        dd      __guard                 ; -> __stack_chk_guard, or 0
