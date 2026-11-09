# OS1993 -- build with modern tools, run on a 1993 machine.
#
#   make            build the loader, the kernel, and the ring-3 modules:
#                   build/OS1993.COM, VMM.BIN, the DLLs and the programs (*.BIN)
#   make run        boot it in DOSBox-X (386, 16 MB, HIMEM-style XMS)
#   make test       headless DOSBox-X run; checks the full round trip to DOS
#   make check386   verify the kernel contains no post-386 instructions
#   make qemu       boot FreeDOS 1.3 in QEMU with OS1993 on C: (real DOS)
#   make qemu-test  the same, headless: run OS1993 /T and check the results
#   make qemu-emm386  OS1993 must refuse to run with JEMM386 loaded
#   make qemu-sessions  type into two concurrent DOS sessions (QEMU sendkey)
#   make qemu-vga   check the VGA driver with QEMU screendumps
#   make qemu-wm    drive the window manager and every control (mouse, keys)
#   make qemu-absmouse  absolute pointer via QEMU's VMware port (exact position)
#   make qemu-menu  menu bar, system menu and context menus (mouse, keys)
#   make qemu-msgbox  keyboard accelerators and message boxes
#   make qemu-shell the shell and its programs, each a process
#   make clean

CC      := i686-elf-gcc
OBJCOPY := i686-elf-objcopy
OBJDUMP := i686-elf-objdump
NASM    := nasm
DOSBOX  := dosbox-x
QEMU    := qemu-system-i386

BUILD   := build

CFLAGS  := -std=gnu11 -march=i386 -mtune=i386 -mgeneral-regs-only \
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
USER_CFLAGS := $(filter-out -Ikernel/include,$(CFLAGS)) -Iuser/lib -Iuser/gfx -Iuser/ui -Iuser/shell -Ikernel/include -MMD -MP
MOD_LDFLAGS := -T user/lib/module.ld -nostdlib -ffreestanding \
               -Wl,--build-id=none -Wl,--no-warn-rwx-segments
KRNL_BASE   := $(shell $(MKDEF) user/krnl/krnl.def base)
GFX_BASE    := $(shell $(MKDEF) user/gfx/gfx.def base)
UI_BASE     := $(shell $(MKDEF) user/ui/ui.def base)
SHELL_BASE  := $(shell $(MKDEF) user/shell/shell.def base)

# DLLs: header, code, export table, import libraries of what they call
KRNL_OBJ  := $(U)/hdr/KERNEL.DLL.o $(U)/krnl/krnl.o $(U)/gen/krnl_exports.o $(U)/lib/lib.o
GFX_OBJ   := $(U)/hdr/GFX.DLL.o $(U)/gfx/gfx.o $(U)/gen/gfx_exports.o $(U)/lib/lib.o
UI_SRC    := wnd queue input frame paint menu accel msgbox dialog button static edit listbox \
             scroll trackbar kbd theme
UI_OBJ    := $(U)/hdr/UI.DLL.o $(patsubst %,$(U)/ui/%.o,$(UI_SRC)) \
             $(U)/gen/ui_exports.o $(U)/gen/gfx_imports.o $(U)/gen/krnl_imports.o $(U)/lib/lib.o
SHELL_OBJ := $(U)/hdr/SHELL.DLL.o $(U)/shell/shell.o $(U)/shell/icons.o $(U)/gen/shell_exports.o \
             $(U)/gen/ui_imports.o $(U)/gen/krnl_imports.o $(U)/lib/lib.o

# Programs: user/apps/<name>.c, each a process of its own
APPS      := SHELL DEMO CALC PAINT CLOCK
APP_LIBS  := $(U)/lib/crt0.o $(U)/gen/krnl_imports.o $(U)/gen/ui_imports.o \
             $(U)/gen/shell_imports.o $(U)/lib/lib.o
app_obj    = $(U)/hdr/$(1).BIN.o $(U)/apps/$(shell echo $(1) | tr A-Z a-z).o $(APP_LIBS)

USER_OBJ  := $(sort $(KRNL_OBJ) $(GFX_OBJ) $(UI_OBJ) $(SHELL_OBJ) \
                    $(foreach a,$(APPS),$(call app_obj,$(a))))
MODULES   := $(BUILD)/KERNEL.DLL $(BUILD)/GFX.DLL $(BUILD)/UI.DLL $(BUILD)/SHELL.DLL \
             $(patsubst %,$(BUILD)/%.BIN,$(APPS))
MODULE_ELFS := $(patsubst $(BUILD)/%,$(U)/%.elf,$(MODULES))

KERNEL_C   := $(wildcard kernel/*.c)
KERNEL_ASM := $(wildcard kernel/*.asm)
KERNEL_OBJ := $(patsubst kernel/%.asm,$(BUILD)/kernel/%.o,$(KERNEL_ASM)) \
              $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(KERNEL_C))

.PHONY: all run test check386 qemu qemu-test qemu-emm386 qemu-sessions qemu-vga qemu-wm qemu-absmouse qemu-menu qemu-msgbox qemu-shell clean

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

# QEMU has no 386 model; a 486 with 16 MB is the closest 1993 machine.
QEMU_ARGS := -M isapc,vmport=on -cpu 486 -m 16 -drive file=$(BUILD)/freedos.img,format=raw,if=ide \
             -serial file:$(BUILD)/qemu/SERIAL.LOG -device isa-debug-exit,iobase=0xf4,iosize=4
QEMU_IMG  := $(BUILD)/freedos.img@@32256

qemu: all
	@mkdir -p $(BUILD)/qemu
	tools/mkfreedos.sh normal
	$(QEMU) $(QEMU_ARGS)

qemu-test: all
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

qemu-msgbox: all
	@tools/qemu-msgbox-test.py

qemu-shell: all
	@tools/qemu-shell-test.py

check386: $(BUILD)/vmm.elf $(MODULE_ELFS)
	@for f in $^; do tools/check386.sh $(OBJDUMP) $$f || exit 1; done

clean:
	rm -rf $(BUILD)

-include $(KERNEL_OBJ:.o=.d)

-include $(patsubst %.o,%.d,$(filter-out $(U)/hdr/% $(U)/gen/% %/crt0.o,$(USER_OBJ)))
