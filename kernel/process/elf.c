#include "process/elf.h"

#include "process/process.h"

#include "core/log.h"
#include "core/string.h"
#include "memory/layout.h"

#define ELF_CLASS_64    2
#define ELF_DATA_LSB    1
#define ELF_TYPE_EXEC   2
#define ELF_MACHINE_X64 62
#define ELF_PT_LOAD     1
#define ELF_PF_X        (1u << 0)
#define ELF_PF_W        (1u << 1)
#define MAX_SEGMENTS    16

typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_header_t;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} elf_phdr_t;

static bool in_file(uint64_t offset, uint64_t length, size_t size)
{
    return offset <= size && length <= size - offset;
}

static const elf_phdr_t *phdr(const void *image, unsigned i)
{
    const elf_header_t *eh = image;
    return (const elf_phdr_t *)((const uint8_t *)image + eh->phoff) + i;
}

static bool is_load(const elf_phdr_t *ph)
{
    return ph->type == ELF_PT_LOAD && ph->memsz;
}

static status_t validate(const void *image, size_t size)
{
    const elf_header_t *eh = image;
    bool entry_ok = false;

    if (size < sizeof(*eh) || eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != ELF_CLASS_64 || eh->ident[5] != ELF_DATA_LSB ||
        eh->type != ELF_TYPE_EXEC || eh->machine != ELF_MACHINE_X64 || eh->phentsize != sizeof(elf_phdr_t) ||
        eh->phnum == 0 || eh->phnum > MAX_SEGMENTS || !in_file(eh->phoff, eh->phnum * sizeof(elf_phdr_t), size))
        return STATUS_INVALID_ARGUMENT;

    for (unsigned i = 0; i < eh->phnum; i++) {
        const elf_phdr_t *ph = phdr(image, i);
        if (!is_load(ph))
            continue;
        if (ph->filesz > ph->memsz || !in_file(ph->offset, ph->filesz, size) ||
            ph->vaddr < USER_SPACE_START || ph->vaddr >= USER_IMAGE_END || ph->memsz > USER_IMAGE_END - ph->vaddr ||
            ((ph->flags & ELF_PF_W) && (ph->flags & ELF_PF_X)))
            return STATUS_INVALID_ARGUMENT;

        uint64_t start = align_down(ph->vaddr, PAGE_SIZE), end = align_up(ph->vaddr + ph->memsz, PAGE_SIZE);
        for (unsigned j = 0; j < i; j++) {
            const elf_phdr_t *other = phdr(image, j);
            if (is_load(other) && start < align_up(other->vaddr + other->memsz, PAGE_SIZE) &&
                align_down(other->vaddr, PAGE_SIZE) < end)
                return STATUS_INVALID_ARGUMENT;
        }
        if ((ph->flags & ELF_PF_X) && eh->entry >= ph->vaddr && eh->entry < ph->vaddr + ph->memsz)
            entry_ok = true;
    }
    return entry_ok ? STATUS_SUCCESS : STATUS_INVALID_ARGUMENT;
}

/* Copy file data into the (not active) address space through the direct map. */
static void copy_segment(vm_space_t *space, uint64_t vaddr, const uint8_t *data, uint64_t length)
{
    while (length) {
        uint64_t phys;
        uint32_t flags;
        uint64_t chunk = PAGE_SIZE - (vaddr & (PAGE_SIZE - 1));
        if (chunk > length)
            chunk = length;

        vmm_query(space, vaddr, &phys, &flags);
        memcpy(phys_to_virt(phys), data, chunk);
        vaddr += chunk;
        data += chunk;
        length -= chunk;
    }
}

status_t elf_load_user(vm_space_t *space, const void *image, size_t size, uint64_t *entry, uint64_t *pages)
{
    const elf_header_t *eh = image;
    status_t status = validate(image, size);

    if (STATUS_IS_ERROR(status)) {
        klog_warn("elf: invalid or unsupported user executable");
        return status;
    }

    *pages = 0;
    for (unsigned i = 0; i < eh->phnum; i++) {
        const elf_phdr_t *ph = phdr(image, i);
        if (!is_load(ph))
            continue;

        uint64_t start = align_down(ph->vaddr, PAGE_SIZE);
        uint64_t length = align_up(ph->vaddr + ph->memsz, PAGE_SIZE) - start;
        uint32_t flags = VM_USER | ((ph->flags & ELF_PF_W) ? VM_WRITE : 0) | ((ph->flags & ELF_PF_X) ? VM_EXEC : 0);

        status = vmm_alloc(space, start, length, flags);
        if (STATUS_IS_ERROR(status))
            return status; /* mapped segments are freed with the address space */
        *pages += length / PAGE_SIZE;
        copy_segment(space, ph->vaddr, (const uint8_t *)image + ph->offset, ph->filesz);
    }
    *entry = eh->entry;
    return STATUS_SUCCESS;
}
