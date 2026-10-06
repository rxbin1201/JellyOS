#include "memory/memory.h"

#include "memory/layout.h"
#include "memory/pmm.h"
#include "memory/vmm.h"

#include "core/log.h"

uint64_t hhdm_base;

status_t memory_init(const boot_info_t *info, const boot_memory_entry_t *entries, size_t count)
{
    /* The kernel keeps the boot manager's direct-map base, so pointers stay valid across the switch. */
    hhdm_base = info->hhdm_base;

    status_t status = pmm_init(entries, count);
    if (STATUS_IS_ERROR(status))
        return status;
    return vmm_init(info, entries, count);
}

void memory_reclaim_boot(void)
{
    pmm_stats_t stats;
    uint64_t freed = pmm_reclaim(BOOT_MEMORY_BOOTLOADER_RECLAIMABLE);

    pmm_get_stats(&stats);
    klog_info("pmm: reclaimed %lu KiB of boot manager memory, %lu MiB free, %lu KiB allocated",
              freed / 1024, stats.free_bytes >> 20, stats.allocated_bytes / 1024);
}
