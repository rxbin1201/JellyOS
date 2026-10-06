# JellyOS top-level build
#
#   make        build boot manager and kernel into build/esp
#   make run    boot in QEMU + OVMF (serial on stdio), KVM=1 for hardware virtualization
#   make debug  like run, but wait for GDB on :1234
#   make test   run the kernel self-tests in QEMU (exit status = result)
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
# Built-in drivers live in drivers/, storage layers and file systems in fs/. Kernel tests (tests/kernel) are linked
# into the kernel and run with selftest=1.
KERNEL_SRCS    := $(shell find kernel drivers fs tests/kernel -name '*.c' -o -name '*.S')
KERNEL_OBJS    := $(patsubst %,$(BUILD)/%.o,$(KERNEL_SRCS))
KERNEL_LDS     := kernel/arch/x86_64/linker.ld
KERNEL_ELF     := $(BUILD)/kernel.elf
KERNEL_ESP     := $(ESP)/boot/kernels/kernel-current.elf

SDK_INC        := sdk/include

# Kernel symbols for stack traces: link once with an empty table, generate the
# table from that image, link again and verify that no function moved.
KSYMS_DIR      := $(BUILD)/ksyms
KSYMS_TOOL     := tools/debugger/ksyms.py

# -fno-tree-loop-distribute-patterns: never turn the memset/memcpy loops into calls to themselves
KERNEL_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Werror \
                 -I. -Ikernel -I$(PROTOCOL_INC) -I$(SDK_INC) \
                 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
                 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
                 -fno-omit-frame-pointer -fno-tree-loop-distribute-patterns \
                 -MMD -MP

KERNEL_LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 -z noexecstack \
                  -T $(KERNEL_LDS)

# --- Userspace (static ELF64 programs on libos) -------------------------------

USER_LDS     := userspace/libos/user.ld
LIBOS_SRCS   := $(wildcard userspace/libos/*.c)
LIBOS_OBJS   := $(patsubst %,$(BUILD)/%.o,$(LIBOS_SRCS))

USER_CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Werror -I$(SDK_INC) -I. \
                -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
                -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns \
                -MMD -MP
USER_LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 -z noexecstack -T $(USER_LDS)

# Test program embedded into the kernel (tests/kernel/user_images.S)
USERTEST_ELF := $(BUILD)/tests/userspace/usertest.elf

# --- Loadable kernel modules (ELF64 relocatable, .ko) ---------------------------

MODULE_DIR    := $(BUILD)/modules
MODULE_CFLAGS := $(KERNEL_CFLAGS) -DJELLY_MODULE -fno-asynchronous-unwind-tables

# Test driver modules, loaded as boot modules by `make test`
TEST_MODULE_SRCS := tests/drivers/edu/edu.c tests/drivers/e1000e_msix/e1000e_msix.c \
                    tests/drivers/bad/bad_api.c tests/drivers/bad/bad_dependency.c \
                    tests/drivers/bad/bad_symbol.c
TEST_MODULES     := $(patsubst %.c,$(MODULE_DIR)/%.ko,$(notdir $(TEST_MODULE_SRCS)))

# --- QEMU --------------------------------------------------------------------

OVMF_CODE := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS := /usr/share/OVMF/OVMF_VARS_4M.fd
VARS_COPY := $(BUILD)/OVMF_VARS.fd

QEMU       := qemu-system-x86_64
QEMU_BASE  := -machine q35 -m 512M -no-reboot -serial stdio -net none \
              -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE)

# make run KVM=1: hardware virtualization with the host CPU (needs /dev/kvm)
ifeq ($(KVM),1)
QEMU_BASE  += -enable-kvm -cpu host
endif

# $(call qemu_disks,<esp dir>,<vars file>)
qemu_disks  = -drive if=pflash,format=raw,file=$(2) -drive format=raw,file=fat:rw:$(1)
# $(call virtio_disk,<image>,<id>)
virtio_disk = -drive file=$(1),if=none,id=$(2),format=raw -device virtio-blk-pci,drive=$(2)

QEMU_FLAGS := $(QEMU_BASE) $(call qemu_disks,$(ESP),$(VARS_COPY))

# make run DISK=<image>: attach a raw disk image as a VirtIO block device
ifneq ($(DISK),)
QEMU_FLAGS += $(call virtio_disk,$(DISK),disk0)
endif

# Self-test run: separate ESP and variable store, no menu, QEMU exits with the result.
TEST_ESP     := $(BUILD)/test-esp
TEST_VARS    := $(BUILD)/OVMF_VARS_test.fd
TEST_TIMEOUT := 120
TEST_CMDLINE := loglevel=info selftest=exit

# Test disk: GPT + FAT32 built from tests/storage/disk (tools/image_builder/mkdisk.sh)
TEST_DISK       := $(BUILD)/test-disk.img
TEST_DISK_FILES := $(BUILD)/test-disk-files
# Written by tests/kernel/storage_tests.c and checked from the host after the run
HOST_CHECK_PATH := ::/jellyos/written.txt
HOST_CHECK_TEXT := Written by the JellyOS FAT32 driver.

# --- Targets -----------------------------------------------------------------

.PHONY: all run debug test modules reset-vars clean

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

$(BUILD)/drivers/%.o: drivers/%
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/fs/%.o: fs/%
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/tests/%.o: tests/%
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# User code: the more specific patterns win over the kernel rules above.
$(BUILD)/userspace/%.o: userspace/%
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/tests/userspace/%.o: tests/userspace/%
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USERTEST_ELF): $(BUILD)/tests/userspace/usertest.c.o $(LIBOS_OBJS) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) $(BUILD)/tests/userspace/usertest.c.o $(LIBOS_OBJS) -o $@

$(BUILD)/tests/kernel/user_images.S.o: $(USERTEST_ELF)

# Module sources compile like kernel code; `ld -r` keeps them relocatable.
$(BUILD)/tests/drivers/%.o: tests/drivers/%
	@mkdir -p $(@D)
	$(CC) $(MODULE_CFLAGS) -c $< -o $@

define module_rule
$(MODULE_DIR)/$(basename $(notdir $(1))).ko: $(BUILD)/$(1).o
	@mkdir -p $$(@D)
	$(LD) -r -o $$@ $$<
endef
$(foreach src,$(TEST_MODULE_SRCS),$(eval $(call module_rule,$(src))))

modules: $(TEST_MODULES)

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

# QEMU's isa-debug-exit turns the kernel's verdict into the exit status: 1 = passed.
# The test drivers are passed as boot modules; edu and e1000e are their devices.
test: all $(TEST_MODULES)
	@rm -rf $(TEST_ESP)
	@mkdir -p $(TEST_ESP)/EFI/BOOT $(TEST_ESP)/boot/kernels $(TEST_ESP)/boot/modules
	@cp $(BOOT_EFI) $(TEST_ESP)/EFI/BOOT/
	@cp $(KERNEL_ESP) $(TEST_ESP)/boot/kernels/
	@cp $(TEST_MODULES) $(TEST_ESP)/boot/modules/
	@{ printf '[boot]\ntimeout=0\nmenu=hidden\nfallback_kernel=\n[entry Test]\n'; \
	   printf 'kernel=/boot/kernels/kernel-current.elf\ncmdline="$(TEST_CMDLINE)"\n'; \
	   for m in $(notdir $(TEST_MODULES)); do printf 'module=/boot/modules/%s\n' $$m; done; \
	 } > $(TEST_ESP)/boot/boot.cfg
	@cp $(OVMF_VARS) $(TEST_VARS)
	@rm -rf $(TEST_DISK_FILES) && mkdir -p $(TEST_DISK_FILES) && cp -r tests/storage/disk/. $(TEST_DISK_FILES)/
	@python3 -c "import sys; sys.stdout.buffer.write(bytes((i * 7 + i // 251) % 256 for i in range(200000)))" \
	    > $(TEST_DISK_FILES)/pattern.bin
	@sh tools/image_builder/mkdisk.sh $(TEST_DISK) 64 $(TEST_DISK_FILES) JELLYTEST
	@timeout $(TEST_TIMEOUT) $(QEMU) $(QEMU_BASE) $(call qemu_disks,$(TEST_ESP),$(TEST_VARS)) -display none \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04 -device edu -device e1000e \
	    $(call virtio_disk,$(TEST_DISK),testdisk); \
	status=$$?; \
	if [ $$status -ne 1 ]; then echo "make test: FAILED (QEMU exit status $$status)"; exit 1; fi
	@# The host (mtools) must read what the kernel's FAT32 driver wrote.
	@if [ "$$(mtype -i $(TEST_DISK)@@1M $(HOST_CHECK_PATH))" = "$(HOST_CHECK_TEXT)" ]; then \
	    echo "make test: host reads the file written by JellyOS"; \
	else echo "make test: FAILED (host cannot read $(HOST_CHECK_PATH) correctly)"; exit 1; fi
	@echo "make test: PASSED"

# Forget the persistent boot state (fresh UEFI variable store).
reset-vars:
	rm -f $(VARS_COPY)

clean:
	rm -rf $(BUILD)

-include $(BOOT_OBJS:.o=.d) $(KERNEL_OBJS:.o=.d) $(LIBOS_OBJS:.o=.d) $(BUILD)/tests/userspace/usertest.c.d \
         $(patsubst %,$(BUILD)/%.d,$(TEST_MODULE_SRCS))
