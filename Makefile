# OS1993 -- build with modern tools, run on a 1993 machine.
#
#   make            build build/OS1993.COM, build/VMM.BIN and build/GUI.BIN
#   make run        boot it in DOSBox-X (386, 16 MB, HIMEM-style XMS)
#   make test       headless DOSBox-X run; checks the full round trip to DOS
#   make check386   verify the kernel contains no post-386 instructions
#   make qemu       boot FreeDOS 1.3 in QEMU with OS1993 on C: (real DOS)
#   make qemu-test  the same, headless: run OS1993 /T and check the results
#   make qemu-emm386  OS1993 must refuse to run with JEMM386 loaded
#   make qemu-sessions  type into two concurrent DOS sessions (QEMU sendkey)
#   make qemu-vga   check the VGA driver with QEMU screendumps
#   make qemu-mouse move and click the PS/2 mouse; drag the GUI's window
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

USER_CFLAGS := $(filter-out -Ikernel/include,$(CFLAGS)) -Iuser -Ikernel/include
USER_LDFLAGS := -T user/user.ld -nostdlib -ffreestanding \
           -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-Map,$(BUILD)/gui.map
# The GUI links the kernel's display driver unchanged: it runs in ring 3.
USER_OBJ   := $(BUILD)/user/crt0.o $(BUILD)/user/gui.o $(BUILD)/user/lib.o $(BUILD)/user/gfx.o

KERNEL_C   := $(wildcard kernel/*.c)
KERNEL_ASM := $(wildcard kernel/*.asm)
KERNEL_OBJ := $(patsubst kernel/%.asm,$(BUILD)/kernel/%.o,$(KERNEL_ASM)) \
              $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(KERNEL_C))

.PHONY: all run test check386 qemu qemu-test qemu-emm386 qemu-sessions qemu-vga qemu-mouse clean

all: $(BUILD)/OS1993.COM $(BUILD)/VMM.BIN $(BUILD)/GUI.BIN

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

$(BUILD)/GUI.BIN: $(BUILD)/gui.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/gui.elf: $(USER_OBJ) user/user.ld
	$(CC) $(USER_LDFLAGS) -o $@ $(USER_OBJ)

$(BUILD)/user/%.o: user/%.c | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c -o $@ $<

$(BUILD)/user/gfx.o: kernel/gfx.c | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c -o $@ $<

$(BUILD)/user/%.o: user/%.asm | $(BUILD)/user
	$(NASM) -f elf32 -g -o $@ $<

$(BUILD) $(BUILD)/kernel $(BUILD)/user:
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
QEMU_ARGS := -M isapc -cpu 486 -m 16 -drive file=$(BUILD)/freedos.img,format=raw,if=ide \
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

qemu-mouse: all
	@tools/qemu-mouse-test.py

check386: $(BUILD)/vmm.elf $(BUILD)/gui.elf
	@tools/check386.sh $(OBJDUMP) $(BUILD)/vmm.elf
	@tools/check386.sh $(OBJDUMP) $(BUILD)/gui.elf

clean:
	rm -rf $(BUILD)

-include $(KERNEL_OBJ:.o=.d) $(USER_OBJ:.o=.d)
