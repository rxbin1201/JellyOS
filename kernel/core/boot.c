#include "core/boot.h"

#include "core/string.h"

#define CMDLINE_MAX 1024

static boot_info_t info;

static boot_memory_entry_t memory_entries[BOOT_MAX_MEMORY_ENTRIES];
static size_t memory_entry_count;
static boot_module_t modules[BOOT_MAX_MODULES];
static size_t module_count;
static char log_text[BOOT_LOG_MAX];
static size_t log_length;
static char cmdline[CMDLINE_MAX];
static size_t truncated;

/* The boot manager's direct map is valid until the kernel switches page tables. */
static const void *loader_virt(uint64_t phys)
{
    return (const void *)(uintptr_t)(info.hhdm_base + phys);
}

static void copy_memory_map(void)
{
    const uint8_t *entries = loader_virt(info.memory.entries_phys);
    size_t count = info.memory.entry_count;

    if (count > BOOT_MAX_MEMORY_ENTRIES) {
        truncated += count - BOOT_MAX_MEMORY_ENTRIES;
        count = BOOT_MAX_MEMORY_ENTRIES;
    }
    for (size_t i = 0; i < count; i++)
        memcpy(&memory_entries[i], entries + i * info.memory.entry_size, sizeof(boot_memory_entry_t));
    memory_entry_count = count;
}

static void copy_modules(void)
{
    if (!info.modules.module_count || info.modules.entry_size < sizeof(boot_module_t))
        return;

    const uint8_t *list = loader_virt(info.modules.modules_phys);
    size_t count = info.modules.module_count;
    if (count > BOOT_MAX_MODULES) {
        truncated += count - BOOT_MAX_MODULES;
        count = BOOT_MAX_MODULES;
    }
    for (size_t i = 0; i < count; i++) {
        memcpy(&modules[i], list + i * info.modules.entry_size, sizeof(boot_module_t));
        modules[i].name[sizeof(modules[i].name) - 1] = '\0';
    }
    module_count = count;
}

static void copy_strings(void)
{
    const char *source = loader_virt(info.cmdline_phys);
    size_t n = 0;

    for (; n + 1 < CMDLINE_MAX && n < info.cmdline_length && source[n]; n++)
        cmdline[n] = source[n];
    cmdline[n] = '\0';

    if (info.size >= __builtin_offsetof(boot_info_t, log_length) + sizeof(info.log_length) && info.log_phys) {
        log_length = info.log_length < BOOT_LOG_MAX - 1 ? info.log_length : BOOT_LOG_MAX - 1;
        memcpy(log_text, loader_virt(info.log_phys), log_length);
        log_text[log_length] = '\0';
    }
}

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

    copy_memory_map();
    copy_modules();
    copy_strings();

    /* The originals are about to be reclaimed; force use of the copies. */
    info.memory.entries_phys = 0;
    info.modules.modules_phys = 0;
    info.cmdline_phys = 0;
    info.log_phys = 0;
    return true;
}

const boot_info_t *boot_info(void)
{
    return &info;
}

const boot_memory_entry_t *boot_memory_map(size_t *count)
{
    *count = memory_entry_count;
    return memory_entries;
}

const boot_module_t *boot_modules(size_t *count)
{
    *count = module_count;
    return modules;
}

const char *boot_log(size_t *length)
{
    *length = log_length;
    return log_text;
}

const char *boot_cmdline(void)
{
    return cmdline;
}

size_t boot_truncated_entries(void)
{
    return truncated;
}
