#include "fs/vfs/vfs.h"

#include "core/format.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "scheduler/mutex.h"

#define MAY_READ  4u
#define MAY_WRITE 2u
#define MAY_EXEC  1u

/* One lock for the whole VFS: simple and sufficient on one CPU (finer locking with SMP). */
static mutex_t lock;
static list_t types = { { &types.head, &types.head } };
static list_t mounts = { { &mounts.head, &mounts.head } };
static filesystem_t *root_fs;

extern fs_type_t fat_type;
extern fs_type_t exfat_type;

/* --- Vnodes --------------------------------------------------------------------- */

void vnode_init(vnode_t *vnode, filesystem_t *fs, vnode_type_t type, const vnode_ops_t *ops)
{
    memset(vnode, 0, sizeof(*vnode));
    vnode->refs = 1;
    vnode->type = type;
    vnode->fs = fs;
    vnode->ops = ops;
    vnode->mode = type == VNODE_DIRECTORY ? 0755 : 0644;
    if (fs)
        fs->live_vnodes++;
}

void vnode_retain(vnode_t *vnode)
{
    if (vnode->refs++ == 0 && vnode->fs)
        vnode->fs->live_vnodes++;
}

void vnode_release(vnode_t *vnode)
{
    if (--vnode->refs)
        return;
    if (vnode->fs)
        vnode->fs->live_vnodes--;
    if (vnode->ops->release)
        vnode->ops->release(vnode);
}

static void fill_stat(const vnode_t *v, vfs_stat_t *stat)
{
    stat->type = v->type;
    stat->mode = v->mode & 0777;
    stat->uid = v->uid;
    stat->gid = v->gid;
    stat->size = v->size;
    stat->inode = v->inode;
}

static bool permitted(const vnode_t *v, const credentials_t *cred, uint32_t want)
{
    if (!cred || cred->uid == UID_ROOT)
        return true;
    uint32_t bits = cred->uid == v->uid ? v->mode >> 6 : cred->gid == v->gid ? v->mode >> 3 : v->mode;
    return (bits & want) == want;
}

/* --- Path normalization ---------------------------------------------------------- */

static status_t append_components(const char *path, size_t length, char *out, size_t *used, size_t capacity)
{
    size_t i = 0;

    while (i < length) {
        while (i < length && path[i] == '/')
            i++;
        size_t start = i;
        while (i < length && path[i] != '/') {
            if (path[i] == '\0')
                return STATUS_INVALID_ARGUMENT;
            i++;
        }
        size_t n = i - start;
        if (n == 0 || (n == 1 && path[start] == '.'))
            continue;
        if (n > VFS_NAME_MAX)
            return STATUS_INVALID_ARGUMENT;
        if (n == 2 && path[start] == '.' && path[start + 1] == '.') {
            while (*used > 1 && out[*used - 1] != '/')
                (*used)--;
            if (*used > 1)
                (*used)--; /* drop the separator too, "/" stays */
            continue;
        }
        if (*used + (*used > 1) + n + 1 > capacity)
            return STATUS_INVALID_ARGUMENT;
        if (*used > 1)
            out[(*used)++] = '/';
        memcpy(out + *used, path + start, n);
        *used += n;
    }
    return STATUS_SUCCESS;
}

status_t vfs_normalize(const char *base, const char *path, size_t length, char *out, size_t capacity)
{
    size_t used = 1;

    if (length == 0 || capacity < 2)
        return STATUS_INVALID_ARGUMENT;
    out[0] = '/';

    status_t status = STATUS_SUCCESS;
    if (path[0] != '/' && base)
        status = append_components(base, strlen(base), out, &used, capacity);
    if (!STATUS_IS_ERROR(status))
        status = append_components(path, length, out, &used, capacity);
    out[used] = '\0';
    return status;
}

/* --- Path resolution -------------------------------------------------------------- */

static vnode_t *enter_mounts(vnode_t *v)
{
    while (v->mounted) {
        vnode_t *root = v->mounted->fs->root;
        vnode_retain(root);
        vnode_release(v);
        v = root;
    }
    return v;
}

/* Resolve a normalized absolute path. Returns a new reference. Lock held. */
static status_t walk(const char *path, const credentials_t *cred, bool follow_last, vnode_t **result)
{
    char *buffer = kmalloc(VFS_PATH_MAX);
    char *joined = kmalloc(VFS_PATH_MAX);
    char *target = kmalloc(VFS_PATH_MAX);
    status_t status = (buffer && joined && target) ? STATUS_SUCCESS : STATUS_OUT_OF_MEMORY;
    unsigned links = 0;
    vnode_t *v = NULL;

    /* Every entry point accepts any absolute path: normalize here instead of trusting callers. */
    if (!STATUS_IS_ERROR(status))
        status = path[0] == '/' ? vfs_normalize(NULL, path, strlen(path), buffer, VFS_PATH_MAX)
                                : STATUS_INVALID_ARGUMENT;

restart:
    if (STATUS_IS_ERROR(status))
        goto out;
    v = enter_mounts((vnode_retain(root_fs->root), root_fs->root));

    for (const char *p = buffer + 1; *p;) {
        const char *component = p;
        size_t n = 0;
        while (component[n] && component[n] != '/')
            n++;
        const char *next = component + n + (component[n] == '/');

        if (v->type != VNODE_DIRECTORY) {
            status = STATUS_NOT_DIRECTORY;
            goto out;
        }
        if (!permitted(v, cred, MAY_EXEC)) {
            status = STATUS_ACCESS_DENIED;
            goto out;
        }

        vnode_t *child;
        status = v->ops->lookup(v, component, n, &child);
        if (STATUS_IS_ERROR(status))
            goto out;
        child = enter_mounts(child);

        if (child->type == VNODE_SYMLINK && (*next || follow_last)) {
            size_t length;
            if (++links > VFS_SYMLINK_DEPTH) {
                status = STATUS_LIMIT_EXCEEDED;
            } else if (!child->ops->readlink) {
                status = STATUS_NOT_SUPPORTED;
            } else {
                status = child->ops->readlink(child, target, VFS_PATH_MAX, &length);
            }
            vnode_release(child);
            if (STATUS_IS_ERROR(status))
                goto out;

            /* Continue at target (relative to the link's directory), then the rest of the path. */
            size_t dir_length = (size_t)(component - buffer) - 1;
            char saved = buffer[dir_length ? dir_length : 1];
            buffer[dir_length ? dir_length : 1] = '\0';
            status = vfs_normalize(buffer, target, length, joined, VFS_PATH_MAX);
            buffer[dir_length ? dir_length : 1] = saved;
            if (!STATUS_IS_ERROR(status) && *next) {
                size_t used = strlen(joined);
                status = append_components(next, strlen(next), joined, &used, VFS_PATH_MAX);
                joined[used] = '\0';
            }
            if (!STATUS_IS_ERROR(status))
                memcpy(buffer, joined, strlen(joined) + 1);
            vnode_release(v);
            v = NULL;
            goto restart;
        }

        vnode_release(v);
        v = child;
        p = next;
    }

out:
    if (STATUS_IS_ERROR(status) && v) {
        vnode_release(v);
        v = NULL;
    }
    kfree(buffer);
    kfree(joined);
    kfree(target);
    *result = v;
    return status;
}

/*
 * Split a path into its parent directory (resolved) and the last name.
 * The normalized path is written to scratch (VFS_PATH_MAX bytes); name points into it.
 */
static status_t walk_parent(const char *raw, const credentials_t *cred, vnode_t **parent, const char **name,
                            size_t *length, char *scratch)
{
    if (raw[0] != '/' || STATUS_IS_ERROR(vfs_normalize(NULL, raw, strlen(raw), scratch, VFS_PATH_MAX)))
        return STATUS_INVALID_ARGUMENT;
    const char *path = scratch;
    size_t total = strlen(path);
    if (total <= 1)
        return STATUS_INVALID_ARGUMENT; /* "/" has no parent entry */

    size_t slash = total;
    while (slash > 0 && path[slash - 1] != '/')
        slash--;
    *name = path + slash;
    *length = total - slash;

    char *dir = kmalloc(slash + 1);
    if (!dir)
        return STATUS_OUT_OF_MEMORY;
    memcpy(dir, path, slash > 1 ? slash - 1 : 1);
    dir[slash > 1 ? slash - 1 : 1] = '\0';
    status_t status = walk(dir, cred, true, parent);
    kfree(dir);

    if (!STATUS_IS_ERROR(status) && (*parent)->type != VNODE_DIRECTORY) {
        vnode_release(*parent);
        status = STATUS_NOT_DIRECTORY;
    }
    return status;
}

/* --- Path operations ---------------------------------------------------------------- */

status_t vfs_lookup(const char *path, const credentials_t *cred, bool follow, vnode_t **vnode)
{
    mutex_lock(&lock);
    status_t status = walk(path, cred, follow, vnode);
    mutex_unlock(&lock);
    return status;
}

status_t vfs_stat(const char *path, const credentials_t *cred, bool follow, vfs_stat_t *stat)
{
    vnode_t *v;

    mutex_lock(&lock);
    status_t status = walk(path, cred, follow, &v);
    if (!STATUS_IS_ERROR(status)) {
        fill_stat(v, stat);
        vnode_release(v);
    }
    mutex_unlock(&lock);
    return status;
}

static status_t create_locked(const char *path, vnode_type_t type, uint32_t mode, const credentials_t *cred,
                              vnode_t **result)
{
    vnode_t *dir, *v;
    const char *name;
    size_t length;
    char scratch[VFS_PATH_MAX];

    status_t status = walk_parent(path, cred, &dir, &name, &length, scratch);
    if (STATUS_IS_ERROR(status))
        return status;
    if (!permitted(dir, cred, MAY_WRITE | MAY_EXEC))
        status = STATUS_ACCESS_DENIED;
    else if (!dir->ops->create)
        status = STATUS_NOT_SUPPORTED;
    else
        status = dir->ops->create(dir, name, length, type, mode & 0777, &v);

    if (!STATUS_IS_ERROR(status)) {
        v->uid = cred ? cred->uid : UID_ROOT;
        v->gid = cred ? cred->gid : GID_ROOT;
        if (result)
            *result = v;
        else
            vnode_release(v);
    }
    vnode_release(dir);
    return status;
}

status_t vfs_mkdir(const char *path, uint32_t mode, const credentials_t *cred)
{
    mutex_lock(&lock);
    status_t status = create_locked(path, VNODE_DIRECTORY, mode, cred, NULL);
    mutex_unlock(&lock);
    return status;
}

static status_t unlink_locked(vnode_t *dir, const char *name, size_t length)
{
    vnode_t *child;
    status_t status = dir->ops->lookup(dir, name, length, &child);
    if (STATUS_IS_ERROR(status))
        return status;

    bool mount_point = child->mounted != NULL;
    vnode_release(child);
    if (mount_point)
        return STATUS_BUSY;
    return dir->ops->unlink ? dir->ops->unlink(dir, name, length) : STATUS_NOT_SUPPORTED;
}

status_t vfs_unlink(const char *path, const credentials_t *cred)
{
    vnode_t *dir;
    const char *name;
    size_t length;
    char scratch[VFS_PATH_MAX];

    mutex_lock(&lock);
    status_t status = walk_parent(path, cred, &dir, &name, &length, scratch);
    if (!STATUS_IS_ERROR(status)) {
        status = permitted(dir, cred, MAY_WRITE | MAY_EXEC) ? unlink_locked(dir, name, length) : STATUS_ACCESS_DENIED;
        vnode_release(dir);
    }
    mutex_unlock(&lock);
    return status;
}

status_t vfs_rename(const char *from_raw, const char *to_raw, const credentials_t *cred)
{
    vnode_t *from_dir = NULL, *to_dir = NULL, *source = NULL, *existing = NULL;
    const char *from_name, *to_name;
    size_t from_length, to_length;
    char from[VFS_PATH_MAX], to[VFS_PATH_MAX];

    mutex_lock(&lock);
    status_t status = walk_parent(from_raw, cred, &from_dir, &from_name, &from_length, from);
    if (!STATUS_IS_ERROR(status))
        status = walk_parent(to_raw, cred, &to_dir, &to_name, &to_length, to);

    size_t n = strlen(from);
    if (!STATUS_IS_ERROR(status) && strcmp(from, to) == 0) {
        vnode_release(from_dir);
        vnode_release(to_dir);
        mutex_unlock(&lock);
        return STATUS_SUCCESS;
    }
    if (!STATUS_IS_ERROR(status) && strncmp(from, to, n) == 0 && to[n] == '/')
        status = STATUS_INVALID_ARGUMENT; /* a directory cannot move into itself */
    if (!STATUS_IS_ERROR(status) && (!permitted(from_dir, cred, MAY_WRITE | MAY_EXEC) ||
                                     !permitted(to_dir, cred, MAY_WRITE | MAY_EXEC)))
        status = STATUS_ACCESS_DENIED;
    if (!STATUS_IS_ERROR(status) && (from_dir->fs != to_dir->fs || !from_dir->ops->rename))
        status = STATUS_NOT_SUPPORTED; /* no moves across file systems */
    if (!STATUS_IS_ERROR(status))
        status = from_dir->ops->lookup(from_dir, from_name, from_length, &source);
    if (!STATUS_IS_ERROR(status) && source->mounted)
        status = STATUS_BUSY;

    /* An existing target of the same kind is replaced. */
    if (!STATUS_IS_ERROR(status) && to_dir->ops->lookup(to_dir, to_name, to_length, &existing) == STATUS_SUCCESS) {
        if (existing->mounted)
            status = STATUS_BUSY;
        else if (existing->type == VNODE_DIRECTORY && source->type != VNODE_DIRECTORY)
            status = STATUS_IS_DIRECTORY;
        else if (existing->type != VNODE_DIRECTORY && source->type == VNODE_DIRECTORY)
            status = STATUS_NOT_DIRECTORY;
        vnode_release(existing);
        if (!STATUS_IS_ERROR(status))
            status = to_dir->ops->unlink(to_dir, to_name, to_length);
    }
    if (source)
        vnode_release(source);
    if (!STATUS_IS_ERROR(status))
        status = from_dir->ops->rename(from_dir, from_name, from_length, to_dir, to_name, to_length);

    if (from_dir)
        vnode_release(from_dir);
    if (to_dir)
        vnode_release(to_dir);
    mutex_unlock(&lock);
    return status;
}

status_t vfs_symlink(const char *target, const char *path, const credentials_t *cred)
{
    vnode_t *dir;
    const char *name;
    size_t length, target_length = strlen(target);

    char scratch[VFS_PATH_MAX];

    if (target_length == 0 || target_length >= VFS_PATH_MAX)
        return STATUS_INVALID_ARGUMENT;

    mutex_lock(&lock);
    status_t status = walk_parent(path, cred, &dir, &name, &length, scratch);
    if (!STATUS_IS_ERROR(status)) {
        if (!permitted(dir, cred, MAY_WRITE | MAY_EXEC))
            status = STATUS_ACCESS_DENIED;
        else if (!dir->ops->symlink)
            status = STATUS_NOT_SUPPORTED;
        else
            status = dir->ops->symlink(dir, name, length, target, target_length);
        vnode_release(dir);
    }
    mutex_unlock(&lock);
    return status;
}

status_t vfs_readlink(const char *path, const credentials_t *cred, char *buffer, size_t size, size_t *length)
{
    vnode_t *v;

    mutex_lock(&lock);
    status_t status = walk(path, cred, false, &v);
    if (!STATUS_IS_ERROR(status)) {
        status = v->type != VNODE_SYMLINK ? STATUS_INVALID_ARGUMENT : v->ops->readlink(v, buffer, size, length);
        vnode_release(v);
    }
    mutex_unlock(&lock);
    return status;
}

/* --- Files ------------------------------------------------------------------------ */

static void file_destroy(object_t *object)
{
    file_t *file = container_of(object, file_t, object);
    vnode_t *v = file->vnode;

    if (v->ops->close)
        v->ops->close(v, file->flags); /* pipes count their readers and writers */
    mutex_lock(&lock);
    vnode_release(v);
    mutex_unlock(&lock);
    kfree(file);
}

static const object_ops_t file_ops = {
    .destroy = file_destroy,
};

status_t vfs_open(const char *path, uint32_t flags, uint32_t mode, const credentials_t *cred, file_t **result)
{
    const uint32_t known = JELLY_OPEN_READ | JELLY_OPEN_WRITE | JELLY_OPEN_CREATE | JELLY_OPEN_EXCLUSIVE |
                           JELLY_OPEN_TRUNCATE | JELLY_OPEN_APPEND | JELLY_OPEN_DIRECTORY | JELLY_OPEN_NOFOLLOW |
                           VFS_OPEN_EXEC;
    vnode_t *v = NULL;
    bool created = false;

    if ((flags & ~known) || !(flags & (JELLY_OPEN_READ | JELLY_OPEN_WRITE)))
        return STATUS_INVALID_ARGUMENT;

    file_t *file = kcalloc(1, sizeof(*file));
    if (!file)
        return STATUS_OUT_OF_MEMORY;

    mutex_lock(&lock);
    status_t status = walk(path, cred, !(flags & JELLY_OPEN_NOFOLLOW), &v);
    if (status == STATUS_NOT_FOUND && (flags & JELLY_OPEN_CREATE)) {
        status = create_locked(path, VNODE_FILE, mode, cred, &v);
        created = !STATUS_IS_ERROR(status);
    } else if (!STATUS_IS_ERROR(status) && (flags & JELLY_OPEN_CREATE) && (flags & JELLY_OPEN_EXCLUSIVE)) {
        status = STATUS_ALREADY_EXISTS;
    }

    if (!STATUS_IS_ERROR(status)) {
        if (v->type == VNODE_SYMLINK)
            status = STATUS_INVALID_ARGUMENT;
        else if ((flags & JELLY_OPEN_DIRECTORY) && v->type != VNODE_DIRECTORY)
            status = STATUS_NOT_DIRECTORY;
        else if (v->type == VNODE_DIRECTORY && (flags & JELLY_OPEN_WRITE))
            status = STATUS_IS_DIRECTORY;
        else if (!created && (((flags & JELLY_OPEN_READ) && !permitted(v, cred, MAY_READ)) ||
                              ((flags & JELLY_OPEN_WRITE) && !permitted(v, cred, MAY_WRITE))))
            status = STATUS_ACCESS_DENIED;
        else if ((flags & VFS_OPEN_EXEC) && (v->type != VNODE_FILE || !(v->mode & 0111) ||
                                             !permitted(v, cred, MAY_EXEC)))
            status = STATUS_ACCESS_DENIED; /* even root needs some x bit, as on Unix */
    }
    if (!STATUS_IS_ERROR(status) && (flags & JELLY_OPEN_TRUNCATE) && (flags & JELLY_OPEN_WRITE) && v->size &&
        !vnode_is_stream(v))
        status = v->ops->truncate ? v->ops->truncate(v, 0) : STATUS_NOT_SUPPORTED;

    if (STATUS_IS_ERROR(status)) {
        if (v)
            vnode_release(v);
        mutex_unlock(&lock);
        kfree(file);
        return status;
    }
    mutex_unlock(&lock);

    object_init(&file->object, OBJECT_FILE, &file_ops);
    file->vnode = v;
    file->flags = flags;
    *result = file;
    return STATUS_SUCCESS;
}

status_t vfs_read(file_t *file, void *buffer, size_t size, size_t *done)
{
    *done = 0;
    if (!(file->flags & JELLY_OPEN_READ))
        return STATUS_ACCESS_DENIED;
    if (file->vnode->type == VNODE_DIRECTORY)
        return STATUS_IS_DIRECTORY;
    if (vnode_is_stream(file->vnode))
        return file->vnode->ops->read(file->vnode, 0, buffer, size, done);

    mutex_lock(&lock);
    status_t status = file->vnode->ops->read(file->vnode, file->position, buffer, size, done);
    file->position += *done;
    mutex_unlock(&lock);
    return status;
}

status_t vfs_write(file_t *file, const void *buffer, size_t size, size_t *done)
{
    *done = 0;
    if (!(file->flags & JELLY_OPEN_WRITE))
        return STATUS_ACCESS_DENIED;
    if (!file->vnode->ops->write)
        return STATUS_NOT_SUPPORTED;
    if (vnode_is_stream(file->vnode))
        return file->vnode->ops->write(file->vnode, 0, buffer, size, done);

    mutex_lock(&lock);
    if (file->flags & JELLY_OPEN_APPEND)
        file->position = file->vnode->size;
    status_t status = file->vnode->ops->write(file->vnode, file->position, buffer, size, done);
    file->position += *done;
    mutex_unlock(&lock);
    return status;
}

status_t vfs_seek(file_t *file, int64_t offset, uint32_t whence, uint64_t *position)
{
    int64_t base;

    if (vnode_is_stream(file->vnode))
        return STATUS_NOT_SUPPORTED;
    mutex_lock(&lock);
    switch (whence) {
    case JELLY_SEEK_SET:     base = 0; break;
    case JELLY_SEEK_CURRENT: base = (int64_t)file->position; break;
    case JELLY_SEEK_END:     base = (int64_t)file->vnode->size; break;
    default:
        mutex_unlock(&lock);
        return STATUS_INVALID_ARGUMENT;
    }
    status_t status = STATUS_SUCCESS;
    if (base + offset < 0)
        status = STATUS_INVALID_ARGUMENT;
    else
        file->position = (uint64_t)(base + offset);
    *position = file->position;
    mutex_unlock(&lock);
    return status;
}

status_t vfs_truncate(file_t *file, uint64_t size)
{
    if (!(file->flags & JELLY_OPEN_WRITE))
        return STATUS_ACCESS_DENIED;
    if (!file->vnode->ops->truncate)
        return STATUS_NOT_SUPPORTED;

    mutex_lock(&lock);
    status_t status = file->vnode->ops->truncate(file->vnode, size);
    mutex_unlock(&lock);
    return status;
}

status_t vfs_fstat(file_t *file, vfs_stat_t *stat)
{
    mutex_lock(&lock);
    fill_stat(file->vnode, stat);
    mutex_unlock(&lock);
    return STATUS_SUCCESS;
}

static bool is_dot(const vfs_dirent_t *e)
{
    return (e->name_length == 1 && e->name[0] == '.') ||
           (e->name_length == 2 && e->name[0] == '.' && e->name[1] == '.');
}

status_t vfs_readdir(file_t *file, vfs_dirent_t *entry)
{
    if (file->vnode->type != VNODE_DIRECTORY)
        return STATUS_NOT_DIRECTORY;
    if (!(file->flags & JELLY_OPEN_READ))
        return STATUS_ACCESS_DENIED;

    mutex_lock(&lock);
    status_t status;
    do {
        status = file->vnode->ops->readdir(file->vnode, &file->dir_cookie, entry);
    } while (status == STATUS_SUCCESS && is_dot(entry));
    mutex_unlock(&lock);
    return status;
}

status_t vfs_file_from_vnode(vnode_t *vnode, uint32_t flags, file_t **result)
{
    file_t *file = kcalloc(1, sizeof(*file));
    if (!file)
        return STATUS_OUT_OF_MEMORY;
    object_init(&file->object, OBJECT_FILE, &file_ops);
    vnode_retain(vnode);
    file->vnode = vnode;
    file->flags = flags;
    *result = file;
    return STATUS_SUCCESS;
}

status_t vfs_set_attributes(const char *path, uint32_t uid, uint32_t gid, uint32_t mode)
{
    vnode_t *v;

    mutex_lock(&lock);
    status_t status = walk(path, NULL, false, &v);
    if (!STATUS_IS_ERROR(status)) {
        v->uid = uid;
        v->gid = gid;
        v->mode = mode & 0777;
        vnode_release(v);
    }
    mutex_unlock(&lock);
    return status;
}

/* --- Mounts -------------------------------------------------------------------------- */

status_t vfs_register_type(fs_type_t *type)
{
    mutex_lock(&lock);
    list_push_back(&types, &type->node);
    mutex_unlock(&lock);
    return STATUS_SUCCESS;
}

static status_t attach(const char *path, filesystem_t *fs)
{
    vnode_t *dir;
    status_t status = walk(path, NULL, true, &dir);

    if (STATUS_IS_ERROR(status))
        return status;
    if (dir->type != VNODE_DIRECTORY || dir->mounted || dir == root_fs->root) {
        status = dir->type != VNODE_DIRECTORY ? STATUS_NOT_DIRECTORY : STATUS_BUSY;
        vnode_release(dir);
        return status;
    }

    mount_t *m = kcalloc(1, sizeof(*m));
    if (!m) {
        vnode_release(dir);
        return STATUS_OUT_OF_MEMORY;
    }
    memcpy(m->path, path, strlen(path) + 1);
    m->fs = fs;
    m->covered = dir; /* keeps the mount point alive */
    dir->mounted = m;
    list_push_back(&mounts, &m->node);
    return STATUS_SUCCESS;
}

static status_t mount_locked(const char *path, block_device_t *device, const char *type_name)
{
    list_for_each(node, &types) {
        fs_type_t *type = container_of(node, fs_type_t, node);
        filesystem_t *fs;

        if (type_name && strcmp(type->name, type_name) != 0)
            continue;
        status_t status = type->mount(device, &fs);
        if (status == STATUS_NOT_SUPPORTED)
            continue;
        if (STATUS_IS_ERROR(status))
            return status;

        fs->type = type;
        fs->device = device;
        status = attach(path, fs);
        if (STATUS_IS_ERROR(status)) {
            type->unmount(fs);
            return status;
        }
        klog_info("vfs: %s (%s) mounted at %s", device ? device->name : "none", type->name, path);
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}

status_t vfs_mount(const char *raw, block_device_t *device, const char *type_name)
{
    char path[VFS_PATH_MAX];

    if (raw[0] != '/' || STATUS_IS_ERROR(vfs_normalize(NULL, raw, strlen(raw), path, sizeof(path))))
        return STATUS_INVALID_ARGUMENT;
    mutex_lock(&lock);
    status_t status = mount_locked(path, device, type_name);
    mutex_unlock(&lock);
    return status;
}

status_t vfs_unmount(const char *raw)
{
    char path[VFS_PATH_MAX];

    if (raw[0] != '/' || STATUS_IS_ERROR(vfs_normalize(NULL, raw, strlen(raw), path, sizeof(path))))
        return STATUS_INVALID_ARGUMENT;
    mutex_lock(&lock);
    status_t status = STATUS_NOT_FOUND;

    list_for_each(node, &mounts) {
        mount_t *m = container_of(node, mount_t, node);
        if (strcmp(m->path, path) != 0)
            continue;

        filesystem_t *fs = m->fs;
        if (fs->live_vnodes > 1 || fs->root->refs > 1) {
            status = STATUS_BUSY; /* open files, working directories or nested mounts */
            break;
        }
        if (fs->type->sync)
            fs->type->sync(fs);
        status = fs->type->unmount(fs);
        if (STATUS_IS_ERROR(status))
            break;
        m->covered->mounted = NULL;
        vnode_release(m->covered);
        list_remove(node);
        klog_info("vfs: %s unmounted", m->path);
        kfree(m);
        break;
    }
    mutex_unlock(&lock);
    return status;
}

status_t vfs_sync(void)
{
    status_t result = STATUS_SUCCESS;

    mutex_lock(&lock);
    list_for_each(node, &mounts) {
        filesystem_t *fs = container_of(node, mount_t, node)->fs;
        status_t status = fs->type->sync ? fs->type->sync(fs) : STATUS_SUCCESS;
        if (!STATUS_IS_ERROR(status) && fs->device)
            status = block_flush(fs->device);
        if (STATUS_IS_ERROR(status))
            result = status;
    }
    mutex_unlock(&lock);
    return result;
}

void vfs_automount(block_device_t *device)
{
    char path[64];
    format(path, sizeof(path), "/volumes/%s", device->name);

    mutex_lock(&lock);
    status_t status = create_locked(path, VNODE_DIRECTORY, 0755, NULL, NULL);
    if (!STATUS_IS_ERROR(status)) {
        status = mount_locked(path, device, NULL);
        if (STATUS_IS_ERROR(status)) {
            vnode_t *volumes;
            if (walk("/volumes", NULL, true, &volumes) == STATUS_SUCCESS) {
                unlink_locked(volumes, device->name, strlen(device->name));
                vnode_release(volumes);
            }
            if (status == STATUS_NOT_SUPPORTED)
                klog_info("vfs: no known file system on %s", device->name);
            else
                klog_error("vfs: mounting %s failed: %s", device->name, status_name(status));
        }
    }
    mutex_unlock(&lock);
}

status_t vfs_init(void)
{
    mutex_init(&lock);
    root_fs = ramfs_create();
    if (!root_fs)
        return STATUS_OUT_OF_MEMORY;

    vfs_register_type(&fat_type);
    vfs_register_type(&exfat_type);
    vfs_register_type(&ramfs_type);
    vfs_register_type(&devfs_type);
    vfs_mkdir("/volumes", 0755, NULL);
    vfs_mkdir("/tmp", 0777, NULL);
    vfs_mkdir("/dev", 0755, NULL);

    status_t status = vfs_mount("/dev", NULL, "devfs");
    if (STATUS_IS_ERROR(status))
        return status;
    klog_info("vfs: ramfs root with /volumes, /tmp and /dev (devfs)");
    return STATUS_SUCCESS;
}
