/*
 * Virtual file system (README section 26).
 *
 * File systems provide vnodes (files, directories, symbolic links) with a
 * common set of operations. The VFS resolves paths across mount points,
 * follows symbolic links, checks permissions against the caller's
 * credentials and hands out file objects that userspace reaches through
 * handles. See docs/architecture/storage.md.
 *
 * Paths given to the VFS are absolute. "." and ".." are resolved lexically
 * (as in Plan 9 or Go's path.Clean) before the walk.
 */

#ifndef FS_VFS_VFS_H
#define FS_VFS_VFS_H

#include "core/list.h"
#include "core/object.h"
#include "fs/block/block.h"
#include "security/credentials.h"

#include <jelly/syscall.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VFS_NAME_MAX      255
#define VFS_PATH_MAX      1024
#define VFS_SYMLINK_DEPTH 8

typedef enum {
    VNODE_FILE      = JELLY_FILE_TYPE_FILE,
    VNODE_DIRECTORY = JELLY_FILE_TYPE_DIRECTORY,
    VNODE_SYMLINK   = JELLY_FILE_TYPE_SYMLINK,
    VNODE_DEVICE    = JELLY_FILE_TYPE_DEVICE, /* stream: no position, own locking */
    VNODE_PIPE      = JELLY_FILE_TYPE_PIPE,   /* stream: no position, own locking */
} vnode_type_t;

typedef jelly_stat_t   vfs_stat_t;
typedef jelly_dirent_t vfs_dirent_t;

struct vnode;
struct filesystem;
struct mount;

/*
 * Operations a file system implements. Names are not NUL-terminated.
 * lookup/create return a new reference. Unsupported operations may be NULL.
 */
typedef struct {
    status_t (*lookup)(struct vnode *dir, const char *name, size_t length, struct vnode **result);
    status_t (*create)(struct vnode *dir, const char *name, size_t length, vnode_type_t type, uint32_t mode,
                       struct vnode **result);
    status_t (*read)(struct vnode *file, uint64_t offset, void *buffer, size_t size, size_t *done);
    status_t (*write)(struct vnode *file, uint64_t offset, const void *buffer, size_t size, size_t *done);
    status_t (*truncate)(struct vnode *file, uint64_t size);
    status_t (*unlink)(struct vnode *dir, const char *name, size_t length);
    status_t (*rename)(struct vnode *from_dir, const char *from, size_t from_length,
                       struct vnode *to_dir, const char *to, size_t to_length);
    status_t (*readdir)(struct vnode *dir, uint64_t *cookie, vfs_dirent_t *entry); /* NOT_FOUND at the end */
    status_t (*symlink)(struct vnode *dir, const char *name, size_t length, const char *target, size_t target_length);
    status_t (*readlink)(struct vnode *link, char *buffer, size_t size, size_t *length);
    void     (*close)(struct vnode *vnode, uint32_t open_flags); /* an open file on it was closed */
    void     (*release)(struct vnode *vnode); /* last reference gone */
} vnode_ops_t;

typedef struct vnode {
    uint32_t           refs;
    vnode_type_t       type;
    uint32_t           mode;      /* permission bits (0777) */
    uint32_t           uid;
    uint32_t           gid;
    uint64_t           size;
    uint64_t           inode;
    struct filesystem *fs;
    const vnode_ops_t *ops;
    void              *data;      /* file system private */
    struct mount      *mounted;   /* file system mounted on this directory */
} vnode_t;

/*
 * Devices and pipes are streams: reads and writes may block for a long time,
 * so the VFS calls them without its lock and without a file position.
 */
static inline int vnode_is_stream(const vnode_t *vnode)
{
    return vnode->type == VNODE_DEVICE || vnode->type == VNODE_PIPE;
}

typedef struct filesystem {
    const struct fs_type *type;
    vnode_t              *root;
    block_device_t       *device;
    void                 *data;
    uint32_t              live_vnodes; /* referenced vnodes besides the root (unmount check) */
} filesystem_t;

typedef struct fs_type {
    const char *name;
    /* Probe the device; NOT_SUPPORTED if it does not hold this file system. */
    status_t  (*mount)(block_device_t *device, filesystem_t **fs);
    status_t  (*unmount)(filesystem_t *fs);
    status_t  (*sync)(filesystem_t *fs);
    list_node_t node;
} fs_type_t;

typedef struct mount {
    char          path[VFS_PATH_MAX];
    filesystem_t *fs;
    vnode_t      *covered;
    list_node_t   node;
} mount_t;

/* Open file (kernel object behind a file handle). */
typedef struct file {
    object_t object;
    vnode_t *vnode;
    uint64_t position;
    uint32_t flags;      /* JELLY_OPEN_* */
    uint64_t dir_cookie;
} file_t;

/* --- Setup and mounts --------------------------------------------------------- */

status_t vfs_init(void);
status_t vfs_register_type(fs_type_t *type);

/* Offer a new block device to all file system types and mount it under /volumes/<name>. */
void     vfs_automount(block_device_t *device);

/* device may be NULL for virtual file systems ("ramfs", "devfs"). */
status_t vfs_mount(const char *path, block_device_t *device, const char *type_name);
status_t vfs_unmount(const char *path);
status_t vfs_sync(void);

/* --- Paths --------------------------------------------------------------------- */

/* Join base (absolute) and path, resolve "." / ".." lexically, collapse "//". */
status_t vfs_normalize(const char *base, const char *path, size_t length, char *out, size_t capacity);

status_t vfs_lookup(const char *path, const credentials_t *cred, bool follow, vnode_t **vnode);
status_t vfs_stat(const char *path, const credentials_t *cred, bool follow, vfs_stat_t *stat);
status_t vfs_mkdir(const char *path, uint32_t mode, const credentials_t *cred);
status_t vfs_unlink(const char *path, const credentials_t *cred);
status_t vfs_rename(const char *from, const char *to, const credentials_t *cred);
status_t vfs_symlink(const char *target, const char *path, const credentials_t *cred);
status_t vfs_readlink(const char *path, const credentials_t *cred, char *buffer, size_t size, size_t *length);

/* --- Files ---------------------------------------------------------------------- */

status_t vfs_open(const char *path, uint32_t flags, uint32_t mode, const credentials_t *cred, file_t **file);
status_t vfs_read(file_t *file, void *buffer, size_t size, size_t *done);
status_t vfs_write(file_t *file, const void *buffer, size_t size, size_t *done);
status_t vfs_seek(file_t *file, int64_t offset, uint32_t whence, uint64_t *position);
status_t vfs_truncate(file_t *file, uint64_t size);
status_t vfs_fstat(file_t *file, vfs_stat_t *stat);
status_t vfs_readdir(file_t *file, vfs_dirent_t *entry);

/* Open file on a vnode that has no path (pipes); takes a new reference. */
status_t vfs_file_from_vnode(vnode_t *vnode, uint32_t flags, file_t **file);

/* Kernel-internal open flag: the caller wants to execute the file (needs x permission). */
#define VFS_OPEN_EXEC (1u << 31)

/* Set owner and permission bits (kernel use: initramfs unpacking). */
status_t vfs_set_attributes(const char *path, uint32_t uid, uint32_t gid, uint32_t mode);

/* --- Helpers for file systems ----------------------------------------------------- */

/* Initialize a vnode embedded in a file system node (one reference). */
void     vnode_init(vnode_t *vnode, filesystem_t *fs, vnode_type_t type, const vnode_ops_t *ops);
void     vnode_retain(vnode_t *vnode);
void     vnode_release(vnode_t *vnode);

/* In-memory file system (fs/filesystems/ramfs). */
filesystem_t *ramfs_create(void);
extern fs_type_t ramfs_type;

/* Device file system (fs/filesystems/devfs), mounted at /dev. */
extern fs_type_t devfs_type;
status_t devfs_register(const char *name, const vnode_ops_t *ops, uint32_t mode, void *data);

#endif
