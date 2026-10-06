/*
 * JellyOS Boot Manager - file access on the boot volume.
 */

#ifndef BOOT_FILE_H
#define BOOT_FILE_H

#include "boot.h"

/*
 * Read a whole file from the volume the boot manager was loaded from.
 * On success *data is pool memory owned by the caller (FreePool).
 */
EFI_STATUS file_read_all(EFI_HANDLE image, const CHAR16 *path, void **data, UINTN *size);

#endif
