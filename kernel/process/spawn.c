#include "process/spawn.h"

#include "core/log.h"
#include "core/string.h"
#include "fs/vfs/vfs.h"
#include "memory/heap.h"
#include "memory/layout.h"

static status_t read_image(const char *path, const credentials_t *cred, void **image, size_t *size)
{
    file_t *file;
    status_t status = vfs_open(path, JELLY_OPEN_READ | VFS_OPEN_EXEC, 0, cred, &file);
    if (STATUS_IS_ERROR(status))
        return status;

    uint64_t length = file->vnode->size;
    uint8_t *buffer = NULL;
    size_t total = 0;

    if (length == 0 || length > SPAWN_MAX_IMAGE)
        status = STATUS_INVALID_ARGUMENT;
    else if (!(buffer = kmalloc(length)))
        status = STATUS_OUT_OF_MEMORY;

    while (!STATUS_IS_ERROR(status) && total < length) {
        size_t done;
        status = vfs_read(file, buffer + total, length - total, &done);
        if (!STATUS_IS_ERROR(status) && done == 0)
            status = STATUS_IO_ERROR; /* the file shrank */
        total += done;
    }
    object_release(&file->object);

    if (STATUS_IS_ERROR(status)) {
        kfree(buffer);
        return status;
    }
    *image = buffer;
    *size = length;
    return STATUS_SUCCESS;
}

static const char *base_name(const char *path)
{
    const char *name = path;
    for (; *path; path++) {
        if (*path == '/' && path[1])
            name = path + 1;
    }
    return name;
}

/*
 * Stack layout written below the top (highest address first):
 *   argument and environment strings
 *   envp[] (NULL-terminated), argv[] (NULL-terminated)
 *   jelly_startup_t
 * The entry RSP is just below, 8 mod 16 as after a call.
 */
static status_t write_startup(process_t *p, const spawn_request_t *r, const handle_t *handles, uint64_t top,
                              uint64_t *startup_address, uint64_t *sp)
{
    size_t strings = 0;
    for (uint32_t i = 0; i < r->argc; i++)
        strings += strlen(r->argv[i]) + 1;
    for (uint32_t i = 0; i < r->envc; i++)
        strings += strlen(r->envp[i]) + 1;

    uint64_t pointers = ((uint64_t)r->argc + 1 + r->envc + 1) * sizeof(uint64_t);
    uint64_t needed = strings + pointers + sizeof(jelly_startup_t) + 64;
    if (needed > USER_STACK_SIZE / 2)
        return STATUS_LIMIT_EXCEEDED;

    uint64_t string_area = top - align_up(strings, 16);
    uint64_t array_area = string_area - align_up(pointers, 16);
    uint64_t startup = align_down(array_area - sizeof(jelly_startup_t), 16);
    uint64_t *array = kcalloc(r->argc + 1 + r->envc + 1, sizeof(uint64_t));
    if (!array)
        return STATUS_OUT_OF_MEMORY;

    status_t status = STATUS_SUCCESS;
    uint64_t cursor = string_area;
    for (uint32_t i = 0; i < r->argc + r->envc && !STATUS_IS_ERROR(status); i++) {
        const char *s = i < r->argc ? r->argv[i] : r->envp[i - r->argc];
        size_t length = strlen(s) + 1;
        status = process_copy_to(p, cursor, s, length);
        array[i < r->argc ? i : i + 1] = cursor; /* skip argv's NULL terminator slot */
        cursor += length;
    }
    if (!STATUS_IS_ERROR(status))
        status = process_copy_to(p, array_area, array, pointers);
    kfree(array);

    jelly_startup_t block = {
        .version = JELLY_STARTUP_VERSION,
        .argc = r->argc,
        .argv = (char **)(uintptr_t)array_area,
        .envc = r->envc,
        .envp = (char **)(uintptr_t)(array_area + ((uint64_t)r->argc + 1) * sizeof(uint64_t)),
        .handle_count = r->handle_count,
    };
    memcpy(block.handles, handles, r->handle_count * sizeof(handle_t));
    if (!STATUS_IS_ERROR(status))
        status = process_copy_to(p, startup, &block, sizeof(block));

    *startup_address = startup;
    *sp = startup - 8;
    return status;
}

status_t process_spawn(const spawn_request_t *r, process_t **result)
{
    void *image;
    size_t size;
    uint64_t entry, top, startup, sp;
    handle_t handles[JELLY_SPAWN_MAX_HANDLES] = { 0 };
    process_t *p;

    if (r->handle_count > JELLY_SPAWN_MAX_HANDLES || r->argc > JELLY_SPAWN_MAX_STRINGS ||
        r->envc > JELLY_SPAWN_MAX_STRINGS)
        return STATUS_INVALID_ARGUMENT;

    status_t status = read_image(r->path, r->credentials, &image, &size);
    if (STATUS_IS_ERROR(status))
        return status;

    status = process_create(base_name(r->path), &p);
    if (STATUS_IS_ERROR(status)) {
        kfree(image);
        return status;
    }
    p->credentials = *r->credentials;
    if (r->cwd && strlen(r->cwd) < PROCESS_CWD_MAX)
        memcpy(p->cwd, r->cwd, strlen(r->cwd) + 1);

    status = process_load_elf(p, image, size, &entry);
    kfree(image);

    for (uint32_t i = 0; i < r->handle_count && !STATUS_IS_ERROR(status); i++)
        status = handle_install(&p->handles, r->objects[i], r->rights[i], &handles[i]);
    if (!STATUS_IS_ERROR(status))
        status = process_allocate_stack(p, &top);
    if (!STATUS_IS_ERROR(status))
        status = write_startup(p, r, handles, top, &startup, &sp);
    if (!STATUS_IS_ERROR(status))
        status = process_start_thread(p, entry, sp, startup, 0, 0);

    if (STATUS_IS_ERROR(status)) {
        object_release(&p->object);
        return status;
    }
    *result = p;
    return STATUS_SUCCESS;
}
