#include "elf_loader.h"

#include "elf.h"
#include "log.h"

#include <jelly/boot_info.h>
#include <jelly/boot_layout.h>

#define MAX_PROGRAM_HEADERS 64
#define MAX_KERNEL_SPAN     0x10000000ULL /* 256 MiB */

static const elf64_phdr_t *program_header(const void *file, unsigned index)
{
    const elf64_header_t *eh = file;
    return (const elf64_phdr_t *)((const uint8_t *)file + eh->phoff) + index;
}

static bool range_in_file(uint64_t offset, uint64_t size, UINTN file_size)
{
    return offset <= file_size && size <= file_size - offset;
}

static EFI_STATUS validate_header(const elf64_header_t *eh, UINTN file_size)
{
    if (file_size < sizeof(*eh) ||
        eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' || eh->ident[3] != 'F') {
        log_error(L"Kernel is not an ELF file");
        return EFI_LOAD_ERROR;
    }
    if (eh->ident[4] != ELF_CLASS_64 || eh->ident[5] != ELF_DATA_LSB ||
        eh->machine != ELF_MACHINE_X64 || eh->version != ELF_VERSION_CUR) {
        log_error(L"Kernel is not a little-endian x86_64 ELF64 image");
        return EFI_UNSUPPORTED;
    }
    if (eh->type != ELF_TYPE_EXEC) {
        log_error(L"Kernel must be a static ELF executable (ET_EXEC)");
        return EFI_UNSUPPORTED;
    }
    if (eh->phentsize != sizeof(elf64_phdr_t) || eh->phnum == 0 || eh->phnum > MAX_PROGRAM_HEADERS ||
        !range_in_file(eh->phoff, (uint64_t)eh->phnum * sizeof(elf64_phdr_t), file_size)) {
        log_error(L"Kernel has invalid program headers");
        return EFI_LOAD_ERROR;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS validate_segment(const elf64_phdr_t *ph, UINTN file_size)
{
    if (ph->filesz > ph->memsz || !range_in_file(ph->offset, ph->filesz, file_size)) {
        log_error(L"Kernel segment exceeds the file");
        return EFI_LOAD_ERROR;
    }
    /* Keep room for rounding the end up to a page boundary without wrapping. */
    uint64_t room = ~0ULL - ph->vaddr;
    if (ph->vaddr < BOOT_KERNEL_VMA_MIN || room < BOOT_PAGE_SIZE || ph->memsz > room - BOOT_PAGE_SIZE) {
        log_error(L"Kernel segment 0x%lx is outside the kernel address range", ph->vaddr);
        return EFI_LOAD_ERROR;
    }
    if ((ph->flags & ELF_PF_W) && (ph->flags & ELF_PF_X)) {
        log_error(L"Kernel segment 0x%lx is writable and executable", ph->vaddr);
        return EFI_SECURITY_VIOLATION;
    }
    return EFI_SUCCESS;
}

static bool segments_overlap(const elf64_phdr_t *a, const elf64_phdr_t *b)
{
    uint64_t a_start = align_down(a->vaddr, BOOT_PAGE_SIZE);
    uint64_t a_end = align_up(a->vaddr + a->memsz, BOOT_PAGE_SIZE);
    uint64_t b_start = align_down(b->vaddr, BOOT_PAGE_SIZE);
    uint64_t b_end = align_up(b->vaddr + b->memsz, BOOT_PAGE_SIZE);

    return a_start < b_end && b_start < a_end;
}

static bool is_load_segment(const elf64_phdr_t *ph)
{
    return ph->type == ELF_PT_LOAD && ph->memsz != 0;
}

/* Find the JellyOS boot protocol note inside a PT_NOTE segment. */
static bool find_protocol_note(const void *file, const elf64_phdr_t *ph, UINTN file_size,
                               boot_note_protocol_t *out)
{
    static const char name[] = BOOT_NOTE_NAME;

    if (!range_in_file(ph->offset, ph->filesz, file_size))
        return false;

    const uint8_t *pos = (const uint8_t *)file + ph->offset;
    const uint8_t *end = pos + ph->filesz;

    while ((UINTN)(end - pos) >= sizeof(elf64_note_t)) {
        const elf64_note_t *note = (const elf64_note_t *)pos;
        uint64_t name_size = align_up(note->namesz, 4);
        uint64_t desc_size = align_up(note->descsz, 4);
        const uint8_t *note_name = pos + sizeof(*note);
        const uint8_t *desc = note_name + name_size;

        if (name_size > (UINTN)(end - note_name) || desc_size > (UINTN)(end - desc))
            return false;

        if (note->type == BOOT_NOTE_TYPE_PROTOCOL && note->namesz == sizeof(name) &&
            CompareMem(note_name, name, sizeof(name)) == 0 &&
            note->descsz >= sizeof(boot_note_protocol_t)) {
            CopyMem(out, desc, sizeof(*out));
            return true;
        }
        pos = desc + desc_size;
    }
    return false;
}

EFI_STATUS elf_validate_kernel(const void *file, UINTN file_size, loaded_kernel_t *kernel)
{
    const elf64_header_t *eh = file;
    uint64_t lowest = ~0ULL, highest = 0;
    bool entry_ok = false, note_found = false;
    boot_note_protocol_t note;
    EFI_STATUS status;

    status = validate_header(eh, file_size);
    if (EFI_ERROR(status))
        return status;

    for (unsigned i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t *ph = program_header(file, i);

        if (ph->type == ELF_PT_NOTE && !note_found)
            note_found = find_protocol_note(file, ph, file_size, &note);
        if (!is_load_segment(ph))
            continue;

        status = validate_segment(ph, file_size);
        if (EFI_ERROR(status))
            return status;

        for (unsigned j = 0; j < i; j++) {
            const elf64_phdr_t *other = program_header(file, j);
            if (is_load_segment(other) && segments_overlap(ph, other)) {
                log_error(L"Kernel segments share or overlap pages");
                return EFI_LOAD_ERROR;
            }
        }

        if (ph->vaddr < lowest)
            lowest = ph->vaddr;
        if (ph->vaddr + ph->memsz > highest)
            highest = ph->vaddr + ph->memsz;
        if ((ph->flags & ELF_PF_X) && eh->entry >= ph->vaddr && eh->entry < ph->vaddr + ph->memsz)
            entry_ok = true;
    }

    if (highest == 0) {
        log_error(L"Kernel has no loadable segments");
        return EFI_LOAD_ERROR;
    }
    if (!entry_ok) {
        log_error(L"Kernel entry 0x%lx is not inside an executable segment", eh->entry);
        return EFI_LOAD_ERROR;
    }
    if (!note_found) {
        log_error(L"Kernel has no JellyOS boot protocol note");
        return EFI_INCOMPATIBLE_VERSION;
    }
    if (note.required_version > BOOT_INFO_VERSION) {
        log_error(L"Kernel requires boot protocol %d, boot manager provides %d",
                  note.required_version, BOOT_INFO_VERSION);
        return EFI_INCOMPATIBLE_VERSION;
    }

    kernel->virt_base = align_down(lowest, BOOT_PAGE_SIZE);
    kernel->size = align_up(highest, BOOT_PAGE_SIZE) - kernel->virt_base;
    kernel->entry = eh->entry;
    kernel->phys_base = 0;
    kernel->required_boot_version = note.required_version;

    if (kernel->size > MAX_KERNEL_SPAN) {
        log_error(L"Kernel image is too large (%ld bytes)", kernel->size);
        return EFI_LOAD_ERROR;
    }
    return EFI_SUCCESS;
}

EFI_STATUS elf_load_kernel(const void *file, page_tables_t *pt, loaded_kernel_t *kernel)
{
    const elf64_header_t *eh = file;
    UINTN pages = kernel->size / BOOT_PAGE_SIZE;
    uint8_t *image = boot_alloc_pages(pages, BOOT_EFI_MEMORY_KERNEL);

    if (!image)
        return EFI_OUT_OF_RESOURCES;
    kernel->phys_base = (uint64_t)(UINTN)image;

    for (unsigned i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t *ph = program_header(file, i);
        if (!is_load_segment(ph))
            continue;

        /* The image is zeroed, so the .bss part (memsz > filesz) needs no extra work. */
        CopyMem(image + (ph->vaddr - kernel->virt_base), (const uint8_t *)file + ph->offset, ph->filesz);

        uint32_t map_flags = 0;
        if (ph->flags & ELF_PF_W)
            map_flags |= MAP_WRITABLE;
        if (ph->flags & ELF_PF_X)
            map_flags |= MAP_EXECUTABLE;

        uint64_t start = align_down(ph->vaddr, BOOT_PAGE_SIZE);
        uint64_t end = align_up(ph->vaddr + ph->memsz, BOOT_PAGE_SIZE);
        for (uint64_t virt = start; virt < end; virt += BOOT_PAGE_SIZE) {
            uint64_t phys = kernel->phys_base + (virt - kernel->virt_base);
            EFI_STATUS status = paging_map_page(pt, virt, phys, map_flags);
            if (EFI_ERROR(status))
                return status;
        }
    }
    return EFI_SUCCESS;
}
