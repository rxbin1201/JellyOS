#include "process/process.h"

#include "process/elf.h"

#include "core/arch.h"
#include "core/log.h"
#include "core/panic.h"
#include "core/string.h"
#include "memory/heap.h"
#include "memory/layout.h"
#include "scheduler/thread.h"

#include <jelly/syscall.h>

#define DEFAULT_MAX_HANDLES      256
#define DEFAULT_MAX_THREADS      64
#define DEFAULT_MAX_MEMORY_PAGES 16384 /* 64 MiB */

typedef struct {
    list_node_t node;
    uint64_t    address;
    uint64_t    size;
    object_t   *shm;
} mapping_t;

static uint64_t next_pid = 1;

static process_t *process_of(object_t *object)
{
    return container_of(object, process_t, object);
}

static void copy_name(char *dest, size_t capacity, const char *src)
{
    size_t n = 0;
    for (; src && src[n] && n + 1 < capacity; n++)
        dest[n] = src[n];
    dest[n] = '\0';
}

/* Release everything except the process object itself. */
static void finalize(process_t *p)
{
    if (p->exited)
        return;
    p->exited = true;

    handle_table_destroy(&p->handles);
    list_for_each_safe(node, &p->mappings) {
        mapping_t *m = container_of(node, mapping_t, node);
        list_remove(node);
        object_release(m->shm);
        kfree(m);
    }
    if (p->space.root)
        vmm_space_destroy(&p->space);

    if (p->started)
        klog_info("process %lu (%s) exited with code %d%s%s", p->pid, p->name, p->exit_code,
                  p->exit_reason[0] ? ": " : "", p->exit_reason);
    object_notify(&p->object);
}

static void process_destroy(object_t *object)
{
    process_t *p = process_of(object);
    finalize(p);
    kfree(p);
}

static bool process_signaled(object_t *object)
{
    return process_of(object)->exited;
}

static const object_ops_t process_ops = {
    .destroy = process_destroy,
    .signaled = process_signaled,
};

status_t process_create(const char *name, process_t **process)
{
    process_t *p = kcalloc(1, sizeof(*p));
    if (!p)
        return STATUS_OUT_OF_MEMORY;

    object_init(&p->object, OBJECT_PROCESS, &process_ops);
    p->pid = next_pid++;
    copy_name(p->name, sizeof(p->name), name);
    p->credentials = (credentials_t){ UID_ROOT, GID_ROOT };
    p->limits = (process_limits_t){ DEFAULT_MAX_HANDLES, DEFAULT_MAX_THREADS, DEFAULT_MAX_MEMORY_PAGES };
    list_init(&p->threads);
    list_init(&p->mappings);
    p->next_map = USER_MAP_BASE;
    handle_table_init(&p->handles, p->limits.max_handles);

    status_t status = vmm_space_create(&p->space);
    if (STATUS_IS_ERROR(status)) {
        kfree(p);
        return status;
    }
    *process = p;
    return STATUS_SUCCESS;
}

status_t process_load_elf(process_t *p, const void *image, size_t size, uint64_t *entry)
{
    uint64_t pages = 0;
    status_t status = elf_load_user(&p->space, image, size, entry, &pages);

    p->memory_pages += pages;
    return status;
}

status_t process_start(process_t *p, uint64_t entry, uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
    thread_t *main;
    status_t status = vmm_alloc(&p->space, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, VM_USER | VM_WRITE);
    if (STATUS_IS_ERROR(status))
        return status;
    p->memory_pages += USER_STACK_SIZE / PAGE_SIZE;

    /* RSP = 8 mod 16 at entry, as after a call, so entry may be a C function. */
    status = thread_create_user(p, "main", entry, USER_STACK_TOP - 8, arg0, arg1, arg2, &main);
    if (STATUS_IS_ERROR(status))
        return status;

    p->started = true;
    klog_debug("process %lu (%s) started, entry %p", p->pid, p->name, (void *)entry);
    thread_start(main);
    object_release(&main->object);
    return STATUS_SUCCESS;
}

process_t *process_current(void)
{
    thread_t *t = thread_current();
    return t ? t->process : NULL;
}

void process_exit(process_t *p, int32_t code, const char *reason)
{
    if (p->exiting)
        return;
    p->exiting = true;
    p->exit_code = code;
    copy_name(p->exit_reason, sizeof(p->exit_reason), reason);

    list_for_each(node, &p->threads) {
        thread_t *t = container_of(node, thread_t, process_node);
        if (t != thread_current())
            thread_kill(t);
    }
}

void process_exit_current(int32_t code, const char *reason)
{
    process_exit(process_current(), code, reason);
    thread_exit();
}

void process_fault(const char *reason)
{
    process_t *p = process_current();

    ASSERT(p != NULL);
    klog_warn("process %lu (%s) killed: %s", p->pid, p->name, reason);
    process_exit_current(JELLY_EXIT_FAULT, reason);
}

void process_thread_exited(process_t *p)
{
    ASSERT(p->live_threads > 0);
    if (--p->live_threads == 0) {
        p->exiting = true; /* last thread left: exit code stays 0 unless set */
        finalize(p);
    }
}

/* --- Memory ------------------------------------------------------------------ */

status_t process_reserve_range(process_t *p, uint64_t size, uint64_t *address)
{
    size = align_up(size, PAGE_SIZE);
    if (size == 0 || size > USER_MAP_END - p->next_map)
        return STATUS_OUT_OF_MEMORY;

    *address = p->next_map;
    p->next_map += size + PAGE_SIZE; /* leave an unmapped gap between ranges */
    return STATUS_SUCCESS;
}

status_t process_memory_allocate(process_t *p, uint64_t size, uint32_t vm_flags, uint64_t *address)
{
    if (size == 0 || size > USER_MAP_END - USER_MAP_BASE)
        return STATUS_INVALID_ARGUMENT;

    uint64_t pages = align_up(size, PAGE_SIZE) / PAGE_SIZE;
    if (p->memory_pages + pages > p->limits.max_memory_pages)
        return STATUS_LIMIT_EXCEEDED;

    status_t status = process_reserve_range(p, size, address);
    if (!STATUS_IS_ERROR(status))
        status = vmm_alloc(&p->space, *address, pages * PAGE_SIZE, VM_USER | vm_flags);
    if (!STATUS_IS_ERROR(status))
        p->memory_pages += pages;
    return status;
}

status_t process_memory_unmap(process_t *p, uint64_t address, uint64_t size)
{
    if ((address & (PAGE_SIZE - 1)) || size == 0 || address < USER_SPACE_START || address >= USER_SPACE_END ||
        size > USER_SPACE_END - address)
        return STATUS_INVALID_ARGUMENT;
    size = align_up(size, PAGE_SIZE);

    /* Shared memory is unmapped as a whole. */
    list_for_each(node, &p->mappings) {
        mapping_t *m = container_of(node, mapping_t, node);
        if (m->address == address && m->size == size) {
            for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE)
                vmm_unmap(&p->space, address + offset);
            list_remove(node);
            object_release(m->shm);
            kfree(m);
            return STATUS_SUCCESS;
        }
        if (address < m->address + m->size && m->address < address + size)
            return STATUS_INVALID_ARGUMENT;
    }

    for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
        uint64_t phys;
        uint32_t flags;
        if (!vmm_query(&p->space, address + offset, &phys, &flags))
            continue;
        if (flags & VM_OWNED)
            p->memory_pages--;
        vmm_unmap(&p->space, address + offset);
    }
    return STATUS_SUCCESS;
}

status_t process_add_mapping(process_t *p, uint64_t address, uint64_t size, object_t *shm)
{
    mapping_t *m = kmalloc(sizeof(*m));
    if (!m)
        return STATUS_OUT_OF_MEMORY;

    object_retain(shm);
    m->address = address;
    m->size = size;
    m->shm = shm;
    list_push_back(&p->mappings, &m->node);
    return STATUS_SUCCESS;
}
