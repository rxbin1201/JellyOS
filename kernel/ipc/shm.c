/*
 * Shared memory objects: zeroed frames owned by the object. Mappings do not
 * own the frames (no VM_OWNED); each mapping keeps the object alive.
 *
 * Device memory objects describe a physical range that belongs to a device
 * (a framebuffer). They are mapped write-combining and never free the
 * memory; a release callback tells the device when the last user is gone.
 */

#include "ipc/ipc.h"

#include "core/string.h"
#include "memory/heap.h"
#include "memory/layout.h"
#include "memory/pmm.h"

typedef struct {
    object_t  object;
    uint64_t  pages;
    uint64_t *frames;          /* NULL for device memory */
    uint64_t  device_phys;
    void    (*release)(void *context);
    void     *context;
} shm_t;

static shm_t *shm_of(object_t *object)
{
    return container_of(object, shm_t, object);
}

static void shm_destroy(object_t *object)
{
    shm_t *s = shm_of(object);

    for (uint64_t i = 0; s->frames && i < s->pages; i++) {
        if (s->frames[i])
            pmm_free_page(s->frames[i]);
    }
    if (s->release)
        s->release(s->context);
    kfree(s->frames);
    kfree(s);
}

static const object_ops_t shm_ops = {
    .destroy = shm_destroy,
};

status_t shm_create(uint64_t size, object_t **shm)
{
    if (size == 0 || size > SHM_SIZE_MAX)
        return STATUS_INVALID_ARGUMENT;

    shm_t *s = kcalloc(1, sizeof(*s));
    if (!s)
        return STATUS_OUT_OF_MEMORY;
    object_init(&s->object, OBJECT_SHARED_MEMORY, &shm_ops);
    s->pages = align_up(size, PAGE_SIZE) / PAGE_SIZE;
    s->frames = kcalloc(s->pages, sizeof(uint64_t));
    if (!s->frames) {
        kfree(s);
        return STATUS_OUT_OF_MEMORY;
    }

    for (uint64_t i = 0; i < s->pages; i++) {
        if (STATUS_IS_ERROR(pmm_alloc_page(&s->frames[i]))) {
            s->frames[i] = 0;
            object_release(&s->object);
            return STATUS_OUT_OF_MEMORY;
        }
        memset(phys_to_virt(s->frames[i]), 0, PAGE_SIZE);
    }
    *shm = &s->object;
    return STATUS_SUCCESS;
}

status_t shm_create_device(uint64_t phys, uint64_t size, void (*release)(void *context), void *context,
                           object_t **shm)
{
    if (size == 0 || (phys & (PAGE_SIZE - 1)))
        return STATUS_INVALID_ARGUMENT;
    shm_t *s = kcalloc(1, sizeof(*s));
    if (!s)
        return STATUS_OUT_OF_MEMORY;
    object_init(&s->object, OBJECT_SHARED_MEMORY, &shm_ops);
    s->pages = align_up(size, PAGE_SIZE) / PAGE_SIZE;
    s->device_phys = phys;
    s->release = release;
    s->context = context;
    *shm = &s->object;
    return STATUS_SUCCESS;
}

uint64_t shm_size(object_t *shm)
{
    return shm_of(shm)->pages * PAGE_SIZE;
}

status_t shm_map(process_t *process, object_t *shm, uint32_t vm_flags, uint64_t *address)
{
    shm_t *s = shm_of(shm);
    uint64_t base, size = s->pages * PAGE_SIZE;

    status_t status = process_reserve_range(process, size, &base);
    if (STATUS_IS_ERROR(status))
        return status;

    if (!s->frames)
        vm_flags |= VM_WRITE_COMBINING;
    for (uint64_t i = 0; i < s->pages; i++) {
        uint64_t frame = s->frames ? s->frames[i] : s->device_phys + i * PAGE_SIZE;
        status = vmm_map(&process->space, base + i * PAGE_SIZE, frame, VM_USER | vm_flags);
        if (STATUS_IS_ERROR(status)) {
            for (uint64_t j = 0; j < i; j++)
                vmm_unmap(&process->space, base + j * PAGE_SIZE);
            return status;
        }
    }

    status = process_add_mapping(process, base, size, shm);
    if (STATUS_IS_ERROR(status)) {
        for (uint64_t i = 0; i < s->pages; i++)
            vmm_unmap(&process->space, base + i * PAGE_SIZE);
        return status;
    }
    *address = base;
    return STATUS_SUCCESS;
}
