# JellyOS Storage Stack and File Systems

**Code:** [`fs/block/`](../../fs/block/), [`fs/partition/`](../../fs/partition/), [`fs/vfs/`](../../fs/vfs/), [`fs/filesystems/`](../../fs/filesystems/), [`drivers/bus/virtio/`](../../drivers/bus/virtio/), [`drivers/storage/`](../../drivers/storage/), file system calls in [`kernel/syscall/fs_syscalls.c`](../../kernel/syscall/fs_syscalls.c)
**ABI:** [../abi/syscalls.md](../abi/syscalls.md) (version 2)

## Layers (README section 25)

```text
VirtIO block (NVMe, AHCI, USB later)   drivers/storage, drivers/bus/virtio
            │  block_ops_t
            ▼
      Block Device API                  fs/block
            ▼
     Partition layer (GPT / MBR)        fs/partition
            ▼
   File system (FAT32, ramfs)           fs/filesystems
            ▼
            VFS                         fs/vfs
            ▼
   File handles / system calls          kernel/syscall/fs_syscalls.c
```

Each layer only uses the one below it. File systems see block devices, never
drivers. The VFS sees vnodes, never clusters.

## Block devices

A block device has a name, sector size, sector count, a read-only flag and
`read` / `write` / `flush` operations. `block_read()` and `block_write()`
check ranges. Partition devices forward I/O to their parent with an LBA
offset.

`block_register()` makes a device known. A whole disk is scanned for
partitions first; a disk with partitions is not mounted itself. Every other
device is offered to the file systems (`vfs_automount`).

### Partitions

| Scheme | Handling |
|---|---|
| GPT | Protective MBR, primary header with CRC (backup header as fallback), entry array CRC; type and unique GUID per partition |
| MBR | Four primary entries (extended partitions are not followed) |

Partitions are named `<disk>p<number>`, for example `virtio0p1`.

### VirtIO block driver

`virtio_blk` is a built-in module. It uses the VirtIO 1.x PCI transport
(`drivers/bus/virtio`, shared with future VirtIO devices):

- Feature negotiation with `VERSION_1` required, plus `RO`, `FLUSH` and `BLK_SIZE`.
- One split virtqueue (up to 128 entries) whose completion interrupt arrives via MSI-X.
- Requests are serialized by a mutex. Each one is a 3-descriptor chain
  (header, data, status). Data goes through a 64 KiB DMA bounce buffer, so
  callers may pass any kernel buffer.

Not yet included: multiple requests in flight, multiqueue, discard.

## VFS (README section 26)

| Concept | Implementation |
|---|---|
| Vnode | Type (file, directory, symbolic link), mode, uid/gid, size, inode number, operations; reference counted |
| Operations | lookup, create, read, write, truncate, unlink, rename, readdir, symlink, readlink, release |
| Mount points | A directory vnode points to the mounted file system; walks cross into its root. Unmount refuses while vnodes of the file system are referenced (`BUSY`) |
| Paths | Absolute and normalized at every entry point. `.` and `..` are resolved lexically, as in Plan 9 and Go's `path.Clean` |
| Symbolic links | Followed during the walk (relative targets against the link's directory), at most 8 per lookup (`LIMIT_EXCEEDED`) |
| Permissions | Owner/group/other `rwx` bits against the caller's credentials. Root bypasses them. Search permission is needed on every directory, write+search on the parent to create, remove or rename |
| Open files | `file_t` kernel objects behind file handles: position, open flags, directory cursor |
| Locking | One VFS mutex (sleeping lock; I/O may block). Finer locking comes with SMP |

Rules for operations:

- `rename` replaces an existing target of the same kind and refuses moving a
  directory into itself or across file systems (`NOT_SUPPORTED`).
- `unlink` removes files, symbolic links and empty directories (`NOT_EMPTY`
  otherwise). Mount points are `BUSY`.
- `readdir` never returns `.` and `..`.

### Mount layout at boot

| Path | Contents |
|---|---|
| `/` | ramfs (root file system) |
| `/tmp` | ramfs directory, mode 0777 |
| `/volumes/<device>` | Every partition or disk with a recognized file system, mounted automatically |

The automatic mounts under `/volumes` are an interim policy. With Phase 7,
init decides what is mounted where, using `SYS_MOUNT` / `SYS_UNMOUNT`.

## File systems

### ramfs

ramfs lives entirely in memory and supports everything the VFS offers,
including symbolic links and owners. It is the root file system and will hold
the initramfs (Phase 7).

### FAT32

FAT32 supports reading and writing with long file names (VFAT):

- Reads volumes from any FAT32 formatter. The tests use mtools' `mformat`.
- Writes are write-through: clusters, both FAT copies and directory entries
  go straight to the device. The FSInfo free count is marked unknown on the
  first allocation.
- Creating, extending, truncating (including holes, which read as zeros),
  renaming (moved directories get a new `..`) and deleting.
- Short names follow the Windows `NAME~N.EXT` scheme. Long-name entries are
  always written, case is preserved and lookups ignore case.
- Live vnodes are cached by the position of their directory entry, so all
  users of a file see the same size and clusters. Deleting an open file is
  refused (`BUSY`) because its clusters must not be reused.
- FAT has no owners or permission bits: everything belongs to root, files are
  0644 (0444 with the read-only attribute) and directories 0755.

Not supported: FAT12/16, symbolic links (`NOT_SUPPORTED`), timestamps (a fixed
date until there is a wall clock), a block cache.

## Process side

Every process has a working directory, stored as a normalized path string
(`SYS_CHDIR` / `SYS_GETCWD`), and relative paths are resolved against it. A
working directory does not pin a mount, so it can become stale if the volume
is unmounted.

## Disk images

[`tools/image_builder/mkdisk.sh`](../../tools/image_builder/mkdisk.sh)
builds a GPT image with one FAT32 partition at 1 MiB (one sector per cluster)
and copies a directory into it (`sgdisk` + `mtools`).

`make run DISK=<image>` attaches an image as a VirtIO disk.
