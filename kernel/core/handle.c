#include "core/handle.h"

#include "core/string.h"
#include "memory/heap.h"

#define INITIAL_CAPACITY 16
#define MAX_SLOTS        0xFFFFu

static inline uint32_t slot_of(handle_t handle)
{
    return (handle & 0xFFFF) - 1;
}

static inline handle_t make_handle(uint32_t slot, uint16_t generation)
{
    return ((uint32_t)generation << 16) | (slot + 1);
}

void handle_table_init(handle_table_t *table, uint32_t limit)
{
    table->entries = NULL;
    table->capacity = 0;
    table->used = 0;
    table->limit = limit < MAX_SLOTS ? limit : MAX_SLOTS;
}

void handle_table_destroy(handle_table_t *table)
{
    for (uint32_t i = 0; i < table->capacity; i++) {
        if (table->entries[i].object)
            object_release(table->entries[i].object);
    }
    kfree(table->entries);
    table->entries = NULL;
    table->capacity = table->used = 0;
}

static status_t grow(handle_table_t *table)
{
    uint32_t capacity = table->capacity ? table->capacity * 2 : INITIAL_CAPACITY;
    if (capacity > table->limit)
        capacity = table->limit;
    if (capacity <= table->capacity)
        return STATUS_LIMIT_EXCEEDED;

    handle_entry_t *entries = krealloc(table->entries, capacity * sizeof(handle_entry_t));
    if (!entries)
        return STATUS_OUT_OF_MEMORY;
    memset(entries + table->capacity, 0, (capacity - table->capacity) * sizeof(handle_entry_t));
    table->entries = entries;
    table->capacity = capacity;
    return STATUS_SUCCESS;
}

status_t handle_install(handle_table_t *table, object_t *object, uint32_t rights, handle_t *handle)
{
    if (table->used == table->capacity) {
        status_t status = grow(table);
        if (STATUS_IS_ERROR(status))
            return status;
    }

    for (uint32_t i = 0; i < table->capacity; i++) {
        handle_entry_t *e = &table->entries[i];
        if (e->object)
            continue;
        object_retain(object);
        e->object = object;
        e->rights = rights;
        /* 15-bit generations keep every handle a positive int (BSD-style socket descriptors in libc). */
        e->generation++;
        if (e->generation == 0 || e->generation > 0x7FFF)
            e->generation = 1;
        table->used++;
        *handle = make_handle(i, e->generation);
        return STATUS_SUCCESS;
    }
    return STATUS_LIMIT_EXCEEDED;
}

static handle_entry_t *lookup(handle_table_t *table, handle_t handle)
{
    uint32_t slot = slot_of(handle);

    if (handle == JELLY_HANDLE_INVALID || slot >= table->capacity)
        return NULL;
    handle_entry_t *e = &table->entries[slot];
    if (!e->object || e->generation != (handle >> 16))
        return NULL;
    return e;
}

status_t handle_get(handle_table_t *table, handle_t handle, object_type_t type, uint32_t rights,
                    object_t **object, uint32_t *held_rights)
{
    handle_entry_t *e = lookup(table, handle);

    if (!e || (type && e->object->type != type))
        return STATUS_BAD_HANDLE;
    if ((e->rights & rights) != rights)
        return STATUS_ACCESS_DENIED;

    object_retain(e->object);
    *object = e->object;
    if (held_rights)
        *held_rights = e->rights;
    return STATUS_SUCCESS;
}

status_t handle_close(handle_table_t *table, handle_t handle)
{
    handle_entry_t *e = lookup(table, handle);

    if (!e)
        return STATUS_BAD_HANDLE;
    object_t *object = e->object;
    e->object = NULL;
    e->rights = 0;
    table->used--;
    object_release(object);
    return STATUS_SUCCESS;
}
