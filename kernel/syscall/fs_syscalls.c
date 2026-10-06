/*
 * File system calls (syscall ABI version 2).
 *
 * Paths arrive as (pointer, length) and are resolved against the calling
 * process's working directory. Data moves in bounded chunks through kernel
 * buffers, so a single call never pins large amounts of kernel memory.
 */

#include "syscall/internal.h"

#include "core/string.h"
#include "fs/vfs/vfs.h"
#include "memory/heap.h"
#include "process/process.h"
#include "process/usercopy.h"

#include <jelly/syscall.h>

#define IO_CHUNK (64 * 1024)

static const credentials_t *credentials(void)
{
    return &process_current()->credentials;
}

/* Copy a user path and make it absolute and normalized. out has VFS_PATH_MAX bytes. */
static status_t user_path(uint64_t pointer, uint64_t length, char *out)
{
    if (length == 0 || length >= VFS_PATH_MAX)
        return STATUS_INVALID_ARGUMENT;

    char *raw = kmalloc(length);
    if (!raw)
        return STATUS_OUT_OF_MEMORY;
    status_t status = copy_from_user(raw, pointer, length);
    if (!STATUS_IS_ERROR(status))
        status = vfs_normalize(process_current()->cwd, raw, length, out, VFS_PATH_MAX);
    kfree(raw);
    return status;
}

/* Allocate a path buffer and fill it from user memory. */
static status_t take_path(uint64_t pointer, uint64_t length, char **path)
{
    *path = kmalloc(VFS_PATH_MAX);
    if (!*path)
        return STATUS_OUT_OF_MEMORY;
    status_t status = user_path(pointer, length, *path);
    if (STATUS_IS_ERROR(status)) {
        kfree(*path);
        *path = NULL;
    }
    return status;
}

static status_t get_file(uint64_t handle, uint32_t rights, file_t **file)
{
    object_t *object;
    status_t status = handle_get(syscall_handles(), (handle_t)handle, OBJECT_FILE, rights, &object, NULL);
    if (!STATUS_IS_ERROR(status))
        *file = container_of(object, file_t, object);
    return status;
}

/* --- Open files ---------------------------------------------------------------------- */

status_t sys_file_open(const uint64_t *a)
{
    char *path;
    file_t *file;
    uint32_t flags = (uint32_t)a[2];

    if (!user_range_ok(a[4], sizeof(handle_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = take_path(a[0], a[1], &path);
    if (STATUS_IS_ERROR(status))
        return status;
    status = vfs_open(path, flags, (uint32_t)a[3], credentials(), &file);
    kfree(path);
    if (STATUS_IS_ERROR(status))
        return status;

    uint32_t rights = JELLY_RIGHT_DUPLICATE | ((flags & JELLY_OPEN_READ) ? JELLY_RIGHT_READ : 0) |
                      ((flags & JELLY_OPEN_WRITE) ? JELLY_RIGHT_WRITE : 0);
    return syscall_give_handle(&file->object, rights, a[4]);
}

status_t sys_file_read(const uint64_t *a)
{
    file_t *file;
    uint64_t buffer = a[1], size = a[2], total = 0;

    if (!user_range_ok(a[3], sizeof(uint64_t), true) || (size && !user_range_ok(buffer, size, true)))
        return STATUS_INVALID_ARGUMENT;
    status_t status = get_file(a[0], JELLY_RIGHT_READ, &file);
    if (STATUS_IS_ERROR(status))
        return status;

    uint8_t *chunk = kmalloc(IO_CHUNK);
    if (!chunk)
        status = STATUS_OUT_OF_MEMORY;
    while (!STATUS_IS_ERROR(status) && total < size) {
        size_t want = size - total < IO_CHUNK ? size - total : IO_CHUNK, done;
        status = vfs_read(file, chunk, want, &done);
        if (!STATUS_IS_ERROR(status))
            status = copy_to_user(buffer + total, chunk, done);
        total += done;
        if (done < want)
            break; /* end of file */
    }
    kfree(chunk);
    object_release(&file->object);
    return STATUS_IS_ERROR(status) ? status : put_user_u64(a[3], total);
}

status_t sys_file_write(const uint64_t *a)
{
    file_t *file;
    uint64_t buffer = a[1], size = a[2], total = 0;

    if (!user_range_ok(a[3], sizeof(uint64_t), true) || (size && !user_range_ok(buffer, size, false)))
        return STATUS_INVALID_ARGUMENT;
    status_t status = get_file(a[0], JELLY_RIGHT_WRITE, &file);
    if (STATUS_IS_ERROR(status))
        return status;

    uint8_t *chunk = kmalloc(IO_CHUNK);
    if (!chunk)
        status = STATUS_OUT_OF_MEMORY;
    while (!STATUS_IS_ERROR(status) && total < size) {
        size_t want = size - total < IO_CHUNK ? size - total : IO_CHUNK, done = 0;
        status = copy_from_user(chunk, buffer + total, want);
        if (!STATUS_IS_ERROR(status))
            status = vfs_write(file, chunk, want, &done);
        total += done;
    }
    kfree(chunk);
    object_release(&file->object);
    /* Report partial progress even if the device filled up. */
    status_t reported = put_user_u64(a[3], total);
    return STATUS_IS_ERROR(status) ? status : reported;
}

status_t sys_file_seek(const uint64_t *a)
{
    file_t *file;
    uint64_t position;

    if (!user_range_ok(a[3], sizeof(uint64_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = get_file(a[0], 0, &file);
    if (STATUS_IS_ERROR(status))
        return status;
    status = vfs_seek(file, (int64_t)a[1], (uint32_t)a[2], &position);
    object_release(&file->object);
    return STATUS_IS_ERROR(status) ? status : put_user_u64(a[3], position);
}

status_t sys_file_truncate(const uint64_t *a)
{
    file_t *file;
    status_t status = get_file(a[0], JELLY_RIGHT_WRITE, &file);
    if (STATUS_IS_ERROR(status))
        return status;
    status = vfs_truncate(file, a[1]);
    object_release(&file->object);
    return status;
}

status_t sys_file_stat(const uint64_t *a)
{
    file_t *file;
    jelly_stat_t stat;

    status_t status = get_file(a[0], 0, &file);
    if (STATUS_IS_ERROR(status))
        return status;
    status = vfs_fstat(file, &stat);
    object_release(&file->object);
    return STATUS_IS_ERROR(status) ? status : copy_to_user(a[1], &stat, sizeof(stat));
}

status_t sys_directory_read(const uint64_t *a)
{
    file_t *file;
    jelly_dirent_t *entry = kmalloc(sizeof(*entry));

    if (!entry)
        return STATUS_OUT_OF_MEMORY;
    status_t status = user_range_ok(a[1], sizeof(*entry), true) ? get_file(a[0], JELLY_RIGHT_READ, &file)
                                                                  : STATUS_INVALID_ARGUMENT;
    if (!STATUS_IS_ERROR(status)) {
        status = vfs_readdir(file, entry);
        object_release(&file->object);
    }
    if (!STATUS_IS_ERROR(status))
        status = copy_to_user(a[1], entry, sizeof(*entry));
    kfree(entry);
    return status;
}

/* --- Paths ------------------------------------------------------------------------------ */

status_t sys_path_stat(const uint64_t *a)
{
    char *path;
    jelly_stat_t stat;

    if (!user_range_ok(a[3], sizeof(stat), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = take_path(a[0], a[1], &path);
    if (STATUS_IS_ERROR(status))
        return status;
    status = vfs_stat(path, credentials(), !(a[2] & JELLY_STAT_NOFOLLOW), &stat);
    kfree(path);
    return STATUS_IS_ERROR(status) ? status : copy_to_user(a[3], &stat, sizeof(stat));
}

status_t sys_path_mkdir(const uint64_t *a)
{
    char *path;
    status_t status = take_path(a[0], a[1], &path);
    if (STATUS_IS_ERROR(status))
        return status;
    status = vfs_mkdir(path, (uint32_t)a[2], credentials());
    kfree(path);
    return status;
}

status_t sys_path_unlink(const uint64_t *a)
{
    char *path;
    status_t status = take_path(a[0], a[1], &path);
    if (STATUS_IS_ERROR(status))
        return status;
    status = vfs_unlink(path, credentials());
    kfree(path);
    return status;
}

status_t sys_path_rename(const uint64_t *a)
{
    char *from, *to;
    status_t status = take_path(a[0], a[1], &from);
    if (STATUS_IS_ERROR(status))
        return status;
    status = take_path(a[2], a[3], &to);
    if (!STATUS_IS_ERROR(status)) {
        status = vfs_rename(from, to, credentials());
        kfree(to);
    }
    kfree(from);
    return status;
}

status_t sys_path_symlink(const uint64_t *a)
{
    char *path, *target;

    if (a[1] == 0 || a[1] >= VFS_PATH_MAX)
        return STATUS_INVALID_ARGUMENT;
    target = kcalloc(1, a[1] + 1); /* stored as given, not normalized */
    if (!target)
        return STATUS_OUT_OF_MEMORY;
    status_t status = copy_from_user(target, a[0], a[1]);
    if (!STATUS_IS_ERROR(status) && strlen(target) != a[1])
        status = STATUS_INVALID_ARGUMENT; /* embedded NUL */
    if (!STATUS_IS_ERROR(status))
        status = take_path(a[2], a[3], &path);
    if (!STATUS_IS_ERROR(status)) {
        status = vfs_symlink(target, path, credentials());
        kfree(path);
    }
    kfree(target);
    return status;
}

status_t sys_path_readlink(const uint64_t *a)
{
    char *path, *link = kmalloc(VFS_PATH_MAX);
    size_t length;

    if (!link)
        return STATUS_OUT_OF_MEMORY;
    status_t status = user_range_ok(a[4], sizeof(uint64_t), true) ? take_path(a[0], a[1], &path)
                                                                     : STATUS_INVALID_ARGUMENT;
    if (!STATUS_IS_ERROR(status)) {
        status = vfs_readlink(path, credentials(), link, VFS_PATH_MAX, &length);
        kfree(path);
    }
    if (!STATUS_IS_ERROR(status) && length > a[3]) {
        put_user_u64(a[4], length);
        status = STATUS_BUFFER_TOO_SMALL;
    }
    if (!STATUS_IS_ERROR(status))
        status = copy_to_user(a[2], link, length);
    if (!STATUS_IS_ERROR(status))
        status = put_user_u64(a[4], length);
    kfree(link);
    return status;
}

/* --- Working directory ------------------------------------------------------------------- */

status_t sys_chdir(const uint64_t *a)
{
    char *path;
    jelly_stat_t stat;

    status_t status = take_path(a[0], a[1], &path);
    if (STATUS_IS_ERROR(status))
        return status;
    status = vfs_stat(path, credentials(), true, &stat);
    if (!STATUS_IS_ERROR(status) && stat.type != JELLY_FILE_TYPE_DIRECTORY)
        status = STATUS_NOT_DIRECTORY;
    if (!STATUS_IS_ERROR(status) && strlen(path) >= PROCESS_CWD_MAX)
        status = STATUS_INVALID_ARGUMENT;
    if (!STATUS_IS_ERROR(status))
        memcpy(process_current()->cwd, path, strlen(path) + 1);
    kfree(path);
    return status;
}

status_t sys_getcwd(const uint64_t *a)
{
    const char *cwd = process_current()->cwd;
    size_t length = strlen(cwd);

    if (!user_range_ok(a[2], sizeof(uint64_t), true))
        return STATUS_INVALID_ARGUMENT;
    if (a[1] < length + 1) {
        put_user_u64(a[2], length);
        return STATUS_BUFFER_TOO_SMALL;
    }
    status_t status = copy_to_user(a[0], cwd, length + 1);
    return STATUS_IS_ERROR(status) ? status : put_user_u64(a[2], length);
}

/* --- File systems ------------------------------------------------------------------------- */

status_t sys_fs_sync(const uint64_t *a)
{
    (void)a;
    return vfs_sync();
}

/* Copy a short name (device, file system type) from user memory. */
static status_t user_name(uint64_t pointer, uint64_t length, char *out, size_t capacity)
{
    if (length == 0 || length >= capacity)
        return STATUS_INVALID_ARGUMENT;
    status_t status = copy_from_user(out, pointer, length);
    out[length] = '\0';
    return STATUS_IS_ERROR(status) || strlen(out) != length ? STATUS_INVALID_ARGUMENT : STATUS_SUCCESS;
}

status_t sys_mount(const uint64_t *a)
{
    char device_name[BLOCK_NAME_MAX], type[32], *path;

    if (credentials()->uid != UID_ROOT)
        return STATUS_ACCESS_DENIED;
    status_t status = user_name(a[2], a[3], device_name, sizeof(device_name));
    if (!STATUS_IS_ERROR(status) && a[5])
        status = user_name(a[4], a[5], type, sizeof(type));
    if (STATUS_IS_ERROR(status))
        return status;

    block_device_t *device = block_find(device_name);
    if (!device)
        return STATUS_NOT_FOUND;
    status = take_path(a[0], a[1], &path);
    if (STATUS_IS_ERROR(status))
        return status;
    status = vfs_mount(path, device, a[5] ? type : NULL);
    kfree(path);
    return status;
}

status_t sys_unmount(const uint64_t *a)
{
    char *path;

    if (credentials()->uid != UID_ROOT)
        return STATUS_ACCESS_DENIED;
    status_t status = take_path(a[0], a[1], &path);
    if (STATUS_IS_ERROR(status))
        return status;
    status = vfs_unmount(path);
    kfree(path);
    return status;
}
