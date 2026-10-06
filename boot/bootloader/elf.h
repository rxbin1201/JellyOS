/*
 * Minimal ELF64 definitions needed by the kernel loader.
 */

#ifndef BOOT_ELF_H
#define BOOT_ELF_H

#include <stdint.h>

#define ELF_CLASS_64     2
#define ELF_DATA_LSB     1
#define ELF_VERSION_CUR  1
#define ELF_TYPE_EXEC    2
#define ELF_MACHINE_X64  62

#define ELF_PT_LOAD      1
#define ELF_PT_NOTE      4

#define ELF_PF_X         (1u << 0)
#define ELF_PF_W         (1u << 1)
#define ELF_PF_R         (1u << 2)

typedef struct {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf64_header_t;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} elf64_phdr_t;

typedef struct {
    uint32_t namesz;
    uint32_t descsz;
    uint32_t type;
} elf64_note_t;

#endif
