# JellyOS top-level build
#
#   make        build boot manager and kernel into build/esp
#   make run    boot in QEMU + OVMF (serial on stdio), KVM=1 for hardware virtualization
#   make debug  like run, but wait for GDB on :1234
#   make clean  remove build output

BUILD   := build
ESP     := $(BUILD)/esp

CC      := gcc
LD      := ld
OBJCOPY := objcopy

PROTOCOL_INC := boot/protocol

# --- gnu-efi (prebuilt) ------------------------------------------------------

GNUEFI     := gnu-efi
GNUEFI_INC := $(GNUEFI)/inc
GNUEFI_LIB := $(GNUEFI)/x86_64

# --- Boot manager (UEFI application) -----------------------------------------

BOOT_SRC_DIR := boot/bootloader
BOOT_OBJ_DIR := $(BUILD)/boot
BOOT_SRCS    := $(wildcard $(BOOT_SRC_DIR)/*.c $(BOOT_SRC_DIR)/*.S)
BOOT_OBJS    := $(patsubst $(BOOT_SRC_DIR)/%,$(BOOT_OBJ_DIR)/%.o,$(BOOT_SRCS))
BOOT_SO      := $(BOOT_OBJ_DIR)/bootx64.so
BOOT_EFI     := $(ESP)/EFI/BOOT/BOOTX64.EFI
BOOT_CFG     := $(ESP)/boot/boot.cfg

BOOT_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Werror \
               -I$(GNUEFI_INC) -I$(GNUEFI_INC)/x86_64 -I$(PROTOCOL_INC) \
               -DGNU_EFI_USE_MS_ABI \
               -fpic -ffreestanding -fno-stack-protector -fno-stack-check \
               -fshort-wchar -mno-red-zone -maccumulate-outgoing-args \
               -MMD -MP

BOOT_LDFLAGS := -nostdlib -shared -Bsymbolic -znocombreloc -z noexecstack \
                -T $(GNUEFI_LIB)/elf_x86_64_efi.lds \
                -L $(GNUEFI_LIB)

# --- Kernel (ELF64, higher half) ---------------------------------------------

KERNEL_OBJ_DIR := $(BUILD)/kernel
KERNEL_SRCS    := $(shell find kernel -name '*.c' -o -name '*.S')
KERNEL_OBJS    := $(patsubst %,$(BUILD)/%.o,$(KERNEL_SRCS))
KERNEL_LDS     := kernel/arch/x86_64/linker.ld
KERNEL_ELF     := $(BUILD)/kernel.elf
KERNEL_ESP     := $(ESP)/boot/kernels/kernel-current.elf

SDK_INC        := sdk/include

# Kernel symbols for stack traces: link once with an empty table, generate the
# table from that image, link again and verify that no function moved.
KSYMS_DIR      := $(BUILD)/ksyms
KSYMS_TOOL     := tools/debugger/ksyms.py

KERNEL_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Werror \
                 -Ikernel -I$(PROTOCOL_INC) -I$(SDK_INC) \
                 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
                 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
                 -fno-omit-frame-pointer \
                 -MMD -MP

KERNEL_LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 -z noexecstack \
                  -T $(KERNEL_LDS)

# --- QEMU --------------------------------------------------------------------

OVMF_CODE := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS := /usr/share/OVMF/OVMF_VARS_4M.fd
VARS_COPY := $(BUILD)/OVMF_VARS.fd

QEMU       := qemu-system-x86_64
QEMU_FLAGS := -machine q35 -m 512M -no-reboot \
              -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
              -drive if=pflash,format=raw,file=$(VARS_COPY) \
              -drive format=raw,file=fat:rw:$(ESP) \
              -serial stdio -net none

# make run KVM=1: hardware virtualization with the host CPU (needs /dev/kvm)
ifeq ($(KVM),1)
QEMU_FLAGS += -enable-kvm -cpu host
endif

# --- Targets -----------------------------------------------------------------

.PHONY: all run debug reset-vars clean

all: $(BOOT_EFI) $(BOOT_CFG) $(KERNEL_ESP)

$(BOOT_OBJ_DIR)/%.o: $(BOOT_SRC_DIR)/%
	@mkdir -p $(@D)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BOOT_SO): $(BOOT_OBJS)
	$(LD) $(BOOT_LDFLAGS) $(GNUEFI_LIB)/crt0-efi-x86_64.o $^ -o $@ -lgnuefi -lefi

$(BOOT_EFI): $(BOOT_SO)
	@mkdir -p $(@D)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym \
	           -j .rel -j .rela -j .rel.* -j .rela.* -j .reloc \
	           --target efi-app-x86_64 --subsystem=10 $< $@

$(BOOT_CFG): $(BOOT_SRC_DIR)/boot.cfg
	@mkdir -p $(@D)
	cp $< $@

$(BUILD)/kernel/%.o: kernel/%
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KSYMS_DIR)/empty.c: $(KSYMS_TOOL)
	@mkdir -p $(@D)
	python3 $(KSYMS_TOOL) --empty > $@

$(KSYMS_DIR)/%.o: $(KSYMS_DIR)/%.c
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KSYMS_DIR)/pass1.elf: $(KERNEL_OBJS) $(KSYMS_DIR)/empty.o $(KERNEL_LDS)
	$(LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJS) $(KSYMS_DIR)/empty.o -o $@

$(KSYMS_DIR)/table.c: $(KSYMS_DIR)/pass1.elf $(KSYMS_TOOL)
	nm -n --defined-only $< | python3 $(KSYMS_TOOL) > $@

$(KERNEL_ELF): $(KERNEL_OBJS) $(KSYMS_DIR)/table.o $(KERNEL_LDS)
	$(LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJS) $(KSYMS_DIR)/table.o -o $@
	@nm -n --defined-only $@ | python3 $(KSYMS_TOOL) | cmp -s - $(KSYMS_DIR)/table.c || \
	    { echo "error: kernel symbol table does not match the final link"; rm -f $@; exit 1; }

$(KERNEL_ESP): $(KERNEL_ELF)
	@mkdir -p $(@D)
	$(OBJCOPY) --strip-debug $< $@

$(VARS_COPY):
	@mkdir -p $(@D)
	cp $(OVMF_VARS) $@

run: all $(VARS_COPY)
	$(QEMU) $(QEMU_FLAGS)

debug: all $(VARS_COPY)
	$(QEMU) $(QEMU_FLAGS) -s -S

# Forget the persistent boot state (fresh UEFI variable store).
reset-vars:
	rm -f $(VARS_COPY)

clean:
	rm -rf $(BUILD)

-include $(BOOT_OBJS:.o=.d) $(KERNEL_OBJS:.o=.d)
