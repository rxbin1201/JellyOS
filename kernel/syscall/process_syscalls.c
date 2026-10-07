/*
 * Program, pipe and power system calls (syscall ABI version 3).
 */

#include "syscall/internal.h"

#include "core/string.h"
#include "fs/vfs/vfs.h"
#include "ipc/ipc.h"
#include "memory/heap.h"
#include "power/power.h"
#include "process/spawn.h"
#include "process/usercopy.h"

#include <jelly/syscall.h>

#define STRING_MAX 4096

/* Copy a NUL-terminated user string (at most STRING_MAX bytes including the NUL). */
static status_t copy_string(uint64_t pointer, char **out, size_t *budget)
{
    char *s = kmalloc(STRING_MAX);
    if (!s)
        return STATUS_OUT_OF_MEMORY;

    size_t length = 0;
    for (;;) {
        /* Copy up to the end of the current page so an unmapped next page is never touched. */
        size_t chunk = 4096 - ((pointer + length) & 4095);
        if (chunk > STRING_MAX - length)
            chunk = STRING_MAX - length;
        if (chunk == 0 || STATUS_IS_ERROR(copy_from_user(s + length, pointer + length, chunk))) {
            kfree(s);
            return STATUS_INVALID_ARGUMENT;
        }
        for (size_t i = 0; i < chunk; i++) {
            if (s[length + i] == '\0') {
                length += i;
                if (length + 1 > *budget) {
                    kfree(s);
                    return STATUS_LIMIT_EXCEEDED;
                }
                *budget -= length + 1;
                *out = s;
                return STATUS_SUCCESS;
            }
        }
        length += chunk;
    }
}

static status_t copy_string_array(uint64_t pointer, uint32_t count, char ***out, size_t *budget)
{
    char **array = kcalloc(count + 1, sizeof(char *));
    uint64_t *pointers = kcalloc(count + 1, sizeof(uint64_t));
    status_t status = (array && pointers) ? copy_from_user(pointers, pointer, count * sizeof(uint64_t))
                                          : STATUS_OUT_OF_MEMORY;

    for (uint32_t i = 0; i < count && !STATUS_IS_ERROR(status); i++)
        status = copy_string(pointers[i], &array[i], budget);
    kfree(pointers);
    if (STATUS_IS_ERROR(status)) {
        for (uint32_t i = 0; array && i < count; i++)
            kfree(array[i]);
        kfree(array);
        return status;
    }
    *out = array;
    return STATUS_SUCCESS;
}

static void free_string_array(char **array, uint32_t count)
{
    for (uint32_t i = 0; array && i < count; i++)
        kfree(array[i]);
    kfree(array);
}

/* Spawn with the caller's credentials, or (SYS_PROCESS_SPAWN_AS, root only) with others. */
static status_t spawn(uint64_t user_request, const credentials_t *as, uint64_t user_out)
{
    const uint64_t a[2] = { user_request, user_out };
    jelly_spawn_t request;
    spawn_request_t r = { 0 };
    char *path = kmalloc(VFS_PATH_MAX), *raw = NULL;
    char **argv = NULL, **envp = NULL;
    size_t budget = JELLY_SPAWN_MAX_BYTES;
    process_t *self = process_current(), *child;

    status_t status = path ? copy_from_user(&request, a[0], sizeof(request)) : STATUS_OUT_OF_MEMORY;
    if (!STATUS_IS_ERROR(status) &&
        (request.flags || request.handle_count > JELLY_SPAWN_MAX_HANDLES || request.argc > JELLY_SPAWN_MAX_STRINGS ||
         request.envc > JELLY_SPAWN_MAX_STRINGS || request.path_length == 0 || request.path_length >= VFS_PATH_MAX ||
         !user_range_ok(a[1], sizeof(handle_t), true)))
        status = STATUS_INVALID_ARGUMENT;

    if (!STATUS_IS_ERROR(status) && !(raw = kmalloc(request.path_length)))
        status = STATUS_OUT_OF_MEMORY;
    if (!STATUS_IS_ERROR(status))
        status = copy_from_user(raw, (uint64_t)(uintptr_t)request.path, request.path_length);
    if (!STATUS_IS_ERROR(status))
        status = vfs_normalize(self->cwd, raw, request.path_length, path, VFS_PATH_MAX);
    if (!STATUS_IS_ERROR(status))
        status = copy_string_array((uint64_t)(uintptr_t)request.argv, request.argc, &argv, &budget);
    if (!STATUS_IS_ERROR(status))
        status = copy_string_array((uint64_t)(uintptr_t)request.envp, request.envc, &envp, &budget);

    /* The child gets the same objects with the same rights as the caller's handles. */
    for (uint32_t i = 0; i < request.handle_count && !STATUS_IS_ERROR(status); i++) {
        status = handle_get(syscall_handles(), request.handles[i], 0, 0, &r.objects[i], &r.rights[i]);
        if (!STATUS_IS_ERROR(status))
            r.handle_count = i + 1;
    }

    if (!STATUS_IS_ERROR(status)) {
        r.path = path;
        r.argv = argv;
        r.argc = request.argc;
        r.envp = envp;
        r.envc = request.envc;
        r.credentials = as ? as : &self->credentials;
        r.cwd = self->cwd;
        status = process_spawn(&r, &child);
    }
    if (!STATUS_IS_ERROR(status))
        status = syscall_give_handle(&child->object, JELLY_RIGHT_WAIT | JELLY_RIGHT_MANAGE | JELLY_RIGHT_DUPLICATE,
                                     a[1]);

    for (uint32_t i = 0; i < r.handle_count; i++)
        object_release(r.objects[i]);
    free_string_array(argv, request.argc);
    free_string_array(envp, request.envc);
    kfree(raw);
    kfree(path);
    return status;
}

status_t sys_process_spawn(const uint64_t *a)
{
    return spawn(a[0], NULL, a[1]);
}

status_t sys_process_spawn_as(const uint64_t *a)
{
    if (process_current()->credentials.uid != UID_ROOT)
        return STATUS_ACCESS_DENIED;
    credentials_t as = { (uint32_t)a[1], (uint32_t)a[2] };
    return spawn(a[0], &as, a[3]);
}

status_t sys_process_info(const uint64_t *a)
{
    object_t *object;
    jelly_process_info_t info = { 0 };

    status_t status = handle_get(syscall_handles(), (handle_t)a[0], OBJECT_PROCESS, 0, &object, NULL);
    if (STATUS_IS_ERROR(status))
        return status;

    process_t *p = container_of(object, process_t, object);
    info.pid = p->pid;
    info.state = p->exited ? JELLY_PROCESS_EXITED : JELLY_PROCESS_RUNNING;
    info.exit_code = p->exited ? p->exit_code : 0;
    memcpy(info.name, p->name, sizeof(info.name) < sizeof(p->name) ? sizeof(info.name) : sizeof(p->name));
    info.name[sizeof(info.name) - 1] = '\0';
    object_release(object);
    return copy_to_user(a[1], &info, sizeof(info));
}

status_t sys_process_kill(const uint64_t *a)
{
    object_t *object;
    status_t status = handle_get(syscall_handles(), (handle_t)a[0], OBJECT_PROCESS, JELLY_RIGHT_MANAGE, &object, NULL);
    if (STATUS_IS_ERROR(status))
        return status;

    process_t *p = container_of(object, process_t, object);
    if (p == process_current())
        status = STATUS_INVALID_ARGUMENT; /* use SYS_PROCESS_EXIT */
    else if (p->critical)
        status = STATUS_ACCESS_DENIED;
    else
        process_exit(p, (int32_t)a[1], "killed");
    object_release(object);
    return status;
}

status_t sys_pipe_create(const uint64_t *a)
{
    file_t *read_end, *write_end;

    if (!user_range_ok(a[0], sizeof(handle_t), true) || !user_range_ok(a[1], sizeof(handle_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = pipe_create(&read_end, &write_end);
    if (STATUS_IS_ERROR(status))
        return status;

    status = syscall_give_handle(&read_end->object, JELLY_RIGHT_READ | JELLY_RIGHT_DUPLICATE, a[0]);
    if (STATUS_IS_ERROR(status)) {
        object_release(&write_end->object);
        return status;
    }
    return syscall_give_handle(&write_end->object, JELLY_RIGHT_WRITE | JELLY_RIGHT_DUPLICATE, a[1]);
}

status_t sys_system_power(const uint64_t *a)
{
    if (process_current()->credentials.uid != UID_ROOT)
        return STATUS_ACCESS_DENIED;
    switch (a[0]) {
    case JELLY_POWER_OFF:    return power_off();
    case JELLY_POWER_REBOOT: return power_reboot();
    }
    return STATUS_INVALID_ARGUMENT;
}
