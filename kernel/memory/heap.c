#include "memory/heap.h"
#include "core/export.h"

#include "memory/layout.h"
#include "memory/pmm.h"

#include "core/arch.h"
#include "core/panic.h"
#include "core/string.h"

#include <stdbool.h>

#define MAGIC_USED   0x4A48550AU /* small block in use */
#define MAGIC_FREE   0x4A48460AU /* small block on a free list */
#define MAGIC_LARGE  0x4A484C0AU /* large allocation */
#define POISON_BYTE  0xDE

/* Block sizes include the 16-byte header. */
static const uint32_t class_sizes[] = { 32, 64, 128, 256, 512, 1024, 2048 };
#define CLASS_COUNT (sizeof(class_sizes) / sizeof(class_sizes[0]))

typedef struct {
    uint32_t magic;
    uint32_t info;  /* size class index, or page count for large blocks */
    uint64_t size;  /* requested bytes */
} header_t;

_Static_assert(sizeof(header_t) == HEAP_ALIGNMENT, "header must keep payloads aligned");
_Static_assert(HEAP_SMALL_MAX == 2048 - sizeof(header_t), "largest class must match HEAP_SMALL_MAX");

typedef struct free_block {
    header_t header;
    struct free_block *next;
} free_block_t;

static free_block_t *free_lists[CLASS_COUNT];
static heap_stats_t stats;

static unsigned class_for(size_t size)
{
    unsigned c = 0;
    while (class_sizes[c] - sizeof(header_t) < size)
        c++;
    return c;
}

/* Split a fresh page into free blocks of one class. */
static bool refill(unsigned c)
{
    uint64_t phys;

    if (STATUS_IS_ERROR(pmm_alloc_page(&phys)))
        return false;
    stats.pages++;

    uint8_t *page = phys_to_virt(phys);
    for (uint32_t offset = 0; offset + class_sizes[c] <= PAGE_SIZE; offset += class_sizes[c]) {
        free_block_t *block = (free_block_t *)(page + offset);
        block->header = (header_t){ MAGIC_FREE, c, 0 };
        block->next = free_lists[c];
        free_lists[c] = block;
    }
    return true;
}

static header_t *alloc_small(size_t size)
{
    unsigned c = class_for(size);

    if (!free_lists[c] && !refill(c))
        return NULL;

    free_block_t *block = free_lists[c];
    if (block->header.magic != MAGIC_FREE)
        panic("heap: free list of class %u corrupted at %p", class_sizes[c], (void *)block);
    free_lists[c] = block->next;
    block->header = (header_t){ MAGIC_USED, c, size };
    return &block->header;
}

static header_t *alloc_large(size_t size)
{
    size_t pages = align_up(size + sizeof(header_t), PAGE_SIZE) / PAGE_SIZE;
    uint64_t phys;

    if (pages > UINT32_MAX || STATUS_IS_ERROR(pmm_alloc_pages(pages, &phys)))
        return NULL;
    stats.pages += pages;

    header_t *header = phys_to_virt(phys);
    *header = (header_t){ MAGIC_LARGE, (uint32_t)pages, size };
    return header;
}

void *kmalloc(size_t size)
{
    if (size == 0 || size > (size_t)1 << 40)
        return NULL;

    uint64_t flags = arch_interrupts_save();
    header_t *header = size <= HEAP_SMALL_MAX ? alloc_small(size) : alloc_large(size);
    if (header) {
        stats.allocations++;
        stats.bytes_in_use += size;
    }
    arch_interrupts_restore(flags);
    return header ? header + 1 : NULL;
}

static header_t *checked_header(void *ptr)
{
    if ((uintptr_t)ptr % HEAP_ALIGNMENT)
        panic("heap: misaligned pointer %p", ptr);

    header_t *header = (header_t *)ptr - 1;
    if (header->magic == MAGIC_FREE)
        panic("heap: double free of %p", ptr);
    if (header->magic != MAGIC_USED && header->magic != MAGIC_LARGE)
        panic("heap: %p is not a heap block or its header is corrupted", ptr);
    return header;
}

static size_t capacity(const header_t *header)
{
    if (header->magic == MAGIC_LARGE)
        return (size_t)header->info * PAGE_SIZE - sizeof(header_t);
    return class_sizes[header->info] - sizeof(header_t);
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    uint64_t flags = arch_interrupts_save();
    header_t *header = checked_header(ptr);

    stats.allocations--;
    stats.bytes_in_use -= header->size;

    if (header->magic == MAGIC_LARGE) {
        size_t pages = header->info;
        header->magic = 0;
        pmm_free_pages(virt_to_phys(header), pages);
        stats.pages -= pages;
    } else {
        unsigned c = header->info;
        free_block_t *block = (free_block_t *)header;
        memset(ptr, POISON_BYTE, capacity(header));
        block->header = (header_t){ MAGIC_FREE, c, 0 };
        block->next = free_lists[c];
        free_lists[c] = block;
    }
    arch_interrupts_restore(flags);
}

void *kcalloc(size_t count, size_t size)
{
    size_t total;

    if (__builtin_mul_overflow(count, size, &total))
        return NULL;
    void *ptr = kmalloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr)
        return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    header_t *header = checked_header(ptr);
    if (size <= capacity(header)) {
        uint64_t flags = arch_interrupts_save();
        stats.bytes_in_use = stats.bytes_in_use - header->size + size;
        header->size = size;
        arch_interrupts_restore(flags);
        return ptr;
    }

    void *grown = kmalloc(size);
    if (!grown)
        return NULL; /* the original block stays valid */
    memcpy(grown, ptr, header->size);
    kfree(ptr);
    return grown;
}

void heap_get_stats(heap_stats_t *out)
{
    uint64_t flags = arch_interrupts_save();
    *out = stats;
    arch_interrupts_restore(flags);
}

EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kcalloc);
EXPORT_SYMBOL(krealloc);
EXPORT_SYMBOL(kfree);
