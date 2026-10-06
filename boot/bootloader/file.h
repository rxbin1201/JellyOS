/*
 * JellyOS Boot Manager - file access on the boot volume.
 */

#ifndef BOOT_FILE_H
#define BOOT_FILE_H

#include "boot.h"

/*
 * Read a whole file from the volume the boot manager was loaded from.
 * On success *data is pool memory owned by the caller (FreePool), with a
 * terminating NUL byte after the content.
 */
EFI_STATUS file_read_all(EFI_HANDLE image, const CHAR16 *path, void **data, UINTN *size);

/* Read a whole file into tracked, page-aligned memory of the given type. */
EFI_STATUS file_load_pages(EFI_HANDLE image, const CHAR16 *path, EFI_MEMORY_TYPE type,
                           void **data, UINTN *size);

#endif
