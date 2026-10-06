/*
 * Initramfs unpacking (cpio "newc") into the root file system.
 */

#ifndef FS_INITRAMFS_INITRAMFS_H
#define FS_INITRAMFS_INITRAMFS_H

#include <jelly/status.h>
#include <stddef.h>

status_t initramfs_unpack(const void *archive, size_t size, unsigned *entries);

#endif
