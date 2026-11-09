# OS1993 -- build with modern tools, run on a 1993 machine.
#
#   make            build the loader, the kernel, and the ring-3 modules:
#                   build/OS1993.COM, VMM.BIN, KERNEL.DLL, GFX.DLL, UI.DLL, GUI.BIN
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
U           := $(BUILD)/user
MKDEF       := python3 tools/mkdef.py
USER_CFLAGS := $(filter-out -Ikernel/include,$(CFLAGS)) -Iuser/lib -Iuser/gfx -Iuser/ui -Ikernel/include -MMD -MP
MOD_LDFLAGS := -T user/lib/module.ld -nostdlib -ffreestanding \
               -Wl,--build-id=none -Wl,--no-warn-rwx-segments
KRNL_BASE   := $(shell $(MKDEF) user/krnl/krnl.def base)
GFX_BASE    := $(shell $(MKDEF) user/gfx/gfx.def base)
UI_BASE     := $(shell $(MKDEF) user/ui/ui.def base)

KRNL_OBJ := $(U)/krnl/header.o $(U)/krnl/krnl.o $(U)/gen/krnl_exports.o $(U)/lib/lib.o
GFX_OBJ := $(U)/gfx/header.o $(U)/gfx/gfx.o $(U)/gen/gfx_exports.o $(U)/lib/lib.o
UI_SRC  := wnd input frame paint menu accel msgbox dialog button static edit listbox scroll trackbar kbd theme
UI_OBJ  := $(U)/ui/header.o $(patsubst %,$(U)/ui/%.o,$(UI_SRC)) \
           $(U)/gen/ui_exports.o $(U)/gen/gfx_imports.o $(U)/gen/krnl_imports.o $(U)/lib/lib.o
GUI_OBJ := $(U)/apps/header.o $(U)/lib/crt0.o $(U)/apps/gui.o $(U)/gen/ui_imports.o \
           $(U)/gen/krnl_imports.o $(U)/lib/lib.o
USER_OBJ := $(sort $(KRNL_OBJ) $(GFX_OBJ) $(UI_OBJ) $(GUI_OBJ))
MODULES := $(BUILD)/KERNEL.DLL $(BUILD)/GFX.DLL $(BUILD)/UI.DLL $(BUILD)/GUI.BIN

KERNEL_C   := $(wildcard kernel/*.c)
KERNEL_ASM := $(wildcard kernel/*.asm)
KERNEL_OBJ := $(patsubst kernel/%.asm,$(BUILD)/kernel/%.o,$(KERNEL_ASM)) \
              $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(KERNEL_C))

.PHONY: all run test check386 qemu qemu-test qemu-emm386 qemu-sessions qemu-vga qemu-wm qemu-absmouse qemu-menu qemu-msgbox clean

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

$(BUILD)/%.DLL: $(U)/%.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/%.BIN: $(U)/%.elf
	$(OBJCOPY) -O binary $< $@

$(U)/KERNEL.elf: $(KRNL_OBJ) user/lib/module.ld
	$(CC) $(MOD_LDFLAGS) -Wl,--defsym=MODBASE=$(KRNL_BASE) -o $@ $(KRNL_OBJ)

$(U)/GFX.elf: $(GFX_OBJ) user/lib/module.ld
	$(CC) $(MOD_LDFLAGS) -Wl,--defsym=MODBASE=$(GFX_BASE) -o $@ $(GFX_OBJ)

$(U)/UI.elf: $(UI_OBJ) user/lib/module.ld
	$(CC) $(MOD_LDFLAGS) -Wl,--defsym=MODBASE=$(UI_BASE) -o $@ $(UI_OBJ)

$(U)/GUI.elf: $(GUI_OBJ) user/lib/module.ld
	$(CC) $(MOD_LDFLAGS) -Wl,--defsym=MODBASE=0x00400000 -o $@ $(GUI_OBJ)

$(U)/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c -o $@ $<

$(U)/lib/crt0.o: user/lib/crt0.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 -g -o $@ $<

# One header per module: kind 1 = program, 2 = library
$(U)/krnl/header.o: MODDEF := -DKIND=2 -DMODNAME='"KERNEL.DLL"'
$(U)/gfx/header.o:  MODDEF := -DKIND=2 -DMODNAME='"GFX.DLL"'
$(U)/ui/header.o:   MODDEF := -DKIND=2 -DMODNAME='"UI.DLL"'
$(U)/apps/header.o: MODDEF := -DKIND=1 -DMODNAME='"GUI.BIN"'
$(U)/%/header.o: user/lib/header.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $(MODDEF) -o $@ $<

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

check386: $(BUILD)/vmm.elf $(U)/KERNEL.elf $(U)/GFX.elf $(U)/UI.elf $(U)/GUI.elf
	@for f in $^; do tools/check386.sh $(OBJDUMP) $$f || exit 1; done

clean:
	rm -rf $(BUILD)

-include $(KERNEL_OBJ:.o=.d) $(USER_OBJ:.o=.d)

-include $(USER_OBJ:.o=.d)
