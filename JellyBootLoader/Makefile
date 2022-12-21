SOURCE_FILES := $(shell find Sources -name *.c)
OBJECT_FILES := $(patsubst Sources/%.c, Build/%.o, $(SOURCE_FILES))

CC = gcc
LD = ld

EFI_CFLAGS =-fno-stack-protector -fpic -fshort-wchar -mno-red-zone -I gnu-efi/inc/ -I Sources/ -DEFI_FUNCTION_WRAPPER
EFI_LDFLAGS =gnu-efi/x86_64/crt0-efi-x86_64.o -nostdlib -znocombreloc -T gnu-efi/x86_64/elf_x86_64_efi.lds -shared -Bsymbolic -L gnu-efi/x86_64/ -l:libgnuefi.a -l:libefi.a -z noexecstack

$(OBJECT_FILES): Build/%.o : Sources/%.c
	mkdir -p $(dir $@) && \
	$(CC) $(EFI_CFLAGS) -c $(patsubst Build/%.o, Sources/%.c, $@) -o $@ && \
	$(LD) $(patsubst Sources/%.c, Build/%.o, $@) $(EFI_LDFLAGS) -o BOOTX64.SO
 
.PHONY: boot
boot: $(OBJECT_FILES)
	objcopy -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc --target=efi-app-x86_64 BOOTX64.SO BOOTX64.EFI
