# OS1993 -- build with modern tools, run on a 1993 machine.
#
#   make            build the loader, the kernel, and the ring-3 modules:
#                   build/OS1993.COM, VMM.BIN, the DLLs and the programs (*.BIN)
#   make run        boot it in DOSBox-X (386, 16 MB, HIMEM-style XMS)
#   make test       headless DOSBox-X run; checks the full round trip to DOS
#   make check486   verify nothing contains post-486 or FPU instructions
#   make qemu       boot FreeDOS 1.3 in QEMU with OS1993 on C: (real DOS)
#   make qemu-test  the same, headless: run OS1993 /T and check the results
#   make qemu-emm386  OS1993 must refuse to run with JEMM386 loaded
#   make qemu-sessions  type into two concurrent DOS sessions (QEMU sendkey)
#   make qemu-vga   check the VGA driver with QEMU screendumps
#   make qemu-wm    drive the window manager and every control (mouse, keys)
#   make qemu-absmouse  absolute pointer via QEMU's VMware port (exact position)
#   make qemu-menu  menu bar, system menu and context menus (mouse, keys)
#   make qemu-controls  combo boxes, the tab control and up-downs (mouse, keys)
#   make qemu-msgbox  keyboard accelerators and message boxes
#   make qemu-shell the shell and its programs, each a process
#   make qemu-files file access: Paint's Open/Save, the file dialogs, the disk
#   make qemu-notepad  Notepad and the multi-line edit control, the clipboard
#   make qemu-tasklist Ctrl+Alt+Del: Close Program, and the VMM's screen for a hung GUI
#   make qemu-vesa     the VBE display driver: higher resolutions and colour depths
#   make qemu-crt      the C runtime (CRT.DLL, libcrt.a) and its protections
#   make clean

CC      := i686-elf-gcc
OBJCOPY := i686-elf-objcopy
OBJDUMP := i686-elf-objdump
AR      := i686-elf-ar
NASM    := nasm
DOSBOX  := dosbox-x
QEMU    := qemu-system-i386

BUILD   := build

CFLAGS  := -std=gnu11 -march=i486 -mtune=i486 -mgeneral-regs-only \
           -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables -fno-delete-null-pointer-checks \
           -fno-strict-aliasing --param=min-pagesize=0 -O2 -g -Wall -Wextra \
           -Ikernel/include -MMD -MP
# No -lgcc on purpose: the cross libgcc is built for i686 (CMOV etc.).
# If the link ever fails on __udivdi3 & co., don't do 64-bit math.
LDFLAGS := -T kernel/linker.ld -nostdlib -ffreestanding \
           -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-Map,$(BUILD)/vmm.map
NASMFLAGS_K := -f elf32 -g -Ikernel/include/

# ---- ring 3: MOD3 modules (kernel/include/user.h) -------------------------
# Each DLL is described by a .DEF file; tools/mkdef.py turns it into the
# DLL's export table and an import library (JMP [slot] thunks) for callers.
MAKEFLAGS   += --no-builtin-rules
.SUFFIXES:
U           := $(BUILD)/user
MKDEF       := python3 tools/mkdef.py
# Every ring-3 module is hardened: stack canaries (seeded per process by the
# loader) and _FORTIFY_SOURCE's checked copies; see user/crt/include/crtdefs.h.
USER_CFLAGS := $(filter-out -Ikernel/include,$(CFLAGS)) -Iuser/crt/include -Iuser/lib -Iuser/gfx -Iuser/ui -Iuser/shell \
               -Ikernel/include -fstack-protector-strong -D_FORTIFY_SOURCE=2 -MMD -MP
MOD_LDFLAGS := -T user/lib/module.ld -nostdlib -ffreestanding \
               -Wl,--build-id=none -Wl,--no-warn-rwx-segments
KRNL_BASE   := $(shell $(MKDEF) user/krnl/krnl.def base)
GFX_BASE    := $(shell $(MKDEF) user/gfx/gfx.def base)
UI_BASE     := $(shell $(MKDEF) user/ui/ui.def base)
SHELL_BASE  := $(shell $(MKDEF) user/shell/shell.def base)
CRT_BASE    := $(shell $(MKDEF) user/crt/crt.def base)

# The C runtime, from one set of sources: CRT.DLL for programs to share,
# libcrt.a to link into a module of its own (the system DLLs, CRTSTAT.BIN).
# ssp.o (the module's own stack canary) goes into every module.
CRT_SRC   := string safe strerror fortify printf malloc stdlib ctype errno exit time stdio
CRT_CODE  := $(patsubst %,$(U)/crt/%.o,$(CRT_SRC)) $(U)/crt/setjmp.o
LIBCRT    := $(U)/libcrt.a
SSP       := $(U)/crt/ssp.o
CRT_OBJ   := $(U)/hdr/CRT.DLL.o $(CRT_CODE) $(SSP) $(U)/gen/crt_exports.o $(U)/gen/krnl_imports.o

# DLLs: header, code, export table, import libraries of what they call
KRNL_OBJ  := $(U)/hdr/KERNEL.DLL.o $(U)/krnl/krnl.o $(U)/gen/krnl_exports.o $(SSP) $(LIBCRT)
GFX_OBJ   := $(U)/hdr/GFX.DLL.o $(U)/gfx/gfx.o $(U)/gen/gfx_exports.o $(SSP) $(LIBCRT)
UI_SRC    := wnd queue input frame paint display menu accel msgbox dialog button static edit medit listbox \
             scroll trackbar combo tab updown kbd theme
UI_OBJ    := $(U)/hdr/UI.DLL.o $(patsubst %,$(U)/ui/%.o,$(UI_SRC)) \
             $(U)/gen/ui_exports.o $(U)/gen/gfx_imports.o $(U)/gen/krnl_imports.o $(SSP) $(LIBCRT)
SHELL_OBJ := $(U)/hdr/SHELL.DLL.o $(U)/shell/shell.o $(U)/shell/icons.o $(U)/shell/filedlg.o $(U)/shell/findrepl.o $(U)/gen/shell_exports.o \
             $(U)/gen/ui_imports.o $(U)/gen/krnl_imports.o $(SSP) $(LIBCRT)

# Programs: user/apps/<name>.c, each a process of its own, sharing CRT.DLL.
# CRTSTAT.BIN is CRTTEST.BIN with the C runtime linked in (libcrt.a).
APPS      := SHELL DEMO CALC PAINT CLOCK NOTEPAD HANG CRTTEST
APP_LIBS  := $(U)/lib/crt0.o $(U)/gen/krnl_imports.o $(U)/gen/ui_imports.o \
             $(U)/gen/shell_imports.o $(U)/gen/crt_imports.o $(SSP)
app_obj    = $(U)/hdr/$(1).BIN.o $(U)/apps/$(shell echo $(1) | tr A-Z a-z).o $(APP_LIBS)
CRTSTAT_OBJ := $(U)/hdr/CRTSTAT.BIN.o $(U)/apps/crttest.o $(U)/lib/crt0.o $(U)/gen/krnl_imports.o \
               $(U)/gen/ui_imports.o $(U)/gen/shell_imports.o $(SSP) $(LIBCRT)

USER_OBJ  := $(sort $(filter %.o,$(KRNL_OBJ) $(GFX_OBJ) $(UI_OBJ) $(SHELL_OBJ) $(CRT_OBJ) $(CRTSTAT_OBJ) \
                    $(foreach a,$(APPS),$(call app_obj,$(a)))))
MODULES   := $(BUILD)/KERNEL.DLL $(BUILD)/GFX.DLL $(BUILD)/UI.DLL $(BUILD)/SHELL.DLL $(BUILD)/CRT.DLL \
             $(patsubst %,$(BUILD)/%.BIN,$(APPS)) $(BUILD)/CRTSTAT.BIN
MODULE_ELFS := $(patsubst $(BUILD)/%,$(U)/%.elf,$(MODULES))

KERNEL_C   := $(wildcard kernel/*.c)
KERNEL_ASM := $(wildcard kernel/*.asm)
KERNEL_OBJ := $(patsubst kernel/%.asm,$(BUILD)/kernel/%.o,$(KERNEL_ASM)) \
              $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(KERNEL_C))

.PHONY: all run test check486 qemu qemu-test qemu-emm386 qemu-sessions qemu-vga qemu-wm qemu-absmouse qemu-menu qemu-controls qemu-msgbox qemu-shell qemu-files qemu-notepad qemu-tasklist qemu-vesa qemu-crt clean

all: $(BUILD)/OS1993.COM $(BUILD)/VMM.BIN $(MODULES)

$(BUILD)/OS1993.COM: loader/os1993.asm | $(BUILD)
	$(NASM) -f bin -o $@ -l $(BUILD)/os1993.lst $<

$(BUILD)/VMM.BIN: $(BUILD)/vmm.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/vmm.elf: $(KERNEL_OBJ) kernel/linker.ld
	$(CC) $(LDFLAGS) -o $@ $(KERNEL_OBJ)

$(BUILD)/kernel/%.o: kernel/%.c | $(BUILD)/kernel
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/kernel/%.o: kernel/%.asm kernel/include/selectors.inc | $(BUILD)/kernel
	$(NASM) $(NASMFLAGS_K) -o $@ $<

$(BUILD)/%.DLL: $(U)/%.DLL.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/%.BIN: $(U)/%.BIN.elf
	$(OBJCOPY) -O binary $< $@

$(U)/KERNEL.DLL.elf: $(KRNL_OBJ) user/lib/module.ld
	$(CC) $(MOD_LDFLAGS) -Wl,--defsym=MODBASE=$(KRNL_BASE) -o $@ $(KRNL_OBJ)

$(U)/GFX.DLL.elf: $(GFX_OBJ) user/lib/module.ld
	$(CC) $(MOD_LDFLAGS) -Wl,--defsym=MODBASE=$(GFX_BASE) -o $@ $(GFX_OBJ)

$(U)/UI.DLL.elf: $(UI_OBJ) user/lib/module.ld
	$(CC) $(MOD_LDFLAGS) -Wl,--defsym=MODBASE=$(UI_BASE) -o $@ $(UI_OBJ)

$(U)/SHELL.DLL.elf: $(SHELL_OBJ) user/lib/module.ld
	$(CC) $(MOD_LDFLAGS) -Wl,--defsym=MODBASE=$(SHELL_BASE) -o $@ $(SHELL_OBJ)

$(U)/CRT.DLL.elf: $(CRT_OBJ) user/lib/module.ld
	$(CC) $(MOD_LDFLAGS) -Wl,--defsym=MODBASE=$(CRT_BASE) -o $@ $(CRT_OBJ)

$(U)/CRTSTAT.BIN.elf: $(CRTSTAT_OBJ) user/lib/module.ld
	$(CC) $(MOD_LDFLAGS) -Wl,--defsym=MODBASE=0x00400000 -o $@ $(CRTSTAT_OBJ)

$(LIBCRT): $(CRT_CODE)
	@rm -f $@
	$(AR) rcs $@ $^

# The canary's own code can't be protected by it.
$(SSP): user/crt/ssp.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -fno-stack-protector -c -o $@ $<

$(U)/crt/setjmp.o: user/crt/setjmp.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 -g -o $@ $<

define APP_RULE
$(U)/$(1).BIN.elf: $(call app_obj,$(1)) user/lib/module.ld
	$$(CC) $$(MOD_LDFLAGS) -Wl,--defsym=MODBASE=0x00400000 -o $$@ $(call app_obj,$(1))
endef
$(foreach a,$(APPS),$(eval $(call APP_RULE,$(a))))

$(U)/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c -o $@ $<

$(U)/lib/crt0.o: user/lib/crt0.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 -g -o $@ $<

# One header per module: kind 1 = program, 2 = library
$(U)/hdr/%.o: user/lib/header.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 -DKIND=$(if $(filter %.DLL,$*),2,1) -DMODNAME='"$*"' -o $@ $<

$(U)/gen/krnl_exports.asm $(U)/gen/krnl_imports.asm: user/krnl/krnl.def tools/mkdef.py
	@mkdir -p $(dir $@)
	$(MKDEF) $< exports $(U)/gen/krnl_exports.asm
	$(MKDEF) $< imports $(U)/gen/krnl_imports.asm

$(U)/gen/gfx_exports.asm $(U)/gen/gfx_imports.asm: user/gfx/gfx.def tools/mkdef.py
	@mkdir -p $(dir $@)
	$(MKDEF) $< exports $(U)/gen/gfx_exports.asm
	$(MKDEF) $< imports $(U)/gen/gfx_imports.asm

$(U)/gen/ui_exports.asm $(U)/gen/ui_imports.asm: user/ui/ui.def tools/mkdef.py
	@mkdir -p $(dir $@)
	$(MKDEF) $< exports $(U)/gen/ui_exports.asm
	$(MKDEF) $< imports $(U)/gen/ui_imports.asm

$(U)/gen/shell_exports.asm $(U)/gen/shell_imports.asm: user/shell/shell.def tools/mkdef.py
	@mkdir -p $(dir $@)
	$(MKDEF) $< exports $(U)/gen/shell_exports.asm
	$(MKDEF) $< imports $(U)/gen/shell_imports.asm

$(U)/gen/crt_exports.asm $(U)/gen/crt_imports.asm: user/crt/crt.def tools/mkdef.py
	@mkdir -p $(dir $@)
	$(MKDEF) $< exports $(U)/gen/crt_exports.asm
	$(MKDEF) $< imports $(U)/gen/crt_imports.asm

$(U)/gen/%.o: $(U)/gen/%.asm
	$(NASM) -f elf32 -o $@ $<

$(BUILD) $(BUILD)/kernel:
	mkdir -p $@

DOSBOX_ARGS := -conf test/dosbox-x.conf -fastlaunch \
               -c "mount c \"$(CURDIR)/$(BUILD)\"" -c "c:"

run: all
	$(DOSBOX) $(DOSBOX_ARGS) -c "OS1993"

test: all
	@rm -f $(BUILD)/SERIAL.LOG $(BUILD)/LOADER.LOG
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $(DOSBOX) $(DOSBOX_ARGS) \
	    -nogui -time-limit 60 -c "OS1993 /T > LOADER.LOG" -c "exit"
	@tools/check-test.sh $(BUILD)
	@rm -f $(BUILD)/CPU386.LOG
	@SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $(DOSBOX) $(DOSBOX_ARGS) -set "cpu cputype=386" \
	    -nogui -time-limit 20 -c "OS1993 > CPU386.LOG" -c "exit" >/dev/null 2>&1
	@if grep -q "requires an 80486" $(BUILD)/CPU386.LOG; then \
	    echo "  PASS  on a 386 the loader refuses to start"; \
	else echo "  FAIL  on a 386 the loader refuses to start"; exit 1; fi

# QEMU has no 386 model; a 486 with 16 MB is the closest 1993 machine.
# -vga std: the ISA VGA with the Bochs VBE registers (display.c); its BIOS is
# vgabios.bin, shipped as vgabios-isavga.bin (build/qemu-rom has the link).
QEMU_ROM  := $(BUILD)/qemu-rom/vgabios.bin
QEMU_ARGS := -M isapc,vmport=on -cpu 486 -m 16 -vga std -L $(BUILD)/qemu-rom -L /usr/share/qemu -drive file=$(BUILD)/freedos.img,format=raw,if=ide \
             -serial file:$(BUILD)/qemu/SERIAL.LOG -device isa-debug-exit,iobase=0xf4,iosize=4
QEMU_IMG  := $(BUILD)/freedos.img@@32256

$(QEMU_ROM):
	@mkdir -p $(dir $@)
	@for d in /usr/share/qemu /usr/share/qemu-firmware /usr/share/seabios; do \
	    if [ -e $$d/vgabios-isavga.bin ]; then ln -sf $$d/vgabios-isavga.bin $@; break; fi; done

qemu: all $(QEMU_ROM)
	@mkdir -p $(BUILD)/qemu
	tools/mkfreedos.sh normal
	$(QEMU) $(QEMU_ARGS)

qemu-test: all $(QEMU_ROM)
	@mkdir -p $(BUILD)/qemu && rm -f $(BUILD)/qemu/*.LOG
	@tools/mkfreedos.sh normal autotest
	timeout -s KILL 120 $(QEMU) $(QEMU_ARGS) -display none || true
	@MTOOLS_SKIP_CHECK=1 mcopy -o -i $(QEMU_IMG) ::/OS1993/LOADER.LOG $(BUILD)/qemu/ 2>/dev/null || true
	@tools/check-test.sh $(BUILD)/qemu

qemu-emm386: all
	@mkdir -p $(BUILD)/qemu && rm -f $(BUILD)/qemu/*.LOG
	@tools/mkfreedos.sh emm386 autotest
	timeout -s KILL 120 $(QEMU) $(QEMU_ARGS) -display none || true
	@MTOOLS_SKIP_CHECK=1 mcopy -o -i $(QEMU_IMG) ::/OS1993/LOADER.LOG $(BUILD)/qemu/ 2>/dev/null || true
	@tr -d '\r' < $(BUILD)/qemu/LOADER.LOG
	@tr -d '\r' < $(BUILD)/qemu/LOADER.LOG | grep -q "virtual 8086 mode" && \
	    echo "  PASS  loader refused to run under JEMM386" || \
	    { echo "  FAIL  loader did not refuse JEMM386"; exit 1; }

qemu-sessions: all
	@tools/qemu-sessions-test.py

qemu-vga: all
	@tools/qemu-vga-test.py

qemu-wm: all
	@tools/qemu-wm-test.py

qemu-absmouse: all
	@tools/qemu-absmouse-test.py

qemu-menu: all
	@tools/qemu-menu-test.py

qemu-controls: all
	@tools/qemu-controls-test.py

qemu-msgbox: all
	@tools/qemu-msgbox-test.py

qemu-shell: all
	@tools/qemu-shell-test.py

qemu-files: all
	@tools/qemu-files-test.py

qemu-notepad: all
	@tools/qemu-notepad-test.py

qemu-tasklist: all
	@tools/qemu-tasklist-test.py

qemu-vesa: all $(QEMU_ROM)
	@tools/qemu-vesa-test.py

qemu-crt: all $(QEMU_ROM)
	@tools/qemu-crt-test.py

check486: $(BUILD)/vmm.elf $(MODULE_ELFS)
	@for f in $^; do tools/check486.sh $(OBJDUMP) $$f || exit 1; done

clean:
	rm -rf $(BUILD)

-include $(KERNEL_OBJ:.o=.d)

-include $(patsubst %.o,%.d,$(filter-out $(U)/hdr/% $(U)/gen/% %/crt0.o %/setjmp.o,$(USER_OBJ)))
