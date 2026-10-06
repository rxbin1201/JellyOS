#include "core/boot.h"

#include "core/string.h"

static boot_info_t info;

bool boot_accept(const boot_info_t *loader_info)
{
    if (!loader_info || loader_info->magic != BOOT_INFO_MAGIC ||
        loader_info->version < KERNEL_REQUIRED_BOOT_VERSION ||
        loader_info->size < __builtin_offsetof(boot_info_t, cmdline_length) + sizeof(uint64_t) ||
        loader_info->memory.entry_size < sizeof(boot_memory_entry_t))
        return false;

    /* Fields beyond what the boot manager wrote stay zero. */
    size_t size = loader_info->size < sizeof(info) ? loader_info->size : sizeof(info);
    memset(&info, 0, sizeof(info));
    memcpy(&info, loader_info, size);
    info.size = (uint32_t)size;
    return true;
}

const boot_info_t *boot_info(void)
{
    return &info;
}

void *boot_phys_to_virt(uint64_t phys)
{
    return (void *)(uintptr_t)(info.hhdm_base + phys);
}
