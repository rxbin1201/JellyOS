# JellyOS top-level build
#
#   make        build boot manager, kernel and initramfs (userspace) into build/esp
#   make run    boot in QEMU + OVMF (serial on stdio), KVM=1 for hardware virtualization
#   make debug  like run, but wait for GDB on :1234
#   make test   host unit tests, kernel self-tests, then the integration test in QEMU (exit status = result)
#   make unit   host unit tests only (tests/unit)
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
# Built-in drivers live in drivers/, storage layers and file systems in fs/, the network stack in net/,
# the input manager in input/. Kernel tests (tests/kernel) are linked
# into the kernel and run with selftest=1.
KERNEL_SRCS    := $(shell find kernel drivers fs net input audio/device tests/kernel -name '*.c' -o -name '*.S') \
                  graphics/core/font8x16.c
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

# --- Userspace (static ELF64 programs on libc and libos) ----------------------

USER_LDS     := userspace/libos/user.ld
LIBOS_SRCS   := $(wildcard userspace/libos/*.c)
LIBOS_OBJS   := $(patsubst %,$(BUILD)/%.o,$(LIBOS_SRCS))
# libos.a holds only the system call wrappers; start.c and string.c serve bare libos programs (usertest).
LIBOS_A      := $(BUILD)/userspace/libos.a

# libc: crt0 is linked as an object (it holds the entry point), the rest as an archive.
LIBC_SRCS    := $(wildcard userspace/libc/*.c)
LIBC_OBJS    := $(patsubst %,$(BUILD)/%.o,$(filter-out userspace/libc/crt0.c,$(LIBC_SRCS)))
LIBC_CRT0    := $(BUILD)/userspace/libc/crt0.c.o
LIBC_A       := $(BUILD)/userspace/libc.a

# User code sees only the SDK headers and GCC's freestanding ones (stddef, stdint, stdarg, limits, ...).
USER_CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Werror -nostdinc -isystem $(shell $(CC) -print-file-name=include) \
                -I$(SDK_INC) -I. \
                -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
                -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns \
                -MMD -MP
USER_LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 -z noexecstack -T $(USER_LDS)
# Compiler support routines (__popcountdi2, 128-bit division, ...)
LIBGCC       := $(shell $(CC) -print-libgcc-file-name)

# Graphics libraries (README sections 33-36): core drawing, display, window client, compositor, GUI toolkit.
# Built for userspace into their own object tree; the kernel only takes the font.
GRAPHICS_SRCS := $(wildcard graphics/*/*.c)
GRAPHICS_OBJS := $(patsubst %,$(BUILD)/ugraphics/%.o,$(GRAPHICS_SRCS))
GRAPHICS_A    := $(BUILD)/userspace/libgraphics.a

# Audio libraries (README section 38) for userspace: the mixer and the client side of the audio server.
# The kernel takes audio/device.
AUDIO_SRCS    := $(wildcard audio/mixer/*.c audio/client/*.c)
AUDIO_OBJS    := $(patsubst %,$(BUILD)/uaudio/%.o,$(AUDIO_SRCS))
AUDIO_A       := $(BUILD)/userspace/libaudio.a

# Programs of the initramfs: <name>:<install path>:<sources>
PROGRAM_DIR  := $(BUILD)/userspace/programs
COREUTILS    := cat cp dmesg echo false ls mkdir mv rm sleep touch true
NETTOOLS     := http ifconfig nc nslookup ping
AUDIOTOOLS   := play record tone volume
PROGRAMS     := init:/init:userspace/init/init.c \
                servicemanager:/sbin/servicemanager:userspace/services/servicemanager/servicemanager.c \
                networkd:/sbin/networkd:userspace/services/network/networkd.c \
                displayd:/sbin/displayd:userspace/services/display/displayd.c \
                sh:/bin/sh:userspace/shell/shell.c \
                guidemo:/bin/guidemo:userspace/applications/guidemo/guidemo.c \
                terminal:/bin/terminal:userspace/applications/terminal/terminal.c \
                login:/sbin/login:userspace/desktop/login/login.c \
                desktop:/bin/desktop:userspace/desktop/shell/desktop.c \
                files:/bin/files:userspace/applications/files/files.c \
                settings:/bin/settings:userspace/applications/settings/settings.c \
                viewer:/bin/viewer:userspace/applications/viewer/viewer.c \
                notify:/bin/notify:userspace/applications/notify/notify.c \
                audiod:/sbin/audiod:userspace/services/audio/audiod.c \
                gamepad:/bin/gamepad:userspace/applications/gamepad/gamepad.c \
                $(foreach u,$(AUDIOTOOLS),$(u):/bin/$(u):userspace/applications/audio/$(u).c) \
                $(foreach u,$(NETTOOLS),$(u):/bin/$(u):userspace/applications/network/$(u).c) \
                $(foreach u,$(COREUTILS),$(u):/bin/$(u):userspace/applications/coreutils/$(u).c)
# Further sources of programs made of several files: PROGRAM_EXTRA_<name>
PROGRAM_EXTRA_displayd := userspace/services/display/keymap.c
PROGRAM_EXTRA_play     := userspace/applications/audio/wav.c
PROGRAM_EXTRA_record   := userspace/applications/audio/wav.c
program_name   = $(word 1,$(subst :, ,$(1)))
program_path   = $(word 2,$(subst :, ,$(1)))
program_source = $(word 3,$(subst :, ,$(1)))
PROGRAM_ELFS := $(foreach p,$(PROGRAMS),$(PROGRAM_DIR)/$(call program_name,$(p)).elf)

# Initramfs (cpio newc): programs plus the skeleton in tools/image_builder/initramfs
INITRAMFS_SKEL  := tools/image_builder/initramfs
INITRAMFS_ROOT  := $(BUILD)/initramfs-root
INITRAMFS       := $(BUILD)/initramfs.img
INITRAMFS_ESP   := $(ESP)/boot/initrd/current.img
INITRAMFS_TOOL  := tools/image_builder/mkinitramfs.py
INITRAMFS_OWNERS := tools/image_builder/initramfs.owners
# Generated files of the initramfs: system sounds (/usr/share/sounds)
SOUND_TOOL      := tools/image_builder/mksound.py
SOUNDS          := $(BUILD)/sounds/chime.wav

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
# VirtIO keyboard and tablet (absolute pointer) for the display server
virtio_input = -device virtio-keyboard-pci -device virtio-tablet-pci
# USB keyboard and tablet on an xHCI controller (the PS/2 keyboard and mouse are part of the machine)
usb_input = -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0
# $(call audio_hw,<backend>): Intel HDA sound card with QEMU's audio backend (none, pa, alsa, sdl, ...)
audio_hw = -audiodev $(1),id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0
# Sound of `make run`: silent unless a backend is chosen, e.g. `make run AUDIO=pa`
AUDIO ?= none
# Integration test: the output goes into a WAV file that the test analyzes; the input is silence in real time.
AUDIO_TEST_WAV := $(BUILD)/audio-test.wav
audio_test_hw = -audiodev wav,id=snd0,path=$(AUDIO_TEST_WAV) -audiodev none,id=snd1 \
                -device intel-hda -device hda-output,audiodev=snd0 -device hda-micro,audiodev=snd1
# $(call virtio_disk,<image>,<id>)
virtio_disk = -drive file=$(1),if=none,id=$(2),format=raw -device virtio-blk-pci,drive=$(2)
# $(call virtio_nic,<id>): VirtIO NIC on QEMU's user network (NAT; gateway and host 10.0.2.2, DNS 10.0.2.3)
virtio_nic  = -netdev user,id=$(1) -device virtio-net-pci,netdev=$(1)
# Intel 82574L on a second user network (10.0.3.0/24; gateway and host 10.0.3.2)
e1000_nic   = -netdev user,id=intelnet,net=10.0.3.0/24 -device e1000e,netdev=intelnet

QEMU_FLAGS := $(QEMU_BASE) $(call qemu_disks,$(ESP),$(VARS_COPY)) $(usb_input) $(call audio_hw,$(AUDIO))

# make run NET=0: without a network card
ifneq ($(NET),0)
QEMU_FLAGS += $(call virtio_nic,net0)
endif

# make run DISK=<image>: attach a raw disk image as a VirtIO block device
ifneq ($(DISK),)
QEMU_FLAGS += $(call virtio_disk,$(DISK),disk0)
endif

# Self-test run: separate ESP and variable store, no menu, QEMU exits with the result.
TEST_ESP     := $(BUILD)/test-esp
TEST_VARS    := $(BUILD)/OVMF_VARS_test.fd
TEST_TIMEOUT := 120
TEST_CMDLINE := loglevel=info selftest=exit disks=rw nodriver=e1000

# Test disk: GPT + FAT32 built from tests/storage/disk (tools/image_builder/mkdisk.sh)
TEST_DISK       := $(BUILD)/test-disk.img
TEST_DISK_FILES := $(BUILD)/test-disk-files
# Written by tests/kernel/storage_tests.c and checked from the host after the run
# The same disk contents once more behind an NVMe and an AHCI controller
NVME_DISK       := $(BUILD)/test-nvme.img
SATA_DISK       := $(BUILD)/test-sata.img
# $(call hw_disks): NVMe controller with one namespace, and a second SATA disk on the machine's AHCI controller
hw_disks = -drive file=$(NVME_DISK),if=none,id=nvmedisk,format=raw -device nvme,serial=jellynvme,drive=nvmedisk \
           -drive file=$(SATA_DISK),if=none,id=satadisk,format=raw -device ide-hd,drive=satadisk,bus=ide.1
# An exFAT volume written by tools/image_builder/mkexfat.py, as a second VirtIO disk without a partition table
EXFAT_DISK      := $(BUILD)/test-exfat.img
# USB sticks: one on a root port, one behind a hub (with the tablet as its neighbour there)
USB_DISK1       := $(BUILD)/test-usb1.img
USB_DISK2       := $(BUILD)/test-usb2.img
usb_sticks = -drive file=$(USB_DISK1),if=none,id=stick1,format=raw -device usb-storage,bus=xhci.0,drive=stick1,id=usbstick1 \
             -device usb-hub,bus=xhci.0,port=4,id=usbhub \
             -drive file=$(USB_DISK2),if=none,id=stick2,format=raw \
             -device usb-storage,bus=xhci.0,port=4.2,drive=stick2,id=usbstick2
# Keyboard on a root port, tablet behind the hub: the GUI test needs both paths
usb_input_hub = -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 $(usb_sticks) -device usb-tablet,bus=xhci.0,port=4.1
HOST_CHECK_PATH := ::/jellyos/written.txt
HOST_CHECK_TEXT := Written by the JellyOS FAT32 driver.

# Integration test (tests/integration/shell_test.py): normal boot with the initramfs into the shell
SHELL_TEST_ESP  := $(BUILD)/shell-test-esp
SHELL_TEST_VARS := $(BUILD)/OVMF_VARS_shell.fd

# --- Targets -----------------------------------------------------------------

.PHONY: all run debug test unit modules programs reset-vars clean

all: $(BOOT_EFI) $(BOOT_CFG) $(KERNEL_ESP) $(INITRAMFS_ESP)

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

$(BUILD)/net/%.o: net/%
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/input/%.o: input/%
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/audio/%.o: audio/%
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# The font is shared: the kernel console gets its own copy built with kernel flags.
$(BUILD)/graphics/core/font8x16.c.o: graphics/core/font8x16.c
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

$(BUILD)/ugraphics/%.o: %
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(GRAPHICS_A): $(GRAPHICS_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(BUILD)/uaudio/%.o: %
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(AUDIO_A): $(AUDIO_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(BUILD)/sounds/%.wav: $(SOUND_TOOL)
	@mkdir -p $(@D)
	python3 $(SOUND_TOOL) $* $@

$(USERTEST_ELF): $(BUILD)/tests/userspace/usertest.c.o $(LIBOS_OBJS) $(USER_LDS)
	$(LD) $(USER_LDFLAGS) $(BUILD)/tests/userspace/usertest.c.o $(LIBOS_OBJS) -o $@

$(LIBOS_A): $(BUILD)/userspace/libos/syscall.c.o
	@rm -f $@
	ar rcs $@ $^

$(LIBC_A): $(LIBC_OBJS)
	@rm -f $@
	ar rcs $@ $^

program_objects = $(BUILD)/$(call program_source,$(1)).o \
                  $(patsubst %,$(BUILD)/%.o,$(PROGRAM_EXTRA_$(call program_name,$(1))))

define program_rule
$(PROGRAM_DIR)/$(call program_name,$(1)).elf: $(call program_objects,$(1)) $(LIBC_CRT0) $(LIBC_A) $(LIBOS_A) \
        $(GRAPHICS_A) $(AUDIO_A) $(USER_LDS)
	@mkdir -p $$(@D)
	$(LD) $(USER_LDFLAGS) $(LIBC_CRT0) $(call program_objects,$(1)) \
	      --start-group $(GRAPHICS_A) $(AUDIO_A) $(LIBC_A) $(LIBOS_A) --end-group $(LIBGCC) -o $$@
endef
$(foreach p,$(PROGRAMS),$(eval $(call program_rule,$(p))))

# libc test program embedded into the kernel (tests/kernel/user_images.S); not part of the initramfs
SPAWNTEST := spawntest:-:tests/userspace/spawntest.c
$(eval $(call program_rule,$(SPAWNTEST)))

programs: $(PROGRAM_ELFS)

# The root is rebuilt from scratch so removed programs disappear from the image.
$(INITRAMFS): $(PROGRAM_ELFS) $(SOUNDS) $(INITRAMFS_TOOL) $(INITRAMFS_OWNERS) $(shell find $(INITRAMFS_SKEL) -type f)
	@rm -rf $(INITRAMFS_ROOT) && mkdir -p $(INITRAMFS_ROOT)
	@cp -r $(INITRAMFS_SKEL)/. $(INITRAMFS_ROOT)/
	@install -D -m 0644 -t $(INITRAMFS_ROOT)/usr/share/sounds $(SOUNDS)
	@mkdir -p $(INITRAMFS_ROOT)/bin $(INITRAMFS_ROOT)/sbin $(INITRAMFS_ROOT)/lib
	@chmod -R u=rwX,go=rX $(INITRAMFS_ROOT)
	@$(foreach p,$(PROGRAMS),install -D -m 0755 $(PROGRAM_DIR)/$(call program_name,$(p)).elf \
	    $(INITRAMFS_ROOT)$(call program_path,$(p)) && \
	    strip --strip-debug $(INITRAMFS_ROOT)$(call program_path,$(p)) &&) true
	python3 $(INITRAMFS_TOOL) $(INITRAMFS_ROOT) $@ $(INITRAMFS_OWNERS)

$(INITRAMFS_ESP): $(INITRAMFS)
	@mkdir -p $(@D)
	cp $< $@

$(BUILD)/tests/kernel/user_images.S.o: $(USERTEST_ELF) $(PROGRAM_DIR)/spawntest.elf

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
# Host unit tests: code without OS dependencies, built with the host compiler and sanitizers.
UNIT_TESTS := $(BUILD)/unit/canvas_test $(BUILD)/unit/sha256_test $(BUILD)/unit/hid_test $(BUILD)/unit/mixer_test
UNIT_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I.

$(BUILD)/unit/canvas_test: tests/unit/canvas_test.c graphics/core/canvas.c graphics/core/canvas.h \
                           graphics/core/font8x16.c
	@mkdir -p $(@D)
	$(CC) $(UNIT_CFLAGS) tests/unit/canvas_test.c graphics/core/canvas.c graphics/core/font8x16.c -o $@

# libc's SHA-256 against the host C library: the SDK directory only supplies sha256.h.
$(BUILD)/unit/sha256_test: tests/unit/sha256_test.c userspace/libc/sha256.c sdk/include/sha256.h
	@mkdir -p $(@D)
	$(CC) $(UNIT_CFLAGS) -idirafter $(SDK_INC) tests/unit/sha256_test.c userspace/libc/sha256.c -o $@

$(BUILD)/unit/hid_test: tests/unit/hid_test.c drivers/input/hid.c drivers/input/hid.h
	@mkdir -p $(@D)
	$(CC) $(UNIT_CFLAGS) -idirafter $(SDK_INC) tests/unit/hid_test.c drivers/input/hid.c -o $@

$(BUILD)/unit/mixer_test: tests/unit/mixer_test.c audio/mixer/mixer.c audio/mixer/mixer.h
	@mkdir -p $(@D)
	$(CC) $(UNIT_CFLAGS) tests/unit/mixer_test.c audio/mixer/mixer.c -o $@

unit: $(UNIT_TESTS)
	@for t in $(UNIT_TESTS); do $$t || { echo "make unit: FAILED ($$t)"; exit 1; }; done

test: unit all $(TEST_MODULES) $(INITRAMFS)
	@rm -rf $(TEST_ESP)
	@mkdir -p $(TEST_ESP)/EFI/BOOT $(TEST_ESP)/boot/kernels $(TEST_ESP)/boot/modules
	@cp $(BOOT_EFI) $(TEST_ESP)/EFI/BOOT/
	@cp $(KERNEL_ESP) $(TEST_ESP)/boot/kernels/
	@cp $(TEST_MODULES) $(TEST_ESP)/boot/modules/
	@{ printf '[boot]\ntimeout=0\nmenu=hidden\nresolution=keep\nfallback_kernel=\n[entry Test]\n'; \
	   printf 'kernel=/boot/kernels/kernel-current.elf\ncmdline="$(TEST_CMDLINE)"\n'; \
	   for m in $(notdir $(TEST_MODULES)); do printf 'module=/boot/modules/%s\n' $$m; done; \
	 } > $(TEST_ESP)/boot/boot.cfg
	@cp $(OVMF_VARS) $(TEST_VARS)
	@rm -rf $(TEST_DISK_FILES) && mkdir -p $(TEST_DISK_FILES) && cp -r tests/storage/disk/. $(TEST_DISK_FILES)/
	@python3 -c "import sys; sys.stdout.buffer.write(bytes((i * 7 + i // 251) % 256 for i in range(200000)))" \
	    > $(TEST_DISK_FILES)/pattern.bin
	@sh tools/image_builder/mkdisk.sh $(TEST_DISK) 64 $(TEST_DISK_FILES) JELLYTEST
	@sh tools/image_builder/mkdisk.sh $(NVME_DISK) 64 $(TEST_DISK_FILES) JELLYNVME
	@sh tools/image_builder/mkdisk.sh $(SATA_DISK) 64 $(TEST_DISK_FILES) JELLYSATA
	@python3 tools/image_builder/mkexfat.py $(EXFAT_DISK)
	@sh tools/image_builder/mkdisk.sh $(USB_DISK1) 64 $(TEST_DISK_FILES) JELLYUSB1
	@sh tools/image_builder/mkdisk.sh $(USB_DISK2) 64 $(TEST_DISK_FILES) JELLYUSB2
	@timeout $(TEST_TIMEOUT) $(QEMU) $(QEMU_BASE) $(call qemu_disks,$(TEST_ESP),$(TEST_VARS)) -display none \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04 -device edu -device e1000e \
	    $(call virtio_disk,$(TEST_DISK),testdisk) $(call virtio_nic,net0) $(virtio_input) $(usb_input) \
	    $(call audio_hw,none) $(hw_disks) $(call virtio_disk,$(EXFAT_DISK),exfatdisk) $(usb_sticks); \
	status=$$?; \
	if [ $$status -ne 1 ]; then echo "make test: FAILED (QEMU exit status $$status)"; exit 1; fi
	@# The host (mtools) must read what the kernel's FAT32 driver wrote.
	@if [ "$$(mtype -i $(TEST_DISK)@@1M $(HOST_CHECK_PATH))" = "$(HOST_CHECK_TEXT)" ]; then \
	    echo "make test: host reads the file written by JellyOS"; \
	else echo "make test: FAILED (host cannot read $(HOST_CHECK_PATH) correctly)"; exit 1; fi
	@# Milestones M6-M8: boot normally into the shell and the desktop; drive the serial console and the
	@# GUI (QMP input events and screenshots).
	@rm -rf $(SHELL_TEST_ESP)
	@mkdir -p $(SHELL_TEST_ESP)/EFI/BOOT $(SHELL_TEST_ESP)/boot/kernels $(SHELL_TEST_ESP)/boot/initrd
	@cp $(BOOT_EFI) $(SHELL_TEST_ESP)/EFI/BOOT/
	@cp $(KERNEL_ESP) $(SHELL_TEST_ESP)/boot/kernels/
	@cp $(INITRAMFS) $(SHELL_TEST_ESP)/boot/initrd/current.img
	@printf '[boot]\ntimeout=0\nmenu=hidden\nresolution=keep\nfallback_kernel=\n[entry Shell]\nkernel=/boot/kernels/kernel-current.elf\n' \
	    > $(SHELL_TEST_ESP)/boot/boot.cfg
	@printf 'initrd=/boot/initrd/current.img\ncmdline="loglevel=info disks=rw"\n' >> $(SHELL_TEST_ESP)/boot/boot.cfg
	@cp $(OVMF_VARS) $(SHELL_TEST_VARS)
	@rm -f $(BUILD)/qmp.sock $(AUDIO_TEST_WAV)
	@python3 tests/integration/shell_test.py --timeout $(TEST_TIMEOUT) --qmp $(BUILD)/qmp.sock \
	    --wav $(AUDIO_TEST_WAV) -- \
	    $(QEMU) $(QEMU_BASE) $(call qemu_disks,$(SHELL_TEST_ESP),$(SHELL_TEST_VARS)) -display none \
	    $(call virtio_disk,$(TEST_DISK),testdisk) $(call virtio_nic,net0) $(usb_input_hub) $(audio_test_hw) \
	    $(hw_disks) $(call virtio_disk,$(EXFAT_DISK),exfatdisk) $(e1000_nic) \
	    -qmp unix:$(BUILD)/qmp.sock,server,nowait || { echo "make test: FAILED (shell test)"; exit 1; }
	@if mtype -i $(TEST_DISK)@@1M ::/motd.txt | grep -q "Welcome to JellyOS"; then \
	    echo "make test: host reads the file the shell copied"; \
	else echo "make test: FAILED (host cannot read ::/motd.txt)"; exit 1; fi
	@for d in $(NVME_DISK) $(SATA_DISK) $(USB_DISK1) $(USB_DISK2); do \
	    if mtype -i $$d@@1M ::/from-jelly.txt | grep -q "Welcome to JellyOS"; then \
	        echo "make test: host reads the file written to $$d"; \
	    else echo "make test: FAILED (host cannot read ::/from-jelly.txt on $$d)"; exit 1; fi; done
	@echo "make test: PASSED"

# Forget the persistent boot state (fresh UEFI variable store).
reset-vars:
	rm -f $(VARS_COPY)

clean:
	rm -rf $(BUILD)

-include $(BOOT_OBJS:.o=.d) $(KERNEL_OBJS:.o=.d) $(LIBOS_OBJS:.o=.d) $(LIBC_OBJS:.o=.d) $(LIBC_CRT0:.o=.d) \
         $(foreach p,$(PROGRAMS) $(SPAWNTEST),$(BUILD)/$(call program_source,$(p)).d) \
         $(BUILD)/userspace/services/display/keymap.c.d $(BUILD)/userspace/applications/audio/wav.c.d \
         $(GRAPHICS_OBJS:.o=.d) $(AUDIO_OBJS:.o=.d) $(BUILD)/tests/userspace/usertest.c.d \
         $(patsubst %,$(BUILD)/%.d,$(TEST_MODULE_SRCS))
