/*
 * libc: heap allocator.
 *
 * Small blocks come from arenas mapped with SYS_MEMORY_ALLOCATE and are kept
 * in one address-ordered free list (first fit, neighbors merge on free).
 * Large blocks get a mapping of their own and are unmapped on free.
 * One futex mutex makes it safe for several threads.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <jelly/os.h>

#define ALIGNMENT      16
#define ARENA_SIZE     (256 * 1024)
#define LARGE_SIZE     (128 * 1024)
#define PAGE_SIZE      4096
#define MAGIC_USED     0x4A454C4C59ULL /* "JELLY" */
#define MAGIC_MAPPED   0x4D41505045ULL /* "MAPPE" */

typedef struct block {
    size_t        size;  /* whole block including this header */
    uint64_t      magic; /* MAGIC_USED / MAGIC_MAPPED while allocated */
    struct block *next;  /* free list only (overlaps user data) */
} block_t;

#define HEADER_SIZE 16 /* size + magic; user data starts after them */

static block_t *free_list;
static mtx_t heap_lock;

static size_t round_up(size_t value, size_t to)
{
    return (value + to - 1) & ~(to - 1);
}

static void insert_free(block_t *block)
{
    block_t **link = &free_list;
    while (*link && *link < block)
        link = &(*link)->next;
    block->next = *link;
    *link = block;

    /* Merge with the following and the preceding block when adjacent. */
    if (block->next && (char *)block + block->size == (char *)block->next) {
        block->size += block->next->size;
        block->next = block->next->next;
    }
    if (link != &free_list) {
        block_t *previous = (block_t *)((char *)link - offsetof(block_t, next));
        if ((char *)previous + previous->size == (char *)block) {
            previous->size += block->size;
            previous->next = block->next;
        }
    }
}

static int grow(size_t needed)
{
    size_t size = needed > ARENA_SIZE ? round_up(needed, PAGE_SIZE) : ARENA_SIZE;
    void *memory;
    if (STATUS_IS_ERROR(jelly_memory_allocate(size, JELLY_MEMORY_WRITE, &memory)))
        return -1;
    block_t *block = memory;
    block->size = size;
    insert_free(block);
    return 0;
}

void *malloc(size_t size)
{
    if (size == 0)
        size = 1;
    if (size > SIZE_MAX / 2) {
        errno = ENOMEM;
        return NULL;
    }
    size_t needed = round_up(size + HEADER_SIZE, ALIGNMENT);
    if (needed < sizeof(block_t))
        needed = round_up(sizeof(block_t), ALIGNMENT);

    if (needed >= LARGE_SIZE) {
        size_t mapping = round_up(needed, PAGE_SIZE);
        void *memory;
        if (STATUS_IS_ERROR(jelly_memory_allocate(mapping, JELLY_MEMORY_WRITE, &memory))) {
            errno = ENOMEM;
            return NULL;
        }
        block_t *block = memory;
        block->size = mapping;
        block->magic = MAGIC_MAPPED;
        return (char *)block + HEADER_SIZE;
    }

    mtx_lock(&heap_lock);
    for (int attempt = 0; attempt < 2; attempt++) {
        for (block_t **link = &free_list; *link; link = &(*link)->next) {
            block_t *block = *link;
            if (block->size < needed)
                continue;
            if (block->size - needed >= 2 * sizeof(block_t)) {
                block_t *rest = (block_t *)((char *)block + needed);
                rest->size = block->size - needed;
                rest->next = block->next;
                *link = rest;
                block->size = needed;
            } else {
                *link = block->next;
            }
            block->magic = MAGIC_USED;
            mtx_unlock(&heap_lock);
            return (char *)block + HEADER_SIZE;
        }
        if (grow(needed))
            break;
    }
    mtx_unlock(&heap_lock);
    errno = ENOMEM;
    return NULL;
}

static block_t *header_of(void *ptr)
{
    block_t *block = (block_t *)((char *)ptr - HEADER_SIZE);
    if (block->magic != MAGIC_USED && block->magic != MAGIC_MAPPED) {
        static const char message[] = "libc: free() or realloc() of an invalid pointer\n";
        jelly_debug_write(message, sizeof(message) - 1);
        abort();
    }
    return block;
}

void free(void *ptr)
{
    if (!ptr)
        return;
    block_t *block = header_of(ptr);
    if (block->magic == MAGIC_MAPPED) {
        block->magic = 0;
        jelly_memory_unmap(block, block->size);
        return;
    }
    block->magic = 0;
    mtx_lock(&heap_lock);
    insert_free(block);
    mtx_unlock(&heap_lock);
}

void *calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    void *ptr = malloc(count * size);
    if (ptr)
        memset(ptr, 0, count * size);
    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr)
        return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    block_t *block = header_of(ptr);
    size_t capacity = block->size - HEADER_SIZE;
    if (size <= capacity)
        return ptr;

    void *copy = malloc(size);
    if (!copy)
        return NULL;
    memcpy(copy, ptr, capacity);
    free(ptr);
    return copy;
}
