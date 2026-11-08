# OS1993 -- build with modern tools, run on a 1993 machine.
#
#   make            build build/OS1993.COM and build/VMM.BIN
#   make run        boot it in DOSBox-X (386, 16 MB, HIMEM-style XMS)
#   make test       headless DOSBox-X run; checks the full round trip to DOS
#   make check386   verify the kernel contains no post-386 instructions
#   make clean

CC      := i686-elf-gcc
OBJCOPY := i686-elf-objcopy
OBJDUMP := i686-elf-objdump
NASM    := nasm
DOSBOX  := dosbox-x

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

KERNEL_C   := $(wildcard kernel/*.c)
KERNEL_ASM := $(wildcard kernel/*.asm)
KERNEL_OBJ := $(patsubst kernel/%.asm,$(BUILD)/kernel/%.o,$(KERNEL_ASM)) \
              $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(KERNEL_C))

.PHONY: all run test check386 clean

all: $(BUILD)/OS1993.COM $(BUILD)/VMM.BIN

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

check386: $(BUILD)/vmm.elf
	@tools/check386.sh $(OBJDUMP) $<

clean:
	rm -rf $(BUILD)

-include $(KERNEL_OBJ:.o=.d)
