/*
 * Kernel heap (README section 15).
 *
 * Small allocations (up to HEAP_SMALL_MAX bytes) come from per-size-class
 * free lists carved out of single pages; larger ones get their own
 * physically contiguous pages. All memory is reached through the direct map.
 * Every block carries a header that catches double frees and corruption.
 * A slab allocator with object caches replaces the size classes later.
 */

#ifndef MEMORY_HEAP_H
#define MEMORY_HEAP_H

#include <stddef.h>
#include <stdint.h>

#define HEAP_ALIGNMENT 16
#define HEAP_SMALL_MAX 2032

/* All return NULL on failure or for size 0; memory is HEAP_ALIGNMENT aligned. */
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);

typedef struct {
    uint64_t allocations;    /* live allocations */
    uint64_t bytes_in_use;   /* requested bytes of live allocations */
    uint64_t pages;          /* pages currently owned by the heap */
} heap_stats_t;

void heap_get_stats(heap_stats_t *stats);

#endif
