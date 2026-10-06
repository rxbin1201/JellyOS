/*
 * Per-process handle tables (README section 17).
 *
 * Userspace never sees kernel pointers, only handles:
 *   bits  0-15: slot index + 1 (0 is never a valid handle)
 *   bits 16-31: slot generation, so a closed handle value stays invalid
 *               after its slot is reused
 * Every entry carries the rights the holder has on the object.
 */

#ifndef CORE_HANDLE_H
#define CORE_HANDLE_H

#include "core/object.h"

#include <jelly/syscall.h>

typedef jelly_handle_t handle_t;

typedef struct {
    object_t *object;   /* NULL: free slot */
    uint32_t  rights;
    uint16_t  generation;
} handle_entry_t;

typedef struct {
    handle_entry_t *entries;
    uint32_t        capacity;
    uint32_t        used;
    uint32_t        limit;    /* resource limit: maximum open handles */
} handle_table_t;

void     handle_table_init(handle_table_t *table, uint32_t limit);

/* Close every handle (releasing the objects) and free the table. */
void     handle_table_destroy(handle_table_t *table);

/* Store a new reference to object. */
status_t handle_install(handle_table_t *table, object_t *object, uint32_t rights, handle_t *handle);

/*
 * Resolve a handle. type 0 accepts any type. Returns a new reference the
 * caller must release. Errors: BAD_HANDLE (invalid or wrong type),
 * ACCESS_DENIED (missing rights).
 */
status_t handle_get(handle_table_t *table, handle_t handle, object_type_t type, uint32_t rights,
                    object_t **object, uint32_t *held_rights);

status_t handle_close(handle_table_t *table, handle_t handle);

#endif
