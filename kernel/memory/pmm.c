#include "memory/pmm.h"

#include "memory/layout.h"

#include "core/arch.h"
#include "core/log.h"
#include "core/panic.h"
#include "core/string.h"

#include <stdbool.h>

/* Below 1 MiB stays reserved: legacy areas and the future SMP trampoline. */
#define LOW_MEMORY_LIMIT   0x100000ULL
#define MAX_RECLAIM_RANGES 128

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} reclaim_range_t;

static uint64_t *bitmap;
static size_t frame_count;
static size_t word_count;
static size_t search_hint;

static uint64_t total_frames;
static uint64_t free_frames;
static uint64_t allocated_frames;
static uint64_t reclaimable_frames;

static reclaim_range_t reclaim_ranges[MAX_RECLAIM_RANGES];
static size_t reclaim_count;

static inline bool frame_used(size_t frame)
{
    return bitmap[frame / 64] & (1ULL << (frame % 64));
}

static inline void frame_set(size_t frame)
{
    bitmap[frame / 64] |= 1ULL << (frame % 64);
}

static inline void frame_clear(size_t frame)
{
    bitmap[frame / 64] &= ~(1ULL << (frame % 64));
}

/* Mark whole frames inside [base, base + length) free. Returns frames freed. */
static uint64_t release_range(uint64_t base, uint64_t length)
{
    uint64_t start = align_up(base < LOW_MEMORY_LIMIT ? LOW_MEMORY_LIMIT : base, PAGE_SIZE);
    uint64_t end = align_down(base + length, PAGE_SIZE);
    uint64_t freed = 0;

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        size_t frame = addr / PAGE_SIZE;
        if (frame < frame_count && frame_used(frame)) {
            frame_clear(frame);
            freed++;
        }
    }
    free_frames += freed;
    return freed;
}

/* Mark every frame touching [base, base + length) used. */
static void claim_range(uint64_t base, uint64_t length)
{
    for (uint64_t addr = align_down(base, PAGE_SIZE); addr < base + length; addr += PAGE_SIZE) {
        size_t frame = addr / PAGE_SIZE;
        if (frame < frame_count && !frame_used(frame)) {
            frame_set(frame);
            free_frames--;
        }
    }
}

static bool place_bitmap(const boot_memory_entry_t *entries, size_t count, uint64_t bytes)
{
    for (size_t i = 0; i < count; i++) {
        const boot_memory_entry_t *e = &entries[i];
        if (e->type != BOOT_MEMORY_USABLE)
            continue;
        uint64_t start = align_up(e->base < LOW_MEMORY_LIMIT ? LOW_MEMORY_LIMIT : e->base, PAGE_SIZE);
        uint64_t end = e->base + e->length;
        if (start < end && end - start >= bytes) {
            bitmap = phys_to_virt(start);
            return true;
        }
    }
    return false;
}

status_t pmm_init(const boot_memory_entry_t *entries, size_t count)
{
    uint64_t highest = 0;

    for (size_t i = 0; i < count; i++) {
        if (!pmm_is_ram_type(entries[i].type))
            continue;
        total_frames += entries[i].length / PAGE_SIZE;
        if (entries[i].base + entries[i].length > highest)
            highest = entries[i].base + entries[i].length;
    }

    frame_count = highest / PAGE_SIZE;
    word_count = (frame_count + 63) / 64;
    uint64_t bitmap_bytes = align_up(word_count * sizeof(uint64_t), PAGE_SIZE);

    if (!frame_count || !place_bitmap(entries, count, bitmap_bytes)) {
        klog_error("pmm: no usable region for a %lu KiB bitmap", bitmap_bytes / 1024);
        return STATUS_OUT_OF_MEMORY;
    }

    /* Everything starts as in use; only usable RAM is released. */
    memset(bitmap, 0xFF, word_count * sizeof(uint64_t));
    for (size_t i = 0; i < count; i++) {
        const boot_memory_entry_t *e = &entries[i];
        if (e->type == BOOT_MEMORY_USABLE)
            release_range(e->base, e->length);
    }
    claim_range(virt_to_phys(bitmap), bitmap_bytes);

    for (size_t i = 0; i < count; i++) {
        const boot_memory_entry_t *e = &entries[i];
        if (e->type != BOOT_MEMORY_BOOTLOADER_RECLAIMABLE && e->type != BOOT_MEMORY_ACPI_RECLAIMABLE)
            continue;
        if (reclaim_count == MAX_RECLAIM_RANGES) {
            klog_warn("pmm: too many reclaimable ranges, 0x%lx+0x%lx stays reserved", e->base, e->length);
            continue;
        }
        reclaim_ranges[reclaim_count++] = (reclaim_range_t){ e->base, e->length, e->type };
        reclaimable_frames += e->length / PAGE_SIZE;
    }

    klog_info("pmm: %lu MiB RAM, %lu MiB free, bitmap %lu KiB for %zu frames",
              (uint64_t)(total_frames * PAGE_SIZE >> 20), (uint64_t)(free_frames * PAGE_SIZE >> 20),
              (uint64_t)(bitmap_bytes / 1024), frame_count);
    return STATUS_SUCCESS;
}

static bool find_free_frame(size_t *frame)
{
    for (size_t n = 0; n < word_count; n++) {
        size_t w = (search_hint + n) % word_count;
        if (bitmap[w] == ~0ULL)
            continue;
        size_t f = w * 64 + (size_t)__builtin_ctzll(~bitmap[w]);
        if (f >= frame_count)
            continue;
        search_hint = w;
        *frame = f;
        return true;
    }
    return false;
}

status_t pmm_alloc_page(uint64_t *phys)
{
    size_t frame;
    uint64_t flags = arch_interrupts_save();
    bool found = find_free_frame(&frame);

    if (found) {
        frame_set(frame);
        free_frames--;
        allocated_frames++;
    }
    arch_interrupts_restore(flags);

    if (!found)
        return STATUS_OUT_OF_MEMORY;
    *phys = (uint64_t)frame * PAGE_SIZE;
    return STATUS_SUCCESS;
}

status_t pmm_alloc_pages(size_t count, uint64_t *phys)
{
    if (count == 0)
        return STATUS_INVALID_ARGUMENT;
    if (count == 1)
        return pmm_alloc_page(phys);

    uint64_t flags = arch_interrupts_save();
    size_t run = 0, start = 0;
    bool found = false;

    for (size_t f = LOW_MEMORY_LIMIT / PAGE_SIZE; f < frame_count; f++) {
        if (f % 64 == 0 && bitmap[f / 64] == ~0ULL) {
            run = 0;
            f += 63;
            continue;
        }
        if (frame_used(f)) {
            run = 0;
            continue;
        }
        if (run++ == 0)
            start = f;
        if (run == count) {
            found = true;
            break;
        }
    }

    if (found) {
        for (size_t f = start; f < start + count; f++)
            frame_set(f);
        free_frames -= count;
        allocated_frames += count;
    }
    arch_interrupts_restore(flags);

    if (!found)
        return STATUS_OUT_OF_MEMORY;
    *phys = (uint64_t)start * PAGE_SIZE;
    return STATUS_SUCCESS;
}

void pmm_free_pages(uint64_t phys, size_t count)
{
    ASSERT((phys & (PAGE_SIZE - 1)) == 0);

    uint64_t flags = arch_interrupts_save();
    for (size_t i = 0; i < count; i++) {
        size_t frame = phys / PAGE_SIZE + i;
        if (frame >= frame_count || frame < LOW_MEMORY_LIMIT / PAGE_SIZE || !frame_used(frame))
            panic("pmm: freeing frame %p that is not allocated", (void *)(frame * PAGE_SIZE));
        frame_clear(frame);
    }
    free_frames += count;
    allocated_frames -= count;
    arch_interrupts_restore(flags);
}

void pmm_free_page(uint64_t phys)
{
    pmm_free_pages(phys, 1);
}

uint64_t pmm_reclaim(boot_memory_type_t type)
{
    uint64_t flags = arch_interrupts_save();
    uint64_t freed = 0;
    size_t kept = 0;

    for (size_t i = 0; i < reclaim_count; i++) {
        reclaim_range_t *r = &reclaim_ranges[i];
        if (r->type != (uint32_t)type) {
            reclaim_ranges[kept++] = *r;
            continue;
        }
        freed += release_range(r->base, r->length);
        reclaimable_frames -= r->length / PAGE_SIZE;
    }
    reclaim_count = kept;
    arch_interrupts_restore(flags);
    return freed * PAGE_SIZE;
}

void pmm_get_stats(pmm_stats_t *stats)
{
    uint64_t flags = arch_interrupts_save();

    stats->total_bytes = total_frames * PAGE_SIZE;
    stats->free_bytes = free_frames * PAGE_SIZE;
    stats->allocated_bytes = allocated_frames * PAGE_SIZE;
    stats->reclaimable_bytes = reclaimable_frames * PAGE_SIZE;
    uint64_t accounted = free_frames + allocated_frames + reclaimable_frames;
    stats->reserved_bytes = total_frames > accounted ? (total_frames - accounted) * PAGE_SIZE : 0;
    arch_interrupts_restore(flags);
}
