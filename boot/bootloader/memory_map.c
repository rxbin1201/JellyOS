#include "memory_map.h"

#include <jelly/boot_layout.h>

static const EFI_MEMORY_DESCRIPTOR *descriptor(const UINT8 *buffer, UINTN desc_size, UINTN index)
{
    return (const EFI_MEMORY_DESCRIPTOR *)(buffer + index * desc_size);
}

static boot_memory_type_t translate_type(UINT32 efi_type)
{
    switch (efi_type) {
    case EfiConventionalMemory:
        return BOOT_MEMORY_USABLE;
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
        return BOOT_MEMORY_BOOTLOADER_RECLAIMABLE;
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:
        return BOOT_MEMORY_FIRMWARE_RUNTIME;
    case EfiACPIReclaimMemory:
        return BOOT_MEMORY_ACPI_RECLAIMABLE;
    case EfiACPIMemoryNVS:
        return BOOT_MEMORY_ACPI_NVS;
    case EfiUnusableMemory:
        return BOOT_MEMORY_BAD;
    default:
        return BOOT_MEMORY_RESERVED;
    }
}

static bool is_ram_like(UINT32 efi_type)
{
    return efi_type != EfiMemoryMappedIO && efi_type != EfiMemoryMappedIOPortSpace &&
           efi_type != EfiReservedMemoryType;
}

EFI_STATUS memory_map_prepare(efi_memory_map_t *map, UINTN headroom_entries)
{
    UINTN size = 0, key, desc_size;
    UINT32 desc_version;
    EFI_STATUS status;

    status = BS->GetMemoryMap(&size, NULL, &key, &desc_size, &desc_version);
    if (status != EFI_BUFFER_TOO_SMALL)
        return EFI_ERROR(status) ? status : EFI_DEVICE_ERROR;

    map->desc_size = desc_size;
    map->entry_capacity = size / desc_size + headroom_entries;
    map->capacity = map->entry_capacity * desc_size;

    map->buffer = boot_alloc_pages(align_up(map->capacity, BOOT_PAGE_SIZE) / BOOT_PAGE_SIZE, EfiLoaderData);
    map->entries = boot_alloc_pages(
        align_up(map->entry_capacity * sizeof(boot_memory_entry_t), BOOT_PAGE_SIZE) / BOOT_PAGE_SIZE,
        BOOT_EFI_MEMORY_BOOT_DATA);
    if (!map->buffer || !map->entries)
        return EFI_OUT_OF_RESOURCES;
    return EFI_SUCCESS;
}

EFI_STATUS memory_map_fetch(efi_memory_map_t *map)
{
    map->size = map->capacity;
    return BS->GetMemoryMap(&map->size, (EFI_MEMORY_DESCRIPTOR *)map->buffer, &map->key,
                            &map->desc_size, &map->desc_version);
}

uint64_t memory_map_highest_address(void)
{
    UINTN count, key, desc_size;
    UINT32 desc_version;
    uint64_t highest = 0;

    EFI_MEMORY_DESCRIPTOR *raw = LibMemoryMap(&count, &key, &desc_size, &desc_version);
    if (!raw)
        return 0;

    for (UINTN i = 0; i < count; i++) {
        const EFI_MEMORY_DESCRIPTOR *d = descriptor((const UINT8 *)raw, desc_size, i);
        uint64_t end = d->PhysicalStart + d->NumberOfPages * BOOT_PAGE_SIZE;
        if (is_ram_like(d->Type) && end > highest)
            highest = end;
    }
    FreePool(raw);
    return highest;
}

uint64_t memory_map_total_ram(void)
{
    UINTN count, key, desc_size;
    UINT32 desc_version;
    uint64_t total = 0;

    EFI_MEMORY_DESCRIPTOR *raw = LibMemoryMap(&count, &key, &desc_size, &desc_version);
    if (!raw)
        return 0;

    for (UINTN i = 0; i < count; i++) {
        const EFI_MEMORY_DESCRIPTOR *d = descriptor((const UINT8 *)raw, desc_size, i);
        if (is_ram_like(d->Type) && d->Type != EfiUnusableMemory)
            total += d->NumberOfPages * BOOT_PAGE_SIZE;
    }
    FreePool(raw);
    return total;
}

static void sort_entries(boot_memory_entry_t *entries, UINTN count)
{
    for (UINTN i = 1; i < count; i++) {
        boot_memory_entry_t current = entries[i];
        UINTN j = i;
        while (j > 0 && entries[j - 1].base > current.base) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = current;
    }
}

/*
 * Mark [base, base + length) as kernel memory: the entries it overlaps are
 * split so that exactly this range changes its type. The firmware knows
 * the range only as loader data, possibly merged with its neighbours.
 */
static UINTN mark_kernel_range(boot_memory_entry_t *entries, UINTN count, UINTN capacity, uint64_t base,
                               uint64_t length)
{
    uint64_t end = base + length;

    for (UINTN i = 0; i < count; i++) {
        boot_memory_entry_t e = entries[i];
        uint64_t e_end = e.base + e.length;
        if (e_end <= base || e.base >= end || e.type == BOOT_MEMORY_KERNEL_AND_MODULES)
            continue;

        uint64_t from = e.base > base ? e.base : base, to = e_end < end ? e_end : end;
        boot_memory_entry_t parts[3];
        UINTN n = 0;
        if (from > e.base)
            parts[n++] = (boot_memory_entry_t){ e.base, from - e.base, e.type, 0 };
        parts[n++] = (boot_memory_entry_t){ from, to - from, BOOT_MEMORY_KERNEL_AND_MODULES, 0 };
        if (e_end > to)
            parts[n++] = (boot_memory_entry_t){ to, e_end - to, e.type, 0 };

        if (count + n - 1 > capacity) {
            /* No room to split: rather lose the neighbours than have the kernel's memory handed out. */
            entries[i].type = BOOT_MEMORY_KERNEL_AND_MODULES;
            continue;
        }
        for (UINTN k = count; k > i + 1; k--)
            entries[k + n - 2] = entries[k - 1];
        for (UINTN k = 0; k < n; k++)
            entries[i + k] = parts[k];
        count += n - 1;
        i += n - 1;
    }
    return count;
}

UINTN memory_map_convert(efi_memory_map_t *map)
{
    UINTN raw_count = map->size / map->desc_size;
    UINTN count = 0;

    for (UINTN i = 0; i < raw_count && count < map->entry_capacity; i++) {
        const EFI_MEMORY_DESCRIPTOR *d = descriptor(map->buffer, map->desc_size, i);
        if (d->NumberOfPages == 0)
            continue;

        boot_memory_entry_t *e = &map->entries[count++];
        e->base = d->PhysicalStart;
        e->length = d->NumberOfPages * BOOT_PAGE_SIZE;
        e->type = translate_type(d->Type);
        e->reserved = 0;
    }

    sort_entries(map->entries, count);

    UINTN range_count;
    const boot_range_t *ranges = boot_kernel_ranges(&range_count);
    for (UINTN i = 0; i < range_count; i++)
        count = mark_kernel_range(map->entries, count, map->entry_capacity, ranges[i].base, ranges[i].length);

    /* Merge adjacent regions of the same type. */
    UINTN merged = 0;
    for (UINTN i = 0; i < count; i++) {
        boot_memory_entry_t *last = merged ? &map->entries[merged - 1] : NULL;
        boot_memory_entry_t *e = &map->entries[i];

        if (last && last->type == e->type && last->base + last->length == e->base)
            last->length += e->length;
        else
            map->entries[merged++] = *e;
    }
    return merged;
}
